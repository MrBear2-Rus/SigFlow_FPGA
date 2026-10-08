#include "YosysScriptGenerator.h"

#include "eda-platform/Platform.h"

#include <cctype>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace eda {
namespace synth {
namespace {

const StrategyInfo kBaselineStrategy{"baseline", "1.0", "Baseline"};
const StrategyInfo kDebugStrategy{"debug", "1.0", "Debug"};
const StrategyInfo kResourceOptimizedStrategy{"resource_optimized", "1.0", "Resource optimized"};

bool IsValidVerilogIdentifier(const std::string& value) {
    if (value.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(value[0]);
    if (!(std::isalpha(first) || value[0] == '_')) return false;
    for (char c : value) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (!(std::isalnum(ch) || c == '_' || c == '$')) return false;
    }
    return true;
}

std::string ToLower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string WindowsAnsiPath(const std::filesystem::path& path) {
#if defined(_WIN32)
    // 随附的 MinGW Yosys 0.49 使用窄字符文件 I/O。它把脚本中的字节按系统 ANSI
    // 代码页传给 fopen，而不是按 UTF-8；故不能把 PathToUtf8 的结果直接写入脚本。
    // 从同一 UTF-16 path 转成 ACP，避免同一个目录被错误地重解释为另一条路径。
    const std::wstring native = path.native();
    if (native.empty()) return {};
    BOOL usedDefault = FALSE;
    const int required = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                                             native.data(), static_cast<int>(native.size()),
                                             nullptr, 0, nullptr, &usedDefault);
    if (required <= 0 || usedDefault) return {};
    std::string encoded(static_cast<std::size_t>(required), '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                            native.data(), static_cast<int>(native.size()), encoded.data(),
                            required, nullptr, &usedDefault) != required || usedDefault) {
        return {};
    }
    return encoded;
#else
    return platform::PathToUtf8(path);
#endif
}

bool NormalizeYosysPath(const std::string& path, std::string& normalizedPath,
                        std::string& errorMessage) {
    if (path.empty()) {
        errorMessage = "A Yosys file path must not be empty.";
        return false;
    }
    if (path.find_first_of("\r\n\"") != std::string::npos) {
        errorMessage = "A Yosys file path contains a forbidden control or quote character.";
        return false;
    }
    std::error_code error;
    // 入参是 UTF-8 字符串（来自 Gateway 的受控快照路径）。必须先还原为 path，
    // 否则 Windows 上每个字节会被当成一个宽字符，中文路径会双重编码而失效。
    const std::filesystem::path absolute =
        std::filesystem::absolute(platform::PathFromUtf8(path), error);
    if (error) {
        errorMessage = "Unable to normalize Yosys file path: " + path;
        return false;
    }
    const std::string generic = WindowsAnsiPath(absolute.lexically_normal());
    if (generic.empty()) {
        errorMessage = "Yosys cannot represent this path in the Windows system code page: " +
                       path;
        return false;
    }
    normalizedPath = generic;
    return true;
}

bool ValidateRequest(const YosysScriptRequest& request, std::string& errorMessage) {
    if (request.sourceFiles.empty()) {
        errorMessage = "At least one RTL source file is required.";
        return false;
    }
    if (!IsValidVerilogIdentifier(request.topModule)) {
        errorMessage = "The top module is not a valid Verilog identifier.";
        return false;
    }
    if (!IsValidVerilogIdentifier(request.yosysFamily)) {
        errorMessage = "The target profile has an invalid Yosys family.";
        return false;
    }
    return true;
}

} // namespace

const StrategyInfo& GetStrategyInfo(YosysStrategy strategy) {
    switch (strategy) {
        case YosysStrategy::Baseline:          return kBaselineStrategy;
        case YosysStrategy::Debug:             return kDebugStrategy;
        case YosysStrategy::ResourceOptimized: return kResourceOptimizedStrategy;
    }
    return kBaselineStrategy;
}

bool ParseStrategy(const std::string& value, YosysStrategy& strategy) {
    if (value == kBaselineStrategy.id) {
        strategy = YosysStrategy::Baseline;
        return true;
    }
    if (value == kDebugStrategy.id) {
        strategy = YosysStrategy::Debug;
        return true;
    }
    if (value == kResourceOptimizedStrategy.id) {
        strategy = YosysStrategy::ResourceOptimized;
        return true;
    }
    return false;
}

YosysScriptResult YosysScriptGenerator::Generate(const YosysScriptRequest& request) const {
    YosysScriptResult result;
    if (!ValidateRequest(request, result.errorMessage)) {
        return result;
    }

    std::string outputPath;
    if (!NormalizeYosysPath(request.outputJsonPath, outputPath, result.errorMessage)) {
        return result;
    }

    const StrategyInfo& strategyInfo = GetStrategyInfo(request.strategy);
    std::string script = "# Generated by SigFlow.\n";
    script += "# Strategy: " + strategyInfo.id + "@" + strategyInfo.version + "\n";
    script += "# Target profile: " + request.targetProfileId + "@" +
              request.targetProfileVersion + "\n";

    for (const std::string& sourceFile : request.sourceFiles) {
        std::string sourcePath;
        if (!NormalizeYosysPath(sourceFile, sourcePath, result.errorMessage)) {
            return result;
        }
        const std::string lower = ToLower(sourceFile);
        script += "read_verilog";
        if (lower.size() >= 3 && lower.compare(lower.size() - 3, 3, ".sv") == 0) {
            script += " -sv";
        }
        script += " \"" + sourcePath + "\"\n";
    }

    script += "hierarchy -check -top " + request.topModule + "\n";
    switch (request.strategy) {
        case YosysStrategy::Baseline:
            break;
        case YosysStrategy::Debug:
            script += "check\n";
            break;
        case YosysStrategy::ResourceOptimized:
            script += "opt_clean\n";
            break;
    }
    script += "synth_gowin -family " + request.yosysFamily + " -top " + request.topModule + "\n";
    if (request.strategy == YosysStrategy::Debug) {
        script += "stat\n";
    }
    script += "write_json \"" + outputPath + "\"\n";

    result.success = true;
    result.script = script;
    return result;
}

} // namespace synth
} // namespace eda
