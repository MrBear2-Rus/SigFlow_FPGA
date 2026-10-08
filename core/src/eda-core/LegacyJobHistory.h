#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <eda/api/jobs.hpp>

namespace eda {

// 旧层（main/jobs，ToolJobState/"1.0" schema）历史记录的只读加载。
// GUI 从旧 JobService 切到新 IJobService 的过渡期，历史面板经它读旧的
// <project>/.sigflow/jobs/<type>/<jobId>/manifest.json 与 job-report.json，
// 状态/类型按 jobs_migration 的锁定映射翻译成新契约值；本模块绝不写入
// 或改写任何旧存储文件（name 里的 Legacy…均为只读行为）。
//
// 布局对齐 main/jobs/JobService.cpp::GetPaths：
//   manifest.json: schema_version/job_type/job_id/retry_of/state/created_at/
//                  updated_at/exit_code/operator/parameters/transitions
//   reports/job-report.json: "1.0" 报告（summary/errors/artifacts 等）
struct LegacyJobHistoryEntry {
    JobRecord record;   // state/类型已映射为新契约值（request.jobType）
    JobReport report;   // 归一化到 edu.jobreport.v1 形态；未落盘报告则为空报告
};

// 遍历五类历史目录并返回可读条目（按 created_at 升序）。
// jobs 根不存在 → 返回 true 且 out 为空（空白历史不是错误）。
// 单条 manifest 解析失败/字段非法 → 跳过该 Job 并把 id 追加到 unreadableJobIds。
bool LoadLegacyJobHistory(const std::filesystem::path& projectPath,
                          std::vector<LegacyJobHistoryEntry>& out,
                          std::vector<std::string>& unreadableJobIds,
                          std::string& error);

// 早期 FPGA 专用 Job 使用独立目录：runs（综合）/runs-nextpnr（布线）。
// 与通用旧 Job 一样仅做只读加载，供新历史面板兼容展示。
bool LoadLegacyFpgaJobHistory(const std::filesystem::path& projectPath,
                              std::vector<LegacyJobHistoryEntry>& out,
                              std::vector<std::string>& unreadableJobIds,
                              std::string& error);

} // namespace eda
