#pragma once
//
// AgentSubmit — AGUI-T13 统一提问提交协议与提交状态策略（纯 C++20，无 wx / 网络 / 线程）
//
// 设计源：GUI/agent-gui/spec.md §4.6（.aw-compose）、§5.3（提问与回答流程）；
//         GUI/agent-gui/implementation.md §AGUI-T13；index.html askForm/ask/chips/petForm。
// 真实合同事实（不猜、不放大）：
//   * v1 `CreateRunRequest.selection.text` 上限 64 KiB，超限 → 400 INVALID_ARGUMENT；
//     服务端按 **UTF-8 字节** 判定（contracts/edu-agent/v1/agent.openapi.yaml:259、
//     agent/src/sigflow_edu_agent/http_server.py::_parse_selection `len(text.encode("utf-8"))`）。
//     因此本文件只用「UTF-8 字节」表达长度，不引入字符数上限、不猜别的单位。
//   * v1 teaching 的真实前提是「工程 + revision + 会话」：MainFrame::StartTeachingRequest
//     在没有 projectId、客户端或 revision 时直接不发请求。本文件把同一门禁前移到输入区。
//
// 职责：冻结提交文本 / 当前等级 / 可信上下文；校验空白与真实长度；串行化在途提交；
//       处理回执去重与迟到回执；**只在受理回执时**清除"对应的那一份草稿"。
// 边界：不联网、不发请求、不读配置、不判断任务完成（不靠定时器）、不制造后端事实；
//       不因"能发送"而绕过工程 / capability 门禁；产品文案不含开发任务编号。
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace agent::gui::submit {

/// v1 create-run 合同上限：`selection.text` ≤ 64 KiB（UTF-8 字节）。
inline constexpr std::size_t kMaxQuestionBytes = 64u * 1024u;

/// 提交时冻结的可信上下文。缺字段就是空串（不猜、不用当前 run 身份顶替旧事实）。
struct Context
{
    std::string projectId;
    std::string revision;
    std::string sessionId;
    std::string level;                 ///< "L1".."L4"；空 = 未协商
    std::string kind = "explain";      ///< v1 只接受 explain/hint/plan/report_review
    std::string sourceId = "ui-selection";
    std::string nodeId;
    std::string jobId;                 ///< 用户显式选择的 Gateway Job，空时使用基础讲解

    bool operator==(const Context&) const = default;
};

/// 拒绝原因。None = 可以提交。
enum class Reject
{
    None = 0,
    Empty,      ///< 空白（含全角空格/不换行空格）
    TooLong,    ///< 超过合同的 64 KiB（UTF-8 字节）
    Busy,       ///< 已有在途提交 / 回答进行中
    NotReady    ///< 门禁未满足（未接适配器 / 没有工程 / 没有 revision）
};

/// 是否只有空白字符：ASCII 空白 + U+00A0 + U+3000（全角空格），按 UTF-8 解码后判断。
bool IsBlank(const std::string& utf8);

/// UTF-8 字节数（非法序列按自身字节计，不做修复、不抛异常）。
std::size_t Utf8Bytes(const std::string& utf8);

/// 合同校验：空白 → Empty；> 64 KiB → TooLong；否则 None。
Reject Validate(const std::string& utf8);

/// 面向用户的原因文案（UTF-8；不含任务编号）。NotReady 的文案由 Readiness 提供。
const char* RejectText(Reject reason);

/// 提交门禁。ready=false 时 reason/explanation 指出真正影响使用的原因。
struct Readiness
{
    bool ready = false;
    Reject reason = Reject::NotReady;
    std::string explanation;

    bool operator==(const Readiness&) const = default;
};

/// 按顺序判断：未接适配器 → 没有工程 → 没有 revision。三者都是 v1 teaching 的真实前提。
/// projectId/revision 必须来自宿主注入的可信事实，不由本层推断。
Readiness EvaluateReadiness(bool handlerPresent, const Context& context);

/// 一次提交的冻结快照。
struct Intent
{
    std::uint64_t submissionId = 0;   ///< 单调递增，用于回执去重与迟到回执识别
    std::string text;                 ///< 用户原文（不裁剪内部空白；校验才判空白）
    Context context;

    bool operator==(const Intent&) const = default;
};

/// 回执状态：Accepted 才会清草稿；其余一律保留草稿。
enum class ReceiptStatus
{
    Accepted = 0,
    Rejected,
    Busy,
    NotReady
};

const char* ReceiptToken(ReceiptStatus status);

struct Receipt
{
    ReceiptStatus status = ReceiptStatus::NotReady;
    std::string explanation;

    bool operator==(const Receipt&) const = default;
};

/// 宿主提交适配器（T22 接真实链路；T13 未接时由状态策略给出 NotReady）。
/// 只在 UI 线程调用；返回的是"是否受理/为何不受理"的同步回执，回答本身异步。
using Handler = std::function<Receipt(const Intent&)>;

// ---------------------------------------------------------------------------
// ComposerState — 提交状态策略（纯逻辑，可在无 UI 线程测试）
//
// 与视图的分工：视图（AgentComposer）只管输入/按钮/反馈；本类持有草稿、在途快照、
// 门禁与回执结算。任何"该不该清草稿"的判断都在这里，避免视图各自为政。
// ---------------------------------------------------------------------------
class ComposerState
{
public:
    void SetDraft(std::string text);
    const std::string& Draft() const { return m_draft; }

    /// 宿主/模型在途状态（回答进行中）。busy 时 Begin 一律拒绝，但草稿仍可编辑。
    void SetBusy(bool busy);
    bool Busy() const { return m_busy; }

    void SetContext(Context context);
    const Context& Context() const { return m_context; }

    void SetReadiness(Readiness readiness);
    const Readiness& Readiness() const { return m_readiness; }

    enum class Start
    {
        Started = 0,
        Empty,
        TooLong,
        Busy,
        NotReady,
        DuplicateId   ///< 重复的提交 id（重入/请求切换），不产生第二次提交
    };

    /// 开始一次提交：按真实合同校验 text，冻结 {id, text, context}。
    /// * 不清草稿：只有受理回执才清（见 Settle）。
    /// * 已有在途快照 / busy → Busy；门禁未满足 → NotReady（explanation 写门禁原因）。
    /// * submissionId 必须大于 0 且未用于上一次 Begin，否则 DuplicateId。
    Start Begin(std::uint64_t submissionId, std::string text, std::string* explanation);

    bool HasPending() const { return m_pending; }
    const Intent& Pending() const { return m_intent; }

    enum class SettleResult
    {
        ClearedDraft = 0,  ///< Accepted 且草稿仍等于本次快照 → 已清
        KeptDraft,         ///< 草稿被编辑过（或本次未受理）→ 原样保留
        Stale,             ///< 不是当前在途快照的 id（迟到/已切换请求）
        Duplicate          ///< 同一 id 的重复回执，无副作用
    };

    /// 结算一次回执。仅 Accepted、草稿版本/文本及工程身份均未改变时清草稿；
    /// 用户此时已在写下一份草稿（或 newAsk 清空）→ KeptDraft，绝不清新草稿。
    SettleResult Settle(std::uint64_t submissionId, ReceiptStatus status,
                        std::string* explanation);

    /// 请求切换 / 销毁：放弃在途快照（不改草稿）。之后的迟到回执一律 Stale。
    void Abandon();

    std::size_t StartCount() const { return m_starts; }
    std::size_t SettleCount() const { return m_settles; }

private:
    std::string m_draft;
    std::uint64_t m_draftRevision = 0;
    std::uint64_t m_submittedDraftRevision = 0;
    bool m_busy = false;
    agent::gui::submit::Context m_context;
    agent::gui::submit::Readiness m_readiness;
    Intent m_intent;
    bool m_pending = false;
    std::uint64_t m_lastStartedId = 0;
    std::uint64_t m_lastSettledId = 0;
    bool m_hasSettled = false;
    std::size_t m_starts = 0;
    std::size_t m_settles = 0;
};

}  // namespace agent::gui::submit
