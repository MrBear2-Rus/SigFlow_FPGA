// AgentReportSelection.cpp — 报告选择过滤/展示模型
// （纯 C++20：无 wxWidgets、无网络、无线程、无文件 IO、无静态可变状态）
//
// 事实来源（不猜、不放大）：
//   * Gateway `GET /api/v1/projects/{p}/jobs` 的每个 summary 只有
//     job_id / job_type / plugin_id / capability / state / exit_code / created_at /
//     updated_at（绑定存在时再加 snapshot_id / revision / plan_hash / grant_id / step_id）。
//     因此过滤必须容忍“没有 exit_code / 没有 project_id”的条目，但绝不能放宽
//     revision、capability、state 与 job_id 这四道门。
//   * 服务端 JobStateName() 的规范状态名是 Queued/Running/Cancelling/Succeeded/
//     Failed/TimedOut/Rejected/Cancelled/Unknown；其中只有后五个是终态。
//     Unknown 既不是成功也不是可解释的终态，必须排除。
//   * job_id 形状与 `MainFrame` 的手动输入校验、服务端 `_parse_selection` 的
//     opaque ID 口径一致：1..128 字节，只允许 [A-Za-z0-9_-]。过滤比手动输入更严：
//     空 job_id 直接排除，绝不为空 ID 制造一个可选择条目。
#include "agent/gui/AgentReportSelection.h"

#include <cstdint>
#include <limits>
#include <utility>

namespace agent::gui::reports {
namespace {

/// 终态白名单：规范状态名 → 中文标签。顺序即展示优先级（不用于过滤排序）。
struct TerminalState
{
    const char* state;
    const char* label;
};

constexpr TerminalState kTerminalStates[] = {
    {"Succeeded", "成功"},
    {"Failed", "失败"},
    {"TimedOut", "超时"},
    {"Rejected", "已拒绝"},
    {"Cancelled", "已取消"},
};

bool IsOpaqueJobIdByte(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
}

/// 读一个字符串字段；缺失或不是字符串都返回空（调用方按“缺字段”处理）。
std::string StringField(const Json& item, const char* key)
{
    const auto it = item.find(key);
    if (it == item.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

/// 读 exit_code：只有整数才展示；字符串/浮点/null 视为“没有给出”。
/// 超出 int 范围的整数同样不展示（不截断、不猜测）。
bool ExitCodeOf(const Json& item, int& exitCode)
{
    const auto it = item.find("exit_code");
    if (it == item.end() || !it->is_number_integer()) return false;
    if (it->is_number_unsigned())
    {
        const auto value = it->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return false;
        exitCode = static_cast<int>(value);
        return true;
    }
    const auto value = it->get<std::int64_t>();
    if (value < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
        return false;
    exitCode = static_cast<int>(value);
    return true;
}

/// 接受 `{"jobs": [...]}` 与裸数组两种形状；其它形状返回 nullptr（不是空数组）。
const Json* JobArray(const Json& jobs)
{
    if (jobs.is_array()) return &jobs;
    if (jobs.is_object())
    {
        const auto it = jobs.find("jobs");
        if (it != jobs.end() && it->is_array()) return &*it;
    }
    return nullptr;
}

}  // namespace

bool ValidJobId(const std::string& jobId)
{
    if (jobId.empty() || jobId.size() > kMaxJobIdLength) return false;
    for (const unsigned char byte : jobId)
    {
        if (!IsOpaqueJobIdByte(byte)) return false;
    }
    return true;
}

bool IsSupportedCapability(const std::string& capability)
{
    return capability == "eda.synth" || capability == "eda.sim.build" || capability == "eda.sim.run";
}

bool IsTerminalState(const std::string& state)
{
    for (const auto& terminal : kTerminalStates)
    {
        if (state == terminal.state) return true;
    }
    return false;
}

std::string StateLabel(const std::string& state)
{
    for (const auto& terminal : kTerminalStates)
    {
        if (state == terminal.state) return terminal.label;
    }
    return std::string();
}

std::string DisplayText(const Entry& entry)
{
    std::string text = entry.capability;
    text += "  ";
    text += entry.jobId;
    text += "  ";
    text += entry.stateLabel;
    if (entry.hasExitCode)
    {
        text += "  exit_code=";
        text += std::to_string(entry.exitCode);
    }
    return text;
}

std::vector<Entry> SelectTerminalJobs(const Json& jobs, const std::string& projectId,
                                      const std::string& revision)
{
    std::vector<Entry> entries;
    // 没有可信的工程/版本绑定就不提供选择：宁可空列表，也不跨工程/跨版本取证据。
    if (projectId.empty() || revision.empty()) return entries;
    const Json* list = JobArray(jobs);
    if (list == nullptr) return entries;
    for (const auto& item : *list)
    {
        if (!item.is_object()) continue;
        if (StringField(item, "revision") != revision) continue;
        // 条目带 project_id 时必须一致；带的是非字符串同样视为不可信并排除。
        const auto projectIt = item.find("project_id");
        if (projectIt != item.end())
        {
            if (!projectIt->is_string() || projectIt->get<std::string>() != projectId) continue;
        }
        const std::string capability = StringField(item, "capability");
        if (!IsSupportedCapability(capability)) continue;
        const std::string state = StringField(item, "state");
        if (!IsTerminalState(state)) continue;
        const std::string jobId = StringField(item, "job_id");
        if (!ValidJobId(jobId)) continue;
        Entry entry;
        entry.jobId = jobId;
        entry.capability = capability;
        entry.state = state;
        entry.stateLabel = StateLabel(state);
        entry.hasExitCode = ExitCodeOf(item, entry.exitCode);
        entry.display = DisplayText(entry);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string NoSelectableFeedback()
{
    return "当前工程当前版本没有可选的终态 Job 报告"
           "（成功、失败、超时、已取消、已拒绝）。请先完成一次综合、仿真构建或仿真运行。";
}

}  // namespace agent::gui::reports
