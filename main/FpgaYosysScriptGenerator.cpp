#include "FpgaYosysScriptGenerator.h"

#include "YosysScriptGenerator.h"

#include "platform/PlatformPaths.h"

namespace {

const FpgaYosysStrategyInfo kBaselineStrategy{"baseline", "1.0", "Baseline"};
const FpgaYosysStrategyInfo kDebugStrategy{"debug", "1.0", "Debug"};
const FpgaYosysStrategyInfo kResourceOptimizedStrategy{"resource_optimized", "1.0", "Resource optimized"};

eda::synth::YosysStrategy ToPluginStrategy(FpgaYosysSynthesisStrategy strategy) {
    switch (strategy) {
        case FpgaYosysSynthesisStrategy::Baseline:          return eda::synth::YosysStrategy::Baseline;
        case FpgaYosysSynthesisStrategy::Debug:             return eda::synth::YosysStrategy::Debug;
        case FpgaYosysSynthesisStrategy::ResourceOptimized: return eda::synth::YosysStrategy::ResourceOptimized;
    }
    return eda::synth::YosysStrategy::Baseline;
}

} // namespace

// P1-1：脚本生成逻辑已移入 InnerPlugin/eda-synth-yosys（wx-free）。
// 本文件保留同签名的 wx 适配层，供旧调用点（MainFrame / fpga_flow_probe）在迁移期使用。
const FpgaYosysStrategyInfo& GetFpgaYosysStrategyInfo(FpgaYosysSynthesisStrategy strategy) {
    switch (strategy) {
        case FpgaYosysSynthesisStrategy::Baseline:          return kBaselineStrategy;
        case FpgaYosysSynthesisStrategy::Debug:             return kDebugStrategy;
        case FpgaYosysSynthesisStrategy::ResourceOptimized: return kResourceOptimizedStrategy;
    }
    return kBaselineStrategy;
}

bool ParseFpgaYosysSynthesisStrategy(const wxString& value,
                                     FpgaYosysSynthesisStrategy& strategy) {
    eda::synth::YosysStrategy parsed = eda::synth::YosysStrategy::Baseline;
    if (!eda::synth::ParseStrategy(sigflow::platform::Utf8String(value), parsed)) {
        return false;
    }
    switch (parsed) {
        case eda::synth::YosysStrategy::Baseline:          strategy = FpgaYosysSynthesisStrategy::Baseline; return true;
        case eda::synth::YosysStrategy::Debug:             strategy = FpgaYosysSynthesisStrategy::Debug; return true;
        case eda::synth::YosysStrategy::ResourceOptimized: strategy = FpgaYosysSynthesisStrategy::ResourceOptimized; return true;
    }
    return false;
}

FpgaYosysScriptResult FpgaYosysScriptGenerator::Generate(
    const FpgaYosysScriptRequest& request) const {
    eda::synth::YosysScriptRequest pluginRequest;
    pluginRequest.sourceFiles.reserve(request.sourceFiles.size());
    for (const wxString& sourceFile : request.sourceFiles) {
        pluginRequest.sourceFiles.push_back(sigflow::platform::Utf8String(sourceFile));
    }
    pluginRequest.topModule = sigflow::platform::Utf8String(request.topModule);
    pluginRequest.targetProfileId = sigflow::platform::Utf8String(request.targetProfile.id);
    pluginRequest.targetProfileVersion = sigflow::platform::Utf8String(request.targetProfile.version);
    pluginRequest.yosysFamily = sigflow::platform::Utf8String(request.targetProfile.yosysFamily);
    pluginRequest.strategy = ToPluginStrategy(request.strategy);
    pluginRequest.outputJsonPath = sigflow::platform::Utf8String(request.outputJsonPath);

    const eda::synth::YosysScriptResult pluginResult =
        eda::synth::YosysScriptGenerator().Generate(pluginRequest);

    FpgaYosysScriptResult result;
    result.success = pluginResult.success;
    result.script = wxString::FromUTF8(pluginResult.script.c_str());
    result.errorMessage = wxString::FromUTF8(pluginResult.errorMessage.c_str());
    return result;
}
