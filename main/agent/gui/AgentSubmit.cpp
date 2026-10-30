// AgentSubmit.cpp — 统一提问提交协议与提交状态策略实现
// （纯 C++20：无 wxWidgets、无网络、无线程、无文件 IO、无异常、无静态可变状态）
//
// 合同事实（来自 v1 edu-agent 合同，不猜、不放大）：
//   * 提问正文就是 `CreateRunRequest.selection.text`，上限 64 KiB（kMaxQuestionBytes =
//     64 * 1024），服务端按 **UTF-8 字节** 计（`len(text.encode("utf-8"))`），超限 → 400
//     INVALID_ARGUMENT。因此这里只按字节判定长度：不引入字符数上限，也不按码点折算；
//     恰好 65536 字节算合规，65537 字节才 TooLong。
//   * 长度与空白是两件事：先判空白（空白问题无论多短都不发），再判长度。空白判定只认
//     解码后的码点：ASCII 空格 / \t / \n / \r / \f / \v、U+00A0（不换行空格）、
//     U+3000（全角空格）。非法 UTF-8 字节无法证明是空白，一律按"有内容"处理，因此
//     不会把一段乱码静默当成空问题。
//   * v1 teaching 的真实前提是「适配器 + 工程 + revision」三者齐备：主界面在没有工程、
//     客户端或 revision 时根本不会发请求。本层把同一门禁前移到输入区，评估顺序固定为
//     未接适配器 → 没有 projectId → 没有 revision，第一条命中即为用户看到的原因。
//     这些字段必须由宿主注入的可信事实提供，本层不推断、不用旧的 run 身份顶替。
//
// 结算规则（Settle）——只有"受理"并且"草稿仍是本次那份"才清草稿：
//   * Accepted 且 m_draft == m_intent.text → 清草稿，返回 ClearedDraft；
//   * Accepted 但草稿已被改写（用户已在写下一问）→ KeptDraft，绝不清新草稿；
//   * 任何非 Accepted 回执 → KeptDraft，草稿原样保留，且本层不编造回执说明；
//   * 同一 id 的重复回执 → Duplicate，无副作用；不是当前在途快照的 id（迟到回执、
//     Abandon 之后到达、或请求已切换）→ Stale，同样无副作用。
//   * 计数口径：StartCount 只数真正 Started 的提交；SettleCount 只数真正落地的结算
//     （Stale / Duplicate 不计），因此两者可用于判断"发起与回执是否配平"。
//
// 确定性：所有决策只依赖入参与成员状态，不读时钟、不读配置、不做隐式重试；
// 除 std::string 的拷贝/移动外不分配内存，输入顺序相同则输出完全相同。
//
// 命名约束（合同事实，影响本文件的写法）：
//   ComposerState 里存在与类型同名的成员函数（Context()/Readiness() 访问器、Settle() 结算
//   函数）。C++ 的类作用域查找里，成员函数会遮蔽同名的命名空间级类型 / 类内枚举，因此：
//     * 参数类型必须写成全限定名 `agent::gui::submit::Context` / `...::Readiness`，
//       否则会被解析成同名成员函数（"does not name a type"）；
//     * 结算结果枚举只能叫 `SettleResult`：若它与 `Settle()` 同名，枚举名在类作用域内
//       被函数遮蔽，类外无法再拼出该类型，连返回类型都写不出来。
//   这些都是 C++ 名字查找的硬约束，不是风格偏好。

#include "agent/gui/AgentSubmit.h"

#include <utility>

