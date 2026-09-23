#include "CstFileValidator.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <regex>

namespace eda {
namespace cst {
namespace {

constexpr std::uintmax_t kMaxCstFileSize = 100 * 1024;
constexpr int kMaxGowinPin = 200;

std::string Trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// 基本 UTF-8 校验（拒绝非法字节序列）。
bool IsValidUtf8(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        std::size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= value.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(value[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

} // namespace

std::filesystem::path CstFileValidator::AutoResolveCst(
    const std::filesystem::path& projectRoot, const std::string& configuredCstPath) {
    if (!configuredCstPath.empty()) {
        const std::filesystem::path configured(configuredCstPath);
        if (configured.is_absolute()) return configured;
        std::error_code error;
        return std::filesystem::absolute(projectRoot / configured, error).lexically_normal();
    }

    const std::filesystem::path defaultPath = projectRoot / "constraints" / "tangnano9k.cst";
    std::error_code error;
    if (std::filesystem::is_regular_file(defaultPath, error)) return defaultPath;

    const std::filesystem::path constraints = projectRoot / "constraints";
    if (std::filesystem::is_directory(constraints, error)) {
        for (std::filesystem::directory_iterator it(constraints, error), end; it != end;
             it.increment(error)) {
            if (error) break;
            if (it->path().extension() == ".cst") return it->path();
        }
    }

    const std::filesystem::path nextpnr = projectRoot / "nextpnr";
    if (std::filesystem::is_directory(nextpnr, error)) {
        for (std::filesystem::directory_iterator it(nextpnr, error), end; it != end;
             it.increment(error)) {
            if (error) break;
            if (it->path().extension() == ".cst") return it->path();
        }
    }

    return defaultPath;
}

bool CstFileValidator::ExistsAndReadable(const std::string& cstPath) {
    if (cstPath.empty()) return false;
    std::ifstream input(cstPath, std::ios::binary);
    return static_cast<bool>(input);
}

CstValidationResult CstFileValidator::Validate(const std::string& cstPath,
                                               const std::vector<std::string>& allPorts) {
    CstValidationResult result;
    result.cstPath = cstPath;

    std::error_code error;
    result.fileExists = std::filesystem::exists(cstPath, error);
    if (!result.fileExists) {
        result.errorSummary =
            "CST constraint file does not exist: " + cstPath +
            "\nRun FPGA > Pin Constraints to generate a pin constraint file first.";
        return result;
    }

    const std::uintmax_t size = std::filesystem::file_size(cstPath, error);
    if (error || size == 0) {
        result.errorSummary = "CST file is empty.";
        return result;
    }
    if (size > kMaxCstFileSize) {
        result.errorSummary =
            "CST file is too large to be a valid constraint file (max 100KB).";
        return result;
    }
    result.fileNonEmpty = true;

    std::ifstream input(cstPath, std::ios::binary);
    if (!input) {
        result.errorSummary = "Cannot read CST file.";
        return result;
    }
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    if (input.bad()) {
        result.errorSummary = "CST file read was incomplete.";
        return result;
    }
    if (!IsValidUtf8(content)) {
        result.errorSummary =
            "CST file is not valid UTF-8; re-save the constraint file as UTF-8.";
        return result;
    }

    // 统一换行符后逐行校验。
    std::string normalized;
    normalized.reserve(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n') ++i;
            normalized += '\n';
        } else {
            normalized += content[i];
        }
    }

    int lineNum = 0;
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t end = normalized.find('\n', start);
        const std::string line = Trim(normalized.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        ++lineNum;
        ++result.totalLines;
        if (!line.empty()) ValidateSingleLine(line, lineNum, result);
        if (end == std::string::npos) break;
        start = end + 1;
    }

    result.syntaxOk = (result.invalidLines == 0);
    result.valid = result.fileExists && result.fileNonEmpty && result.syntaxOk;

    if (!result.syntaxOk) {
        result.errorSummary = "CST file has " + std::to_string(result.invalidLines) +
                              " syntax error(s) across " + std::to_string(result.totalLines) +
                              " line(s).\n";
        for (const auto& lineError : result.lineErrors) {
            result.errorSummary += "  line " + std::to_string(lineError.line) + ": " +
                                   lineError.content + " - " + lineError.reason + "\n";
        }
    }

    if (!allPorts.empty() && result.syntaxOk) {
        std::vector<std::string> unbound;
        for (const auto& port : allPorts) {
            bool found = false;
            for (const auto& bound : result.boundPorts) {
                if (bound == port) { found = true; break; }
            }
            if (!found) unbound.push_back(port);
        }
        if (!unbound.empty()) {
            result.errorSummary += "\nUnbound ports: ";
            for (const auto& port : unbound) result.errorSummary += port + " ";
            result.valid = false;
        }
    }

    return result;
}

bool CstFileValidator::ValidateSingleLine(const std::string& line, int lineNum,
                                          CstValidationResult& result) {
    if (line.rfind("#", 0) == 0 || line.rfind("//", 0) == 0) {
        ++result.commentLines;
        return true;
    }

    static const std::regex ioLocRe("^IO_LOC\\s+\"([^\"]+)\"\\s+(\\d+)\\s*;");
    static const std::regex ioPortRe("^IO_PORT\\s+\"([^\"]+)\"\\s+(.+);");
    static const std::regex ioTypeRe("IO_TYPE\\s*=\\s*(\\w+)",
                                     std::regex::ECMAScript | std::regex::icase);

    std::smatch match;
    if (std::regex_search(line, match, ioLocRe)) {
        ++result.ioLocCount;
        const std::string portName = match[1].str();
        const std::string pinNum = match[2].str();
        result.boundPorts.push_back(portName);
        try {
            const long pin = std::stol(pinNum);
            if (pin <= 0 || pin > kMaxGowinPin) {
                ++result.invalidLines;
                CstValidationResult::LineError lineError;
                lineError.line = lineNum;
                lineError.content = line;
                lineError.reason =
                    "pin number \"" + pinNum + "\" is invalid (expected 1-" +
                    std::to_string(kMaxGowinPin) + ")";
                result.lineErrors.push_back(lineError);
                return false;
            }
        } catch (const std::exception&) {
            ++result.invalidLines;
        }
        return true;
    }

    if (std::regex_search(line, match, ioPortRe)) {
        ++result.ioPortCount;
        const std::string attrs = match[2].str();
        if (!std::regex_search(attrs, ioTypeRe)) {
            ++result.invalidLines;
            CstValidationResult::LineError lineError;
            lineError.line = lineNum;
            lineError.content = line;
            lineError.reason = "missing IO_TYPE attribute (e.g. IO_TYPE=LVCMOS33)";
            result.lineErrors.push_back(lineError);
            return false;
        }
        return true;
    }

    ++result.invalidLines;
    CstValidationResult::LineError lineError;
    lineError.line = lineNum;
    lineError.content = line;
    lineError.reason = "unsupported CST syntax (only IO_LOC / IO_PORT or # comment)";
    result.lineErrors.push_back(lineError);
    return false;
}

} // namespace cst
} // namespace eda
