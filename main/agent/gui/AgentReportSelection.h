#pragma once
//
// AgentReportSelection — 报告选择过滤/展示模型（纯 C++20、无 wx、可单测）
//
// 设计源：docs/edu-agent/sigflow/spec.md（报告教学由服务核验）、
//         docs/agent/SigFlow-Edu-Agent-GUI-Spec.md（可信证据）、
//         contracts/edu-agent/v1/agent.openapi.yaml（`job_id` 形状）、
//         gateway `GET /api/v1/projects/{p}/jobs` 的真实响应字段。
//
// 为什么需要这一层：
//   * 报告教学的价值不只在成功报告：Failed/TimedOut/Rejected/Cancelled 的终态报告
//     恰恰是学生最需要解读的证据（agent/src/sigflow_edu_agent/dispatch.py 的
//     kind=report_review 已能核验并解释这些终态，并在含 diagnostics 时生成
//     带 job_report 证据的卡片）。
//   * 宿主此前在 MainFrame::PollAgentJobs 里只挑 `state == "Succeeded" && exit_code == 0`，
//     等于把失败/超时/取消/拒绝的报告全部挡在 GUI 之外。
//   * 过滤口径必须只有一份，且能被单测覆盖：把「终态 + 当前 revision + 支持的 EDA
//     能力 + 合法 job_id」写成封闭白名单，避免把 Unknown 之类的新状态当成成功。
//
// 边界：本文件无 wx、无网络、无线程、无文件 IO、无异常、无静态可变状态；
//       只做「过滤 + 展示文案」，不判断教学状态、不读凭据、不解密报告正文。
//       展示文案里只有规范能力名、规范状态名、job_id 与 exit_code，绝无路径。
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::gui::reports {

/// Gateway 响应体的 JSON 类型（与 `eda::Json` 同源：nlohmann::json）。
using Json = nlohmann::json;

/// 宿主手动输入与模型过滤共用的 job_id 上限（与 MainFrame 手动输入校验一致）。
inline constexpr std::size_t kMaxJobIdLength = 128;

/// 可供报告教学选择的终态 Job（不可变值类型）。
struct Entry
{
    std::string jobId;       ///< Gateway job_id（仅 [A-Za-z0-9_-]，≤128）
    std::string capability;  ///< 规范能力名（eda.synth / eda.sim.build / eda.sim.run）
    std::string state;       ///< Gateway 规范状态名（Succeeded/Failed/TimedOut/Rejected/Cancelled）
    std::string stateLabel;  ///< 中文终态标签（成功/失败/超时/已拒绝/已取消）
    bool hasExitCode = false;  ///< Gateway 是否给出整数 exit_code
    int exitCode = 0;
    std::string display;  ///< 对话框条目文本：能力 + job_id + 终态 + exit_code（如有）

    bool operator==(const Entry&) const = default;
};

/// 是否是合法的 Gateway job_id：长度 1..128，只允许 [A-Za-z0-9_-]。
/// 因此 `rtl/top.v`、`C:\proj\job`、带空格或中文的值都不是合法 job_id。
bool ValidJobId(const std::string& jobId);

/// 是否是本 GUI 支持的 EDA 能力：eda.synth / eda.sim.build / eda.sim.run。
bool IsSupportedCapability(const std::string& capability);

/// 是否是允许进入报告选择的终态：Succeeded/Failed/TimedOut/Rejected/Cancelled。
/// Queued/Running/Cancelling/Unknown 一律不是终态（Unknown 不是成功）。
bool IsTerminalState(const std::string& state);

/// 终态的中文标签；非终态返回空字符串（调用方据此不展示）。
std::string StateLabel(const std::string& state);

/// 生成对话框条目文本：`eda.synth  job-1  失败  exit_code=1`。
/// 无 exit_code 时省略最后一段；不含任何路径或报告正文。
std::string DisplayText(const Entry& entry);

/// 过滤 Gateway 的 jobs JSON，返回可供选择的终态条目（保持 Gateway 顺序）。
///
/// jobs 允许两种形状（Gateway 真实响应 + 单测都覆盖）：
///   * `{"jobs": [...], "total": ...}` —— GET /projects/{p}/jobs 的 `data`；
///   * `[ ... ]` —— 仅 jobs 数组本身。
///
/// 纳入条件（全部满足，缺一不可）：
///   1. 条目是 JSON 对象；
///   2. `revision` 与之完全相等（跨版本/缺字段一律排除）；
///   3. 若条目带 `project_id`，必须与 projectId 完全相等（跨工程一律排除）；
///   4. `capability` ∈ {eda.synth, eda.sim.build, eda.sim.run}；
///   5. `state` ∈ {Succeeded, Failed, TimedOut, Rejected, Cancelled}；
///   6. `job_id` 合法（ValidJobId）。
///
/// 非终态（Queued/Running/Cancelling/Unknown）、其它能力、非法/空 ID 一律排除；
/// 不因为 `exit_code != 0` 丢条目——失败/超时/取消/拒绝报告正是要能被选择。
/// revision 或 projectId 为空时返回空集合（没有可信的版本/工程绑定就不提供选择）。
std::vector<Entry> SelectTerminalJobs(const Json& jobs,
                                      const std::string& projectId,
                                      const std::string& revision);

/// 没有可选终态条目时的固定中文反馈（无路径、无 ID、无后端猜测）。
std::string NoSelectableFeedback();

}  // namespace agent::gui::reports
