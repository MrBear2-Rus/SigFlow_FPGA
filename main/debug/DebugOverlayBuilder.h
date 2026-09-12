#pragma once

#include "DebugContract.h"
#include "DebugNetlistValidator.h"

#include <map>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

struct DebugOverlayResult {
    std::string wrapperPath;
    std::string cstPath;
    std::string yosysScriptPath;
    std::string nextpnrArgsPath;
    std::string gowinPackArgsPath;
    std::string fsPath;
    std::string netlistJsonPath;
    std::string netlistCheckJsonPath;   // 综合前（hierarchy 后）的探针校验网表
    std::vector<std::string> generatedFiles;
};

// P2-03：调试 overlay 生成器。
// 生成：顶层 wrapper（DUT + sf_micro_ila + sf_debug_link）、CST 补丁、
//       Yosys/nextpnr/gowin_pack 脚本；不修改任何用户源文件。
class DebugOverlayBuilder {
public:
    bool GenerateOverlay(const DebugContract& contract,
                         const std::string& projectPath,
                         const std::string& sessionDir,
                         const std::string& artifactsDir,
                         const std::vector<std::string>& userSourceFiles,
                         const std::vector<DebugPortInfo>& userTopPorts,
                         const std::string& debugRtlDir,
                         const std::string& device,
                         const std::string& family,
                         bool stubLink,
                         const std::string& configuredCstPath,
                         DebugOverlayResult& result,
                         std::string& error) const;

    // 用户源码不变性：构建前快照哈希，构建后校验未变化。
    bool SnapshotUserSources(const std::vector<std::string>& files,
                             std::map<std::string, std::string>& before,
                             std::string& error) const;
    bool VerifyUserSourcesUntouched(const std::map<std::string, std::string>& before,
                                    std::string& error) const;

private:
    std::string BuildWrapper(const DebugContract& contract,
                             const std::vector<DebugPortInfo>& userTopPorts) const;
    std::string BuildMergedCst(const DebugContract& contract,
                                const std::string& projectPath,
                                const std::string& configuredCstPath,
                                const std::vector<DebugPortInfo>& userTopPorts,
                                std::string& error) const;
    std::string BuildYosysScript(const DebugContract& contract,
                                 const std::vector<std::string>& userSourceFiles,
                                 const std::string& debugRtlDir,
                                 const std::string& overlayDir,
                                 const std::string& artifactsDir,
                                 const std::string& wrapperPath,
                                 bool stubLink) const;
    std::string BuildLinkStub(const DebugContract& contract) const;
};

} // namespace debug
} // namespace sigflow
