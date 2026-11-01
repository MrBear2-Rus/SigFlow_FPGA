#pragma once

#include <string>
#include <vector>

namespace eda {
namespace synth {

enum class YosysStrategy {
    Baseline,
    Debug,
    ResourceOptimized,
};

struct StrategyInfo {
    std::string id;
    std::string version;
    std::string displayName;
};

struct YosysScriptRequest {
    std::vector<std::string> sourceFiles;
    std::string topModule;
    std::string targetProfileId;
    std::string targetProfileVersion;
    std::string yosysFamily;
    YosysStrategy strategy = YosysStrategy::Baseline;
    std::string outputJsonPath;
};

struct YosysScriptResult {
    bool success = false;
    std::string script;
    std::string errorMessage;
};

const StrategyInfo& GetStrategyInfo(YosysStrategy strategy);
bool ParseStrategy(const std::string& value, YosysStrategy& strategy);

// 受控 Yosys 脚本生成（wx-free）：从 P1-1 移入自 main/FpgaYosysScriptGenerator。
class YosysScriptGenerator {
public:
    YosysScriptResult Generate(const YosysScriptRequest& request) const;
};

} // namespace synth
} // namespace eda
