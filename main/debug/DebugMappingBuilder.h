#pragma once

#include "DebugContract.h"

#include <cstddef>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

struct DebugMappingBuildResult {
    std::size_t mappedSignals = 0;
    std::size_t graphNodes = 0;
    std::size_t graphEdges = 0;
    std::size_t inputSignals = 0;
};

class DebugMappingBuilder {
public:
    // 从当前工程 RTL 和调试契约生成比较所需的两个 sidecar。
    // 解析结果只用于定位和候选排序，不改变用户 RTL。
    static bool Generate(const std::string& topModule,
                         const std::vector<std::string>& sourceFiles,
                         const DebugContract& contract,
                         const std::string& outputDirectory,
                         DebugMappingBuildResult& result,
                         std::string& error);
};

} // namespace debug
} // namespace sigflow
