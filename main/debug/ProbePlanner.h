#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sigflow {
namespace debug {

// TB-08 首版采用确定性规则；稳定键是完整设计/实例路径，不能只用 UI 数字 ID。
struct ProbeRequirement {
    std::string signal;
    std::string reason;
    std::string kind;          // context / trigger / protocol / symptom / upstream / auxiliary
    std::string clockDomain;
    std::uint32_t width = 0;   // 0 = 未知，不能当作零成本
    int priority = 0;
    bool required = false;
};

struct ProbeCandidate {
    std::string signal;
    std::string clockDomain;
    std::uint32_t width = 0;
    bool visible = true;
    bool knownWidth = true;
    bool excluded = false;
    bool alreadySelected = false;
};

struct ProbePlanEntry {
    std::string signal;
    std::string reason;
    std::string kind;
    std::string clockDomain;
    // Stable host-side grouping key.  It does not imply a new bitstream;
    // callers use it to distinguish topic/domain display groups.
    std::string groupKey;
    std::uint32_t width = 0;
    int priority = 0;
    bool required = false;
    bool selected = false;
    bool needsNewBuild = false;
    std::string diagnostic;
};

struct ProbePlanGroup {
    std::string groupKey;
    std::string clockDomain;
    std::string kind;
    std::size_t entryCount = 0;
    std::size_t selectedBits = 0;
};

struct ProbePlan {
    std::string schemaVersion = "1.0";
    std::size_t capacityBits = 32;
    std::size_t usedBits = 0;
    bool feasible = true;
    bool requiresNewBuild = false;
    std::string contractSnapshot; // populated by contract-aware planning
    std::vector<ProbePlanEntry> entries;
    std::vector<ProbePlanGroup> groups;
    std::vector<std::string> missingRequired;
    std::vector<std::string> diagnostics;

    std::string ToJson() const;
};

class ProbePlanner {
public:
    // 规划结果只依赖输入内容和版本，不读取/修改用户 RTL。
    static bool Plan(const std::vector<ProbeRequirement>& requirements,
                     const std::vector<ProbeCandidate>& candidates,
                     std::size_t capacityBits,
                     ProbePlan& result,
                     std::string& error);
};

} // namespace debug
} // namespace sigflow
