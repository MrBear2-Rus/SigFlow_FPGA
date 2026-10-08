#pragma once

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>

#include <string>

namespace eda {
namespace agent {

// SF-06：新旧 Job 报告归一化为 edu.jobreport.v1。
// 关键语义（spec §5.4）：
//   * 「完整扫描且未发现」与「未提供/解析失败」必须可区分；
//   * metrics 缺失用 null+reason，不填 0；
//   * 老产物缺指纹/版本 → completeness=legacy_unverified；
//   * 保留原始 schema 与产物 hash，不丢来源。
struct ReportNormalizeInput {
    JobReport report;
    std::string projectId;
    std::string revision;      // 可空
    std::string snapshotId;    // 可空
    std::string capability;    // 可空（缺省用 report.jobType 映射）
    std::string pluginVersion; // 可空
    std::string inputFingerprint;  // 可空
    std::string rawReportSchema;   // 可空（如 "main/jobs 1.0"）
    bool diagnosticsProvided = true;  // 上报方是否提供了 diagnostics（false → unavailable）
    bool legacyUnverified = false;    // 老产物（无指纹/版本）
};

Json NormalizeJobReport(const ReportNormalizeInput& input);

// JobState → 稳定字符串（与 spec §5.4 state 取值一致）。
const char* JobStateName(JobState state);

} // namespace agent
} // namespace eda
