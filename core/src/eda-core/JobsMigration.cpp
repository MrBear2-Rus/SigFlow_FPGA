#include "JobsMigration.h"

#include <map>

namespace eda {
namespace jobs_migration {
namespace {

// 旧层（main/jobs JobService.cpp 的 ToString）与新层（ReportNormalizer JobStateName）
// 的九态名字表。名字与顺序在此处单向维护：两侧任何一侧冻结前不允许私改，
// 否则 jobs 层 roundtrip 断言会失败。
const std::map<std::string, JobState>& NameTable() {
    static const std::map<std::string, JobState> table{
        {"Created",             JobState::Created},
        {"Validating",          JobState::Validating},
        {"Queued",              JobState::Queued},
        {"Running",             JobState::Running},
        {"ValidatingArtifact",  JobState::ValidatingArtifact},
        {"Succeeded",           JobState::Succeeded},
        {"Failed",              JobState::Failed},
        {"Cancelled",           JobState::Cancelled},
        {"TimedOut",            JobState::TimedOut},
    };
    return table;
}

bool ProvidesName(const JobState& value, const std::string*& name) {
    for (const auto& entry : NameTable()) {
        if (entry.second == value) {
            name = &entry.first;
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<JobState> LegacyJobStateFromName(const std::string& legacyName) {
    const auto& table = NameTable();
    const auto found = table.find(legacyName);
    if (found == table.end()) return std::nullopt;
    return found->second;
}

std::optional<std::string> JobStateToLegacyName(JobState state) {
    const std::string* name = nullptr;
    if (!ProvidesName(state, name)) return std::nullopt;
    return *name;
}

std::optional<std::string> LegacyJobTypeToJobType(const std::string& legacyTypeName) {
    // 左：main/jobs JobService.cpp ToString(ToolJobType)；右：官方插件 jobType()。
    static const std::map<std::string, std::string> table{
        {"simulation", "sim.build"},
        {"synthesis",  "synth"},
        {"pnr",        "pnr"},
        {"pack",       "pack"},
        {"flash",      "flash"},
    };
    const auto found = table.find(legacyTypeName);
    if (found == table.end()) return std::nullopt;
    return found->second;
}

std::optional<std::string> JobTypeToLegacyJobType(const std::string& jobType) {
    static const std::map<std::string, std::string> table{
        {"sim.build", "simulation"},
        {"sim.run",   "simulation"},
        {"synth",     "synthesis"},
        {"pnr",       "pnr"},
        {"pack",       "pack"},
        {"flash",      "flash"},
    };
    const auto found = table.find(jobType);
    if (found == table.end()) return std::nullopt;
    return found->second;
}

} // namespace jobs_migration
} // namespace eda
