#pragma once

#include <optional>
#include <string>

#include <eda/api/jobs.hpp>

namespace eda {
namespace jobs_migration {

// main/jobs（旧 ToolJobState，wx/jsoncpp 层）与新契约（eda::JobState，插件/Gateway 层）
// 当前是同名同序九态，序列化名字也一致。此函数按名字做旧→新映射，
// 历史记录迁移与 GUI 切换过渡期使用；落在九态之外的名字一律拒绝返回 nullopt。
std::optional<JobState> LegacyJobStateFromName(const std::string& legacyName);

// 逆映射（新→旧名字）。两表必须天然对偶：任何一侧改了名字或增删状态，
// jobs 层回归测试会立刻失败，届时须先复核 docs/SigFlow-Job三层收敛方案.md 的映射约定。
std::optional<std::string> JobStateToLegacyName(JobState state);

// 旧层 ToolJobType 序列化名与新层 IJobProvider::jobType 字符串的映射。
// simulation 在新层派生为 sim.build/sim.run 两个 Job；历史迁移归并到 sim.build。
std::optional<std::string> LegacyJobTypeToJobType(const std::string& legacyTypeName);
std::optional<std::string> JobTypeToLegacyJobType(const std::string& jobType);

} // namespace jobs_migration
} // namespace eda
