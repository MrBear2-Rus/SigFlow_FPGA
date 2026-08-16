#pragma once

#include "DebugContract.h"

#include <string>
#include <vector>

namespace sigflow {
namespace debug {

struct DebugPortInfo {
    std::string name;
    std::string direction;   // input / output / inout
    int width = 1;
};

// P2-04：从 Yosys JSON 网表解析顶层端口 / 校验探针。
class DebugNetlistValidator {
public:
    // 解析 Yosys JSON 顶层端口（modules[top].ports）。
    static bool ParseTopPorts(const std::string& jsonContent, const std::string& topModule,
                              std::vector<DebugPortInfo>& ports, std::string& error);

    // 校验每个探针在网表中存在且位宽一致；keep 网以存在性为准。
    static bool ValidateProbes(const std::string& jsonContent, const std::string& topModule,
                               const std::vector<DebugProbe>& probes, std::string& error);
};

} // namespace debug
} // namespace sigflow