namespace agent::gui::submit {
namespace {

/// ASCII 空白（0x09..0x0D 与 0x20）。
bool IsAsciiBlank(unsigned char byte)
{
    switch (byte)
    {
        case 0x09u:  // \t
        case 0x0Au:  // \n
        case 0x0Bu:  // \v
        case 0x0Cu:  // \f
        case 0x0Du:  // \r
        case 0x20u:  // 空格
            return true;
        default:
            return false;
    }
}

/// 从 index 处解出一个 UTF-8 码点。
/// 成功：码点写入 codePoint，index 前移到该码点之后，返回 true。
/// 失败：index 只前进 1 字节（保证线性推进、不死循环），返回 false。
/// 判为非法的情况：非法起始字节（0x80..0xC1、0xF5..0xFF）、截断、过长编码、
/// UTF-16 代理区码点、超出 U+10FFFF —— 即 Unicode 定义的合法 UTF-8 之外一律不合法。
bool DecodeCodePoint(const std::string& utf8, std::size_t& index, char32_t& codePoint)
{
    const std::size_t size = utf8.size();
    const auto byteAt = [&utf8](std::size_t at) {
        return static_cast<unsigned char>(utf8[at]);
    };

    const unsigned char lead = byteAt(index);
    if (lead < 0x80u)
    {
        codePoint = static_cast<char32_t>(lead);
        ++index;
        return true;
    }

    std::size_t length = 0;
    char32_t code = 0;
    char32_t minimum = 0;  // 该长度允许的最小码点，用于识别过长编码
    if (lead >= 0xC2u && lead <= 0xDFu)
    {
        length  = 2;
        code    = static_cast<char32_t>(lead & 0x1Fu);
        minimum = 0x80u;
    }
    else if (lead >= 0xE0u && lead <= 0xEFu)
    {
        length  = 3;
        code    = static_cast<char32_t>(lead & 0x0Fu);
        minimum = 0x800u;
    }
    else if (lead >= 0xF0u && lead <= 0xF4u)
    {
        length  = 4;
        code    = static_cast<char32_t>(lead & 0x07u);
        minimum = 0x10000u;
    }
    else
    {
        ++index;  // 0x80..0xC1 是裸续字节或过长两字节前导；0xF5..0xFF 超出 Unicode 范围
        return false;
    }

    if (size - index < length)
    {
        ++index;  // 序列被截断
        return false;
    }

    for (std::size_t offset = 1; offset < length; ++offset)
    {
        const unsigned char continuation = byteAt(index + offset);
        if ((continuation & 0xC0u) != 0x80u)
        {
            ++index;  // 续字节格式错误：整体不合法
            return false;
        }
        code = static_cast<char32_t>((code << 6) | static_cast<char32_t>(continuation & 0x3Fu));
    }

    if (code < minimum || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu))
    {
        ++index;  // 过长编码 / 超出范围 / 代理区码点
        return false;
    }

    index += length;
    codePoint = code;
    return true;
}

}  // namespace

bool IsBlank(const std::string& utf8)
{
    std::size_t index = 0;
    while (index < utf8.size())
    {
        char32_t codePoint = 0;
        if (!DecodeCodePoint(utf8, index, codePoint))
        {
            return false;  // 非法 UTF-8 无法证明是空白 → 按有内容处理
        }
        if (codePoint < 0x80u)
        {
            if (!IsAsciiBlank(static_cast<unsigned char>(codePoint)))
            {
                return false;
            }
            continue;
        }
        if (codePoint != 0x00A0u && codePoint != 0x3000u)
        {
            return false;  // 其余非 ASCII 码点都是内容（含其它 Unicode 空格类字符，不擅自扩大口径）
        }
    }
    return true;  // 空串也算空白：没有任何一个"有内容"的码点
}

std::size_t Utf8Bytes(const std::string& utf8)
{
    return utf8.size();  // 合同按字节判定长度，非法序列同样按自身字节计
}

Reject Validate(const std::string& utf8)
{
    if (IsBlank(utf8))
    {
        return Reject::Empty;
    }
    if (Utf8Bytes(utf8) > kMaxQuestionBytes)
    {
        return Reject::TooLong;
    }
    return Reject::None;
}

const char* RejectText(Reject reason)
{
    switch (reason)
    {
        case Reject::Empty:
            return "先写一句要问的问题。";
        case Reject::TooLong:
            return "问题太长，请删减后再发送（上限 64 KiB，按 UTF-8 字节计）。";
        case Reject::Busy:
            return "回答进行中，稍等一下。";
        case Reject::NotReady:
            return "当前还不能发送。";
        case Reject::None:
            return "";
        default:
            return "";
    }
}

Readiness EvaluateReadiness(bool handlerPresent, const Context& context)
{
    // 1) 没有提交适配器：这一版对话窗还发不出去，如实说明并可指向 Help 里的现有功能。
    if (!handlerPresent)
    {
        Readiness readiness;
        readiness.ready       = false;
        readiness.reason      = Reject::NotReady;
        readiness.explanation = "此对话窗还没有接入教学服务，暂时不能发送。"
                                "需要立即提问时，可使用 Help 菜单中的现有教学功能。";
        return readiness;
    }
    // 2) 没有工程：教学助手要结合当前设计回答，缺工程就不发。
    if (context.projectId.empty())
    {
        Readiness readiness;
        readiness.ready       = false;
        readiness.reason      = Reject::NotReady;
        readiness.explanation = "还没有打开工程：教学助手需要结合当前设计回答。";
        return readiness;
    }
    // 3) 没有 revision：版本未确认，不能把问题绑到不确定的设计版本上。
    if (context.revision.empty())
    {
        Readiness readiness;
        readiness.ready       = false;
        readiness.reason      = Reject::NotReady;
        readiness.explanation = "工程版本还没有确认，暂时不能提交。";
        return readiness;
    }
    // 4) 三个前提都满足。
    Readiness ready;
    ready.ready       = true;
    ready.reason      = Reject::None;
    ready.explanation = std::string();
    return ready;
}

const char* ReceiptToken(ReceiptStatus status)
{
    switch (status)
    {
        case ReceiptStatus::Accepted:
            return "accepted";
        case ReceiptStatus::Rejected:
            return "rejected";
        case ReceiptStatus::Busy:
            return "busy";
        case ReceiptStatus::NotReady:
            return "not-ready";
        default:
            return "not-ready";
    }
}

void ComposerState::SetDraft(std::string text)
{
    if (m_draft != text) ++m_draftRevision;
    m_draft = std::move(text);
}

void ComposerState::SetBusy(bool busy)
{
    m_busy = busy;
}

void ComposerState::SetContext(agent::gui::submit::Context context)
{
    m_context = std::move(context);
}

void ComposerState::SetReadiness(agent::gui::submit::Readiness readiness)
{
    m_readiness = std::move(readiness);
}

ComposerState::Start ComposerState::Begin(std::uint64_t submissionId, std::string text,
                                          std::string* explanation)
{
    // 1) id 非法或重复使用：不产生第二次提交，也不改动任何状态。
    if (submissionId == 0 || submissionId <= m_lastStartedId)
    {
        if (explanation != nullptr)
        {
            *explanation = "重复的提交请求。";
        }
        return Start::DuplicateId;
    }

    // 2) 已有在途提交或回答进行中：串行化，避免同一会话里两个问题抢跑。
    if (m_busy || m_pending)
    {
        if (explanation != nullptr)
        {
            *explanation = RejectText(Reject::Busy);
        }
        return Start::Busy;
    }

    // 3) 合同校验：空白优先于长度（空白问题不因"够长"而放行）。
    const Reject verdict = Validate(text);
    if (verdict == Reject::Empty)
    {
        if (explanation != nullptr)
        {
            *explanation = RejectText(Reject::Empty);
        }
        return Start::Empty;
    }
    if (verdict == Reject::TooLong)
    {
        if (explanation != nullptr)
        {
            *explanation = RejectText(Reject::TooLong);
        }
        return Start::TooLong;
    }

    // 4) 门禁：说明由 Readiness 给出。若调用方手工塞了空文案，退回通用文案，
    //    保证 NotReady 的说明永不为空（EvaluateReadiness 产出的说明本来就是非空的）。
    if (!m_readiness.ready)
    {
        if (explanation != nullptr)
        {
            *explanation = m_readiness.explanation.empty() ? RejectText(Reject::NotReady)
                                                           : m_readiness.explanation;
        }
        return Start::NotReady;
    }

    // 5) 冻结快照：文本与上下文都在此刻定格，之后宿主改上下文也不影响本次提交。
    //    不清草稿 —— 草稿只在受理回执落地时才清（见 Settle）。
    m_intent.submissionId = submissionId;
    m_intent.text         = std::move(text);
    m_intent.context      = m_context;
    m_submittedDraftRevision = m_draftRevision;
    m_pending             = true;
    m_lastStartedId       = submissionId;
    ++m_starts;
    if (explanation != nullptr)
    {
        explanation->clear();
    }
    return Start::Started;
}

ComposerState::SettleResult ComposerState::Settle(std::uint64_t submissionId,
                                                  ReceiptStatus status, std::string* explanation)
{
    (void)explanation;  // 本层不生产回执文案：非受理原因由调用方自带的说明承担，绝不编造。

    // 1) 同一个 id 的重复回执：无副作用（重复 Accepted 不能把新草稿再清一次）。
    if (m_hasSettled && submissionId == m_lastSettledId)
    {
        return SettleResult::Duplicate;
    }

    // 2) 不是当前在途快照：迟到回执、Abandon 之后到达，或请求已切换 → Stale。
    if (!m_pending || submissionId != m_intent.submissionId)
    {
        return SettleResult::Stale;
    }

    // 3) 回执确实指向当前快照，开始结算。
    SettleResult outcome = SettleResult::KeptDraft;
    const bool sameContext = m_context.projectId == m_intent.context.projectId &&
                             m_context.revision == m_intent.context.revision &&
                             m_context.sessionId == m_intent.context.sessionId;
    if (status == ReceiptStatus::Accepted && m_draft == m_intent.text &&
        m_draftRevision == m_submittedDraftRevision && sameContext)
    {
        // 只有"草稿仍是本次提交的那一份"才清；否则说明用户已在写下一问。
        m_draft.clear();
        ++m_draftRevision;
        outcome = SettleResult::ClearedDraft;
    }

    m_pending       = false;
    m_lastSettledId = submissionId;
    m_hasSettled    = true;
    ++m_settles;
    return outcome;
}

void ComposerState::Abandon()
{
    // 请求切换 / 视图销毁：只放弃在途快照，草稿与计数都不动。
    // 之后同一个 id 的回执必然走 Stale 分支，不会误清新草稿。
    m_pending = false;
}

}  // namespace agent::gui::submit
