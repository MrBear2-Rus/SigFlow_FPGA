//
// AgentViewModel.cpp — AGUI-T03 类型化 ViewModel 的纯状态实现
//
// 本文件只依赖标准库。没有 wx、没有 jsoncpp、没有网络、没有文件、没有日志、
// 没有线程/定时器。唯一的「外部世界」接口是调用方喂进来的值类型。
//
// 判定顺序是接口语义的一部分（测试据此断言）。逐条理由：
//
//   ApplyRunResult(ctx, snapshot)
//     1 WrongThread         非 owner 线程写入会与 UI 线程竞争，必须在写之前拒绝。
//     2 DemoLocked          演示模式不接受 live 事实（DEC-10 演示与真实不可混淆）。
//     3 DemoPayload         live 入口收到 Origin::Demo 的卡片/证据：拒收，**不洗标签**。
//     4 StaleGeneration     旧请求的迟到结果（B02）。generation 先判，理由最具体。
//     5 Foreign*            project/revision/session/run 与当前绑定不符；
//                          空串不是通配符；同会话的旧 run 迟到属 ForeignRun。
//     6 ForeignLearner      B04 越权 scope：绑定 learner 后，缺失或不同都拒绝。
//     7 LocallyDiscarded    该请求已被本地丢弃（v1 无 cancel，后端可能仍在跑）。
//     8 Rejected(版本非法)  不合法版本不能参与比较（U2 前导零/≥2^63 等）。
//     9 Regressed           终态不可回退/改写（先于版本比较，防止"更新版本"覆盖终态）。
//    10 Regressed           版本更旧。
//    11 Rejected(跨族)      版本族不相容：v1 十进制与 U2 整数之间没有"更新"语义。
//    12–14 水合            同 (revision, version, status) 的权威完整结果只补齐一次；
//                          同版本不同内容保留先到数据（旧数据不得覆盖新数据）。
//
//   ApplyEvent(ctx, event)
//     序号族（v1 十进制 sequence / U2 整数 sequence）在会话内不能混用；去重键是
//     event_id 或 sequence，**action_id 不是事件唯一 ID**（同一 action 的
//     stage_verification 与 commit 会发多条不同事件），只作审计记录。
//     无状态事件（status=Unknown 且无 rawStatus，如 v1 session/created）只推进游标，
//     绝不改动运行状态；带未知状态原文的事件一律 Rejected（保留原文）。
//
// 为什么把「未知」当拒绝而不是成功：Unknown 既不是终态也不是成功（RunStatus 注释），
// 若把它当成功，一次拼写错误的后端状态就会让界面显示"完成"。
//

#include "agent/gui/AgentViewModel.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace agent::gui::vm {

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

struct WireStatusEntry
{
    const char* wire;
    RunStatus status;
};

/// 严格区分大小写：v1 是小写三态，U2 是 PascalCase 九态。未列出的一律 Unknown。
constexpr WireStatusEntry kWireStatuses[] = {
    {"succeeded", RunStatus::Succeeded},
    {"degraded", RunStatus::Degraded},
    {"failed", RunStatus::Failed},
    {"Running", RunStatus::Running},
    {"WaitingStudent", RunStatus::WaitingStudent},
    {"WaitingEvidence", RunStatus::WaitingEvidence},
    {"WaitingApproval", RunStatus::WaitingApproval},
    {"RecoveryRequired", RunStatus::RecoveryRequired},
    {"Completed", RunStatus::Completed},
    {"Cancelled", RunStatus::Cancelled},
    {"Failed", RunStatus::Failed},
    {"BudgetExceeded", RunStatus::BudgetExceeded},
};

/// 请求结束态。BudgetExceeded 是终态但不算成功，按 Failed 表达（界面用自己的文案）。
RequestState RequestStateForStatus(RunStatus status)
{
    switch (status)
    {
    case RunStatus::Completed:
    case RunStatus::Succeeded:
    case RunStatus::Degraded:
        return RequestState::Completed;
    case RunStatus::Cancelled:
        return RequestState::Cancelled;
    case RunStatus::Failed:
    case RunStatus::BudgetExceeded:
    case RunStatus::Unknown:
        return RequestState::Failed;
    case RunStatus::Running:
    case RunStatus::WaitingStudent:
    case RunStatus::WaitingEvidence:
    case RunStatus::WaitingApproval:
    case RunStatus::RecoveryRequired:
        return RequestState::Busy;
    }
    return RequestState::Failed;
}

/// 十进制字符串比较：等长时字典序即数值序；不做任何数值转换。
VersionOrder CompareDecimalStrings(const std::string& lhs, const std::string& rhs)
{
    if (lhs.size() != rhs.size())
    {
        return lhs.size() < rhs.size() ? VersionOrder::Older : VersionOrder::Newer;
    }
    if (lhs == rhs)
    {
        return VersionOrder::Same;
    }
    return lhs < rhs ? VersionOrder::Older : VersionOrder::Newer;
}

/// 身份校验。返回 Applied 表示通过，否则是被拒绝的具体原因。
ApplyResult CheckIdentity(const std::string& projectId, const std::string& revision,
                          std::uint64_t generation, const std::string& sessionId,
                          const std::string& runId, const std::string& learnerId,
                          const RequestContext& ctx, std::string* reason)
{
    if (ctx.generation != generation)
    {
        *reason = "旧 generation：信封 " + std::to_string(ctx.generation) + "，当前 " +
                  std::to_string(generation);
        return ApplyResult::StaleGeneration;
    }
    if (ctx.projectId.empty() || ctx.projectId != projectId)
    {
        *reason = "项目不匹配：信封 '" + ctx.projectId + "'，当前 '" + projectId +
                  "'（未绑定项目时一律拒绝）";
        return ApplyResult::ForeignProject;
    }
    if (ctx.revision != revision)
    {
        *reason = "revision 已变：信封 '" + ctx.revision + "'，当前 '" + revision + "'";
        return ApplyResult::ForeignRevision;
    }
    if (ctx.sessionId.empty() || ctx.sessionId != sessionId)
    {
        *reason = "会话不匹配：信封 '" + ctx.sessionId + "'，当前 '" + sessionId +
                  "'（空会话 id 不是通配符）";
        return ApplyResult::ForeignSession;
    }
    if (ctx.runId.empty() || ctx.runId != runId)
    {
        *reason = "run 不匹配：信封 '" + ctx.runId + "'，当前 '" + runId +
                  "'（同会话的旧 run 迟到写入不生效）";
        return ApplyResult::ForeignRun;
    }
    // learner scope：绑定非空时，**缺失或不同都拒绝**（不能只比"两边非空"）。
    if (!learnerId.empty())
    {
        if (ctx.learnerId.empty())
        {
            *reason = "learner scope 缺失：绑定 '" + learnerId + "'，但投递信封没有 learner";
            return ApplyResult::ForeignLearner;
        }
        if (ctx.learnerId != learnerId)
        {
            *reason = "learner scope 不符：信封 '" + ctx.learnerId + "'，当前 '" + learnerId + "'";
            return ApplyResult::ForeignLearner;
        }
    }
    return ApplyResult::Applied;
}

bool CardIsDemoPayload(const CardView& card)
{
    if (card.origin == Origin::Demo)
    {
        return true;
    }
    for (const EvidenceView& evidence : card.evidence)
    {
        if (evidence.origin == Origin::Demo)
        {
            return true;
        }
    }
    return false;
}

bool CardsHaveDemoPayload(const std::vector<CardView>& cards)
{
    for (const CardView& card : cards)
    {
        if (CardIsDemoPayload(card))
        {
            return true;
        }
    }
    return false;
}

/// 可信适配器未标注（Unknown）时补成 Live；Demo 已在调用点拒收，绝不在这里"洗"。
void MarkCardOriginLive(CardView* card)
{
    if (card->origin == Origin::Unknown)
    {
        card->origin = Origin::Live;
    }
    for (EvidenceView& evidence : card->evidence)
    {
        if (evidence.origin == Origin::Unknown)
        {
            evidence.origin = Origin::Live;
        }
    }
}

void MarkCardsOriginLive(std::vector<CardView>* cards)
{
    for (CardView& card : *cards)
    {
        MarkCardOriginLive(&card);
    }
}

/// 增量卡片合并：按 card_id 替换，且只接受**更新**的卡片版本；
/// 更旧或族不相容时保留现有内容（不回退、不混族）。
void MergeCards(std::vector<CardView>* destination, const std::vector<CardView>& incoming,
                const std::string& receiptRevision)
{
    for (const CardView& card : incoming)
    {
        CardView copy = card;
        copy.receiptRevision = receiptRevision;
        MarkCardOriginLive(&copy);
        bool replaced = false;
        for (CardView& existing : *destination)
        {
            if (!existing.cardId.empty() && existing.cardId == copy.cardId)
            {
                if (CompareVersions(copy.stateVersion, existing.stateVersion) == VersionOrder::Newer)
                {
                    existing = copy;
                }
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            destination->push_back(std::move(copy));
        }
    }
}

/// 权威完整结果的指纹：用于区分"同一份结果重复投递"与"同版本冲突内容"。
std::string FingerprintSnapshot(const RunSnapshot& snapshot)
{
    std::string out;
    out.reserve(512);
    out += snapshot.version.KindToken();
    out += ':';
    out += snapshot.version.CanonicalText();
    out += '|';
    out += RunStatusToken(snapshot.status);
    out += '|';
    out += snapshot.rawStatus;
    out += '|';
    out += snapshot.modelUsed;
    out += '|';
    out += snapshot.reason;
    out += '|';
    for (const std::string& limitation : snapshot.limitations)
    {
        out += limitation;
        out += '\x1f';
    }
    out += '|';
    // 计划也属于这份权威结果：同版本但内容不同的 plan-card 不能被当成重复投递，
    // 否则新版计划会被幂等吞掉。这里只拼投影后的安全字段（没有 params/路径）。
    out += snapshot.plan.present ? "plan" : "no-plan";
    out += ':';
    out += snapshot.plan.planId;
    out += ':';
    out += snapshot.plan.goal;
    out += ':';
    out += snapshot.plan.revision;
    out += ':';
    out += snapshot.plan.stateVersion;
    out += ':';
    out += std::to_string(snapshot.plan.maxJobs);
    out += ':';
    out += snapshot.plan.requiresApproval ? '1' : '0';
    out += ':';
    out += snapshot.plan.snapshotRequired ? '1' : '0';
    out += ':';
    out += snapshot.plan.origin == Origin::Demo ? "demo" : "live";
    for (const PlanStepView& step : snapshot.plan.steps)
    {
        out += '\x1e';
        out += step.stepId;
        out += ':';
        out += step.capability;
        out += ':';
        out += step.title;
        out += ':';
        out += step.detail;
        out += ':';
        out += step.state;
    }
    out += '\x1d';
    for (const std::string& condition : snapshot.plan.stopConditions)
    {
        out += condition;
        out += '\x1f';
    }
    out += '\x1d';
    for (const std::string& limitation : snapshot.plan.limitations)
    {
        out += limitation;
        out += '\x1f';
    }
    out += '|';
    // 课程引用同样属于这份权威结果：同版本但不同的 course_refs 不能被当成重复投递。
    out += snapshot.course.origin == Origin::Demo ? "course-demo" : "course-live";
    for (const CourseRefView& courseRef : snapshot.course.refs)
    {
        out += '\x1e';
        out += courseRef.courseId;
        out += ':';
        out += courseRef.chapter;
        out += ':';
        out += courseRef.title;
        out += ':';
        out += courseRef.url;
        out += ':';
        out += courseRef.passageId;
        out += ':';
        out += courseRef.ref;
    }
    out += '|';
    // 验证进度也属于这份权威结果：同版本不同 verification 不能被幂等吞掉。
    out += snapshot.verification.present ? "verification" : "no-verification";
    out += ':';
    out += std::to_string(snapshot.verification.completedSteps);
    out += ':';
    out += std::to_string(snapshot.verification.stepCount);
    out += ':';
    out += snapshot.verification.hasLastObservedStep
               ? std::to_string(snapshot.verification.lastObservedStepIndex)
               : std::string("-");
    out += ':';
    out += snapshot.verification.jobId;
    out += ':';
    out += snapshot.verification.jobState;
    out += ':';
    out += snapshot.verification.resultEvidenceReady ? '1' : '0';
    out += ':';
    out += snapshot.verification.canObserve ? '1' : '0';
    out += '|';
    // prediction 的可见摘要同样属于这份权威结果。
    out += snapshot.prediction.present ? "prediction" : "no-prediction";
    out += ':';
    out += snapshot.prediction.status;
    out += ':';
    out += snapshot.prediction.reason;
    out += ':';
    out += snapshot.prediction.artifactId;
    out += ':';
    out += std::to_string(snapshot.prediction.signalId);
    out += ':';
    out += snapshot.prediction.signalFullName;
    out += ':';
    out += snapshot.prediction.startTick;
    out += ':';
    out += snapshot.prediction.endTick;
    out += ':';
    out += snapshot.prediction.sampleTick;
    out += ':';
    out += std::to_string(snapshot.prediction.width);
    out += ':';
    out += snapshot.prediction.timescale;
    out += ':';
    out += snapshot.prediction.outcome;
    out += ':';
    out += snapshot.prediction.origin;
    out += ':';
    out += snapshot.prediction.evidenceId;
    out += ':';
    out += snapshot.prediction.limitation;
    out += '|';
    for (const CardView& card : snapshot.cards)
    {
        out += card.cardId;
        out += ':';
        out += card.stateVersion.CanonicalText();
        out += ':';
        out += card.kind;
        out += ':';
        out += card.title;
        out += ':';
        out += card.body;
        out += '\x1e';
        for (const EvidenceView& evidence : card.evidence)
        {
            out += evidence.kind;
            out += ':';
            out += evidence.ref;
            out += ':';
            out += evidence.note;
            out += ':';
            // 波形元数据也属于这份权威结果：同版本不同波形窗口不能被幂等吞掉。
            out += evidence.waveform.hasMetadata ? "waveform" : "no-waveform";
            out += ':';
            out += evidence.waveform.artifactId;
            out += ':';
            out += std::to_string(evidence.waveform.signalId);
            out += ':';
            out += evidence.waveform.signalName;
            out += ':';
            out += evidence.waveform.startTick;
            out += ':';
            out += evidence.waveform.endTick;
            out += ':';
            out += evidence.waveform.timescale;
            out += ':';
            out += evidence.waveform.completeness;
            out += ':';
            out += std::to_string(evidence.waveform.transitionCount);
            out += '\x1d';
        }
    }
    return out;
}

std::string JoinNumbers(std::uint64_t lhs, const char* separator, std::uint64_t rhs)
{
    return std::to_string(lhs) + separator + std::to_string(rhs);
}

}  // namespace

// ===========================================================================
// 1. 标识文本
// ===========================================================================

const char* ModeToken(AppMode mode)
{
    switch (mode)
    {
    case AppMode::Learning:
        return "learning";
    case AppMode::Working:
        return "working";
    }
    return "unknown";
}

const char* FormToken(Form form)
{
    switch (form)
    {
    case Form::Center:
        return "center";
    case Form::Dock:
        return "dock";
    case Form::Float:
        return "float";
    case Form::Pet:
        return "pet";
    }
    return "unknown";
}

const char* AvailabilityToken(Availability availability)
{
    switch (availability)
    {
    case Availability::Unknown:
        return "unknown";
    case Availability::Unavailable:
        return "unavailable";
    case Availability::Loading:
        return "loading";
    case Availability::Ready:
        return "ready";
    case Availability::Failed:
        return "failed";
    }
    return "unknown";
}

const char* OriginToken(Origin origin)
{
    switch (origin)
    {
    case Origin::Unknown:
        return "unknown";
    case Origin::Live:
        return "live";
    case Origin::Demo:
        return "demo";
    }
    return "unknown";
}

const char* CapabilityKeyToken(CapabilityKey key)
{
    switch (key)
    {
    case CapabilityKey::Course:
        return "course";
    case CapabilityKey::TeachingCore:
        return "teaching_core";
    case CapabilityKey::Model:
        return "model";
    case CapabilityKey::EdaExecution:
        return "execution";
    case CapabilityKey::Record:
        return "record";
    case CapabilityKey::BackendCancel:
        return "cancel";
    case CapabilityKey::Checkers:
        return "checkers";
    }
    return "unknown";
}

const char* VerdictToken(CheckerView::Verdict verdict)
{
    switch (verdict)
    {
    case CheckerView::Verdict::Unknown:
        return "unknown";
    case CheckerView::Verdict::NotRun:
        return "not_run";
    case CheckerView::Verdict::Pass:
        return "pass";
    case CheckerView::Verdict::Fail:
        return "fail";
    case CheckerView::Verdict::Skipped:
        return "skipped";
    }
    return "unknown";
}

const char* RequestStateToken(RequestState state)
{
    switch (state)
    {
    case RequestState::Idle:
        return "idle";
    case RequestState::Busy:
        return "busy";
    case RequestState::CancellingLocal:
        return "cancelling_local";
    case RequestState::CancelRequested:
        return "cancel_requested";
    case RequestState::Cancelled:
        return "cancelled";
    case RequestState::Completed:
        return "completed";
    case RequestState::Failed:
        return "failed";
    }
    return "unknown";
}

const char* ApplyResultToken(ApplyResult result)
{
    switch (result)
    {
    case ApplyResult::Applied:
        return "applied";
    case ApplyResult::Duplicated:
        return "duplicated";
    case ApplyResult::StaleGeneration:
        return "stale_generation";
    case ApplyResult::ForeignProject:
        return "foreign_project";
    case ApplyResult::ForeignRevision:
        return "foreign_revision";
    case ApplyResult::ForeignSession:
        return "foreign_session";
    case ApplyResult::ForeignRun:
        return "foreign_run";
    case ApplyResult::ForeignLearner:
        return "foreign_learner";
    case ApplyResult::Regressed:
        return "regressed";
    case ApplyResult::LocallyDiscarded:
        return "locally_discarded";
    case ApplyResult::DemoLocked:
        return "demo_locked";
    case ApplyResult::LiveLocked:
        return "live_locked";
    case ApplyResult::DemoPayload:
        return "demo_payload";
    case ApplyResult::WrongThread:
        return "wrong_thread";
    case ApplyResult::Rejected:
        return "rejected";
    }
    return "unknown";
}

// ===========================================================================
// 2. Capability
// ===========================================================================

Capability Capability::FromReadyFlag(bool ready, std::string reason, std::string detail)
{
    Capability capability;
    capability.state = ready ? Availability::Ready : Availability::Unavailable;
    capability.reason = std::move(reason);
    capability.detail = std::move(detail);
    return capability;
}

Capability Capability::Unknown(std::string reason, std::string detail)
{
    Capability capability;
    capability.state = Availability::Unknown;
    capability.reason = std::move(reason);
    capability.detail = std::move(detail);
    return capability;
}

Capability Capability::Unavailable(std::string reason, std::string detail)
{
    Capability capability;
    capability.state = Availability::Unavailable;
    capability.reason = std::move(reason);
    capability.detail = std::move(detail);
    return capability;
}

// ===========================================================================
// 3. Version
// ===========================================================================

Version Version::Unknown()
{
    return Version{};
}

bool Version::NormalizeDecimal(std::string_view text, std::string* canonical)
{
    if (text.empty())
    {
        return false;
    }
    std::size_t index = 0;
    while (index < text.size() && text[index] == '0')
    {
        ++index;
    }
    if (index == text.size())
    {
        if (canonical != nullptr)
        {
            *canonical = "0";
        }
        return true;
    }
    std::string digits(text.substr(index));
    for (const char ch : digits)
    {
        if (ch < '0' || ch > '9')
        {
            return false;
        }
    }
    if (canonical != nullptr)
    {
        *canonical = std::move(digits);
    }
    return true;
}

bool Version::ParseDecimalInt64(std::string_view text, std::int64_t* out)
{
    if (out != nullptr)
    {
        *out = 0;
    }
    if (text.empty())
    {
        return false;
    }
    std::int64_t value = 0;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return false;
        }
        const int digit = static_cast<int>(ch - '0');
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (out != nullptr)
    {
        *out = value;
    }
    return true;
}

namespace {

/// 十进制族的共同构造：任意长度，规范化后比较。
Version MakeDecimalVersion(Version::Kind kind, std::string_view text)
{
    Version version;
    version.kind = kind;
    version.raw = std::string(text);
    version.valid = Version::NormalizeDecimal(text, &version.canonical);
    if (!version.valid)
    {
        version.canonical.clear();
        return version;
    }
    version.fitsInt64 = Version::ParseDecimalInt64(version.canonical, &version.value);
    if (!version.fitsInt64)
    {
        version.value = 0;
    }
    return version;
}

}  // namespace

Version Version::FromV1Sequence(std::string_view text)
{
    return MakeDecimalVersion(Kind::V1Sequence, text);
}

Version Version::FromV1EventSequence(std::string_view text)
{
    return MakeDecimalVersion(Kind::V1EventSequence, text);
}

Version Version::FromU2StateVersion(std::string_view text)
{
    // u2-state.schema.json：^(0|[1-9][0-9]{0,18})$，服务端另外强制 < 2^63。
    // 因此前导零（"007"）、超过 19 位、>= 2^63 都不合法：不放过、不截断。
    Version version;
    version.kind = Kind::U2StateVersion;
    version.raw = std::string(text);
    if (text.empty() || text.size() > 19)
    {
        return version;
    }
    if (text.size() > 1 && text.front() == '0')
    {
        return version;
    }
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return version;
        }
    }
    std::int64_t value = 0;
    if (!ParseDecimalInt64(text, &value))
    {
        return version;   // 数值 >= 2^63
    }
    version.canonical = std::string(text);
    version.value = value;
    version.fitsInt64 = true;
    version.valid = true;
    return version;
}

Version Version::FromU2Sequence(std::int64_t value)
{
    Version version;
    version.kind = Kind::U2Sequence;
    version.value = value;
    version.raw = std::to_string(value);
    version.canonical = version.raw;
    version.fitsInt64 = true;
    version.valid = value >= 0;
    return version;
}

Version Version::FromU2SequenceText(std::string_view text)
{
    Version version;
    version.kind = Kind::U2Sequence;
    version.raw = std::string(text);
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
    {
        return version;
    }
    std::int64_t value = 0;
    if (!ParseDecimalInt64(text, &value))
    {
        return version;
    }
    version.value = value;
    version.canonical = std::string(text);
    version.fitsInt64 = true;
    version.valid = value >= 0;
    return version;
}

const char* Version::KindToken() const
{
    switch (kind)
    {
    case Kind::Unknown:
        return "unknown";
    case Kind::V1Sequence:
        return "v1_sequence";
    case Kind::V1EventSequence:
        return "v1_event_sequence";
    case Kind::U2StateVersion:
        return "u2_state_version";
    case Kind::U2Sequence:
        return "u2_sequence";
    }
    return "unknown";
}

std::string Version::ToText() const
{
    if (!raw.empty())
    {
        return raw;
    }
    if (!canonical.empty())
    {
        return canonical;
    }
    return "unknown";
}

bool Version::operator==(const Version& rhs) const
{
    if (kind != rhs.kind)
    {
        return false;
    }
    if (kind == Kind::Unknown)
    {
        return true;
    }
    if (!valid || !rhs.valid)
    {
        return valid == rhs.valid && raw == rhs.raw;
    }
    return canonical == rhs.canonical;
}

bool SameVersionFamily(const Version& lhs, const Version& rhs)
{
    return lhs.kind != Version::Kind::Unknown && lhs.kind == rhs.kind;
}

VersionOrder CompareVersions(const Version& lhs, const Version& rhs)
{
    const bool lhsKnown = lhs.known();
    const bool rhsKnown = rhs.known();
    // "没有版本"（kind==Unknown）是最旧的占位，不是任何一个族：这样"事件先到、
    // 完整结果后到"能被正确识别为更新，而不会因为族不同被当成不相容。
    if (lhs.kind == Version::Kind::Unknown && rhs.kind == Version::Kind::Unknown)
    {
        return VersionOrder::Same;
    }
    if (lhs.kind == Version::Kind::Unknown)
    {
        return VersionOrder::Older;
    }
    if (rhs.kind == Version::Kind::Unknown)
    {
        return VersionOrder::Newer;
    }
    // 两个都不是占位：任一非法（格式不合合同）则不可比较。
    if (!lhsKnown || !rhsKnown)
    {
        return VersionOrder::Incomparable;
    }
    if (lhs.kind != rhs.kind)
    {
        // 跨族没有"更新"语义：绝不按枚举顺序比较。
        return VersionOrder::Incomparable;
    }
    if (lhs.kind == Version::Kind::U2Sequence)
    {
        if (lhs.value < rhs.value)
        {
            return VersionOrder::Older;
        }
        if (lhs.value > rhs.value)
        {
            return VersionOrder::Newer;
        }
        return VersionOrder::Same;
    }
    return CompareDecimalStrings(lhs.canonical, rhs.canonical);
}

// ===========================================================================
// 4. RunStatus
// ===========================================================================

RunStatus RunStatusFromWire(std::string_view wire, std::string* rawOut)
{
    if (rawOut != nullptr)
    {
        *rawOut = std::string(wire);
    }
    for (const WireStatusEntry& entry : kWireStatuses)
    {
        if (wire == entry.wire)
        {
            return entry.status;
        }
    }
    return RunStatus::Unknown;
}

const char* RunStatusToken(RunStatus status)
{
    switch (status)
    {
    case RunStatus::Unknown:
        return "unknown";
    case RunStatus::Running:
        return "Running";
    case RunStatus::WaitingStudent:
        return "WaitingStudent";
    case RunStatus::WaitingEvidence:
        return "WaitingEvidence";
    case RunStatus::WaitingApproval:
        return "WaitingApproval";
    case RunStatus::RecoveryRequired:
        return "RecoveryRequired";
    case RunStatus::Completed:
        return "Completed";
    case RunStatus::Cancelled:
        return "Cancelled";
    case RunStatus::Failed:
        return "failed";
    case RunStatus::BudgetExceeded:
        return "BudgetExceeded";
    case RunStatus::Succeeded:
        return "succeeded";
    case RunStatus::Degraded:
        return "degraded";
    }
    return "unknown";
}

bool IsTerminal(RunStatus status)
{
    switch (status)
    {
    case RunStatus::Completed:
    case RunStatus::Cancelled:
    case RunStatus::Failed:
    case RunStatus::BudgetExceeded:
    case RunStatus::Succeeded:
    case RunStatus::Degraded:
        return true;
    case RunStatus::Unknown:
    case RunStatus::Running:
    case RunStatus::WaitingStudent:
    case RunStatus::WaitingEvidence:
    case RunStatus::WaitingApproval:
    case RunStatus::RecoveryRequired:
        return false;
    }
    return false;
}

bool IsWaiting(RunStatus status)
{
    return status == RunStatus::WaitingStudent || status == RunStatus::WaitingEvidence ||
           status == RunStatus::WaitingApproval;
}

bool IsSuccess(RunStatus status)
{
    return status == RunStatus::Succeeded;
}

bool IsDegraded(RunStatus status)
{
    return status == RunStatus::Degraded;
}

// ===========================================================================
// 5. Level
// ===========================================================================

Level Level::Unbound()
{
    return Level{};
}

Level Level::V1(std::string text)
{
    Level level;
    level.kind = Kind::V1String;
    level.text = std::move(text);
    return level;
}

Level Level::U2(int integer)
{
    Level level;
    level.kind = Kind::U2Integer;
    level.integer = integer;
    return level;
}

const char* Level::KindToken() const
{
    switch (kind)
    {
    case Kind::Unbound:
        return "unbound";
    case Kind::V1String:
        return "v1_string";
    case Kind::U2Integer:
        return "u2_integer";
    }
    return "unknown";
}

std::string Level::ToText() const
{
    switch (kind)
    {
    case Kind::Unbound:
        return "未指定";
    case Kind::V1String:
        return text;
    case Kind::U2Integer:
        return std::to_string(integer);
    }
    return "未指定";
}

// ===========================================================================
// 6. RequestContext
// ===========================================================================

RequestContext::RequestContext()
    : projectId(), revision(), generation(0), sessionId(), runId(), learnerId(), level(Level::Unbound())
{
}

RequestContext::RequestContext(std::string projectIdValue, std::string revisionValue,
                               std::uint64_t generationValue, std::string sessionIdValue,
                               std::string runIdValue, std::string learnerIdValue, Level levelValue)
    : projectId(std::move(projectIdValue)),
      revision(std::move(revisionValue)),
      generation(generationValue),
      sessionId(std::move(sessionIdValue)),
      runId(std::move(runIdValue)),
      learnerId(std::move(learnerIdValue)),
      level(std::move(levelValue))
{
}

RequestContext RequestContext::WithTarget(std::string projectIdValue, std::string revisionValue) const
{
    return RequestContext(std::move(projectIdValue), std::move(revisionValue), generation, sessionId,
                          runId, learnerId, level);
}

RequestContext RequestContext::WithGeneration(std::uint64_t generationValue) const
{
    return RequestContext(projectId, revision, generationValue, sessionId, runId, learnerId, level);
}

RequestContext RequestContext::WithSession(std::string sessionIdValue) const
{
    return RequestContext(projectId, revision, generation, std::move(sessionIdValue), runId, learnerId,
                          level);
}

RequestContext RequestContext::WithRun(std::string runIdValue) const
{
    return RequestContext(projectId, revision, generation, sessionId, std::move(runIdValue), learnerId,
                          level);
}

RequestContext RequestContext::WithLearner(std::string learnerIdValue) const
{
    return RequestContext(projectId, revision, generation, sessionId, runId, std::move(learnerIdValue),
                          level);
}

RequestContext RequestContext::WithLevel(Level levelValue) const
{
    return RequestContext(projectId, revision, generation, sessionId, runId, learnerId,
                          std::move(levelValue));
}

// ===========================================================================
// 7. 视图辅助
// ===========================================================================

bool EvidenceView::IsHypothesis() const
{
    return kind == "hypothesis" || kind == "Hypothesis" || kind == "HYPOTHESIS";
}

bool EvidenceView::IsObservation() const
{
    return kind == "observation" || kind == "Observation" || kind == "OBSERVATION";
}

std::string EvidenceView::KindLabel() const
{
    return kind.empty() ? std::string("未知证据") : kind;
}

namespace {

/// 证据引用文本的硬上限：证据面板/对话行只显示短引用，不搬整份报告。
constexpr std::size_t kMaxReferenceTextBytes = 160;
/// 波形摘要包含 artifact + signal + ticks + completeness，单独给一个稍大的上限。
constexpr std::size_t kMaxWaveformReferenceBytes = 256;

/// 按 UTF-8 边界截断，避免把一个多字节序列切成非法 UTF-8（wxString::FromUTF8 会失败）。
std::string ClampReferenceText(std::string text, std::size_t maxBytes = kMaxReferenceTextBytes)
{
    if (text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    text.resize(cut);
    text += "...";
    return text;
}

/// 波形摘要片段：单行化，路径样内容替换为固定标记（防御性；投影层已校验）。
std::string WaveformPart(const std::string& value, std::size_t maxBytes, const char* fallback)
{
    if (value.empty()) return fallback;
    if (LooksLikeLocalPath(value)) return "（受限）";
    std::string out;
    out.reserve(value.size());
    for (const char ch : value)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        out.push_back(byte < 0x20u || byte == 0x7Fu ? ' ' : ch);
    }
    return ClampReferenceText(std::move(out), maxBytes);
}

}  // namespace

std::string EvidenceView::ReferenceText() const
{
    // 波形证据只显示受限元数据摘要；transitions/VCD 内容从不进入这里。
    if (waveform.hasMetadata)
    {
        std::string text = "波形：";
        text += WaveformPart(waveform.artifactId, 128, "未提供 artifact");
        text += " · ";
        text += WaveformPart(waveform.signalName, 128, "未提供信号");
        text += " · ticks ";
        text += WaveformPart(waveform.startTick, 20, "?");
        text += "-";
        text += WaveformPart(waveform.endTick, 20, "?");
        text += " · ";
        text += WaveformPart(waveform.completeness, 16, "未知");
        text += "（只读引用，不含波形内容）";
        return ClampReferenceText(std::move(text), kMaxWaveformReferenceBytes);
    }
    // 只由已投影的规范字段拼装：source_id / artifact_id / job_id 在投影层已通过
    // opaque ID 校验（无路径、无分隔符、无凭据文本）；这里不再猜测文件。
    std::string text;
    if (!sourceId.empty()) text = "source_id=" + sourceId;
    else if (!artifactId.empty()) text = "artifact_id=" + artifactId;
    else if (!jobId.empty()) text = "job_id=" + jobId;
    else text = ref;  // 既有字符串 ref：原样显示（兼容旧行为）
    if (hasLineRange && startLine >= 1 && endLine >= startLine)
    {
        if (!text.empty()) text += " · ";
        text += "lines=" + std::to_string(startLine) + "-" + std::to_string(endLine);
    }
    return ClampReferenceText(std::move(text));
}

// ---------------------------------------------------------------------------
// 验证计划分区（只读文本；无 wx、无网络、无审批/执行入口）
// ---------------------------------------------------------------------------

namespace {

/// 像本机绝对路径/父目录穿越：GUI 文本一律不显示（换成受限标记）。
std::string SingleLine(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (const char ch : value)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        out.push_back(byte < 0x20u || byte == 0x7Fu ? ' ' : ch);
    }
    return out;
}

std::string ClampUtf8Bytes(std::string text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
    {
        return text;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u)
    {
        --cut;
    }
    text.resize(cut);
    text += "…";
    return text;
}

/// 单行 + 有界 + 不含本机路径的展示片段。路径样文本整体替换为固定标记，
/// 不尝试解析或缩短路径（GUI 不需要也不应该看到它）。
std::string SafeDisplay(std::string text, std::size_t maxBytes, const char* fallback)
{
    text = SingleLine(text);
    if (LooksLikeLocalPath(text))
    {
        return "（受限内容已省略）";
    }
    text = ClampUtf8Bytes(std::move(text), maxBytes);
    return text.empty() ? std::string(fallback) : text;
}

std::string PlanStepStateLabel(const std::string& state)
{
    if (state.empty())
    {
        return "未声明";
    }
    if (state == "enabled")
    {
        return "可执行";
    }
    if (state == "disabled")
    {
        return "不可用";
    }
    return state;  // 其它原文（如 demo 的 pending）如实转述
}

}  // namespace

bool LooksLikeLocalPath(const std::string& text)
{
    // 任意路径分隔符（POSIX `/` 与 Windows `\`）都视为路径样式：相对路径
    // `rtl/top.v`、`src/foo.v` 同样不得出现在 GUI 计划文本里。
    if (text.find('/') != std::string::npos || text.find('\\') != std::string::npos)
    {
        return true;
    }
    // 分隔符检查之外仍保留盘符判断：`C:` 形式即使没有分隔符也像本机绝对路径。
    if (text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) != 0 &&
        text[1] == ':')
    {
        return true;
    }
    // 父目录穿越：`..` 单独出现也拒绝（路径分隔符形式已被上面的检查覆盖）。
    if (text.find("..") != std::string::npos)
    {
        return true;
    }
    return false;
}

PlanSectionView ProjectPlanSection(const PlanView& plan, const std::string& currentRevision,
                                   bool demoMode)
{
    PlanSectionView out;
    if (!plan.present)
    {
        out.summary = "尚未生成";
        out.body = "尚未收到当前代码版本的只读验证计划。\n"
                   "审批、授权与执行尚未接入：这里不会出现批准按钮，"
                   "也不会声称计划已被批准或已执行。";
        out.reason = "没有可信计划数据（本次运行没有 plan-card）。";
        return out;
    }
    const bool demo = plan.origin == Origin::Demo;
    if (demo && !demoMode)
    {
        out.summary = "演示计划已隐藏";
        out.body = "演示用验证计划不会显示在真实会话里。";
        out.reason = "演示数据不是真实计划。";
        return out;
    }
    if (!demo && plan.revision.empty())
    {
        out.summary = "计划缺少版本";
        out.body = "计划没有绑定代码版本，无法判断是否属于当前设计。";
        out.reason = "缺少可信的 revision 绑定。";
        return out;
    }
    if (!demo && !currentRevision.empty() && plan.revision != currentRevision)
    {
        out.summary = "版本不匹配";
        out.body = "计划绑定的代码版本与当前版本不一致，计划内容已隐藏。\n"
                   "请按当前版本重新生成计划；本页不提供批准或执行入口。";
        out.reason = "跨版本计划不显示（绑定版本：" +
                     SafeDisplay(plan.revision, 64, "未知") +
                     "，当前版本：" + SafeDisplay(currentRevision, 64, "未知") + "）。";
        return out;
    }
    if (plan.steps.empty())
    {
        out.summary = "计划缺少步骤";
        out.body = "这份计划没有可展示的步骤。";
        out.reason = "计划结构不完整。";
        return out;
    }

    std::string body;
    body += "计划 ID：" + SafeDisplay(plan.planId, 64, "未知") + "\n";
    body += "目标：" + SafeDisplay(plan.goal, 320, "未提供") + "\n";
    body += "绑定版本：" + SafeDisplay(plan.revision, 128, "未提供") + "\n";
    if (!plan.stateVersion.empty())
    {
        body += "状态版本：" + SafeDisplay(plan.stateVersion, 32, "未知") + "\n";
    }
    body += std::string("需要审批：") +
            (plan.requiresApproval ? "是（合同要求；本页不提供批准入口）" : "未声明") + "\n";
    body += std::string("快照必需：") + (plan.snapshotRequired ? "是" : "未声明") + "\n";
    body += "作业上限：" +
            (plan.maxJobs > 0 ? std::to_string(plan.maxJobs) : std::string("未声明")) + "\n";
    body += "\n步骤（" + std::to_string(plan.steps.size()) + "）：\n";
    int index = 0;
    for (const PlanStepView& step : plan.steps)
    {
        ++index;
        body += std::to_string(index) + ". " + SafeDisplay(step.stepId, 64, "未命名步骤");
        body += " · " + SafeDisplay(step.title, 200, "（未提供说明）") + "\n";
        if (!step.capability.empty())
        {
            body += "   能力：" + SafeDisplay(step.capability, 32, "未知") + "\n";
        }
        body += "   状态：" + SafeDisplay(PlanStepStateLabel(step.state), 32, "未声明") + "\n";
        if (!step.detail.empty())
        {
            body += "   说明：" + SafeDisplay(step.detail, 240, "") + "\n";
        }
    }
    if (!plan.stopConditions.empty())
    {
        body += "\n失败即停：\n";
        for (const std::string& condition : plan.stopConditions)
        {
            body += "- " + SafeDisplay(condition, 200, "") + "\n";
        }
    }
    body += "\n限制：\n";
    body += "- 仅查看：审批、授权与执行尚未接入，本页不会提交、批准、取消或执行任何 Job。\n";
    body += "- 计划内容来自后端 plan-card；参数未投影到界面，本页不显示路径、命令或脚本。\n";
    for (const std::string& limitation : plan.limitations)
    {
        body += "- " + SafeDisplay(limitation, 200, "") + "\n";
    }

    if (demo)
    {
        body += "\n（演示数据：不是真实计划，也不能执行。）\n";
        out.available = false;
        out.summary = "演示 · " + SafeDisplay(plan.planId, 64, "未命名计划") + " · " +
                      std::to_string(plan.steps.size()) + " 步";
        out.reason = "演示数据不是真实计划；审批与执行未接入。";
    }
    else
    {
        out.available = true;
        out.summary = SafeDisplay(plan.planId, 64, "计划") + " · " +
                      std::to_string(plan.steps.size()) + " 步";
        out.reason.clear();
    }
    out.body = std::move(body);
    return out;
}

// ---------------------------------------------------------------------------
// 课程引用分区（只读文本；无 wx、无网络、不可点击）
// ---------------------------------------------------------------------------

namespace {

/// 能力状态的中文标签 + 原因（原因经过 SafeDisplay：单行、有界、无路径）。
std::string CapabilityLabelText(const char* name, const Capability& capability)
{
    std::string label;
    switch (capability.state)
    {
    case Availability::Ready:
        label = "已就绪";
        break;
    case Availability::Loading:
        label = "加载中";
        break;
    case Availability::Failed:
        label = "加载失败";
        break;
    case Availability::Unknown:
        label = "未知";
        break;
    case Availability::Unavailable:
        label = "不可用";
        break;
    }
    std::string text = std::string(name) + "：" + label;
    if (!capability.reason.empty())
    {
        text += "（" + SafeDisplay(capability.reason, 160, "原因未知") + "）";
    }
    return text;
}

std::string CourseCapabilityText(const Capability& capability)
{
    return CapabilityLabelText("课程能力", capability);
}

/// 链接只以文本展示：只接受已校验的 https://，其它一律替换为固定标记。
/// 绝不把这个字符串变成可点击控件，也不发起任何请求。
std::string SafeCourseUrlText(const std::string& url, std::size_t maxBytes)
{
    if (url.empty())
    {
        return "未提供";
    }
    if (url.rfind("https://", 0) != 0 || url.find('\\') != std::string::npos ||
        url.find('@') != std::string::npos || url.find("://", 8) != std::string::npos)
    {
        return "（受限链接已省略）";
    }
    for (const char ch : url)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20u || byte == 0x7Fu)
        {
            return "（受限链接已省略）";
        }
    }
    return ClampUtf8Bytes(url, maxBytes);
}

}  // namespace

CourseSectionView ProjectCourseSection(const CourseView& course, const Capability& capability,
                                       const std::string& currentRevision, bool demoMode)
{
    CourseSectionView out;
    const std::string capabilityText = CourseCapabilityText(capability);
    const bool demo = course.origin == Origin::Demo;
    const bool stale = !demo && !currentRevision.empty() && !course.revision.empty() &&
                       course.revision != currentRevision;
    if (course.refs.empty())
    {
        out.summary = "尚未生成";
        out.body = "尚未收到当前代码版本的课程引用。\n" + capabilityText +
                   "\n课程检索、抓取与跳转尚未接入：这里只显示后端随卡片返回的只读引用元数据。";
        out.reason = "没有可信课程引用。";
        return out;
    }
    if (demo && !demoMode)
    {
        out.summary = "演示课程已隐藏";
        out.body = "演示用课程引用不会显示在真实会话里。";
        out.reason = "演示数据不是真实课程引用。";
        return out;
    }
    if (stale)
    {
        out.summary = "版本不匹配";
        out.body = "课程引用绑定的代码版本与当前版本不一致，内容已隐藏。\n"
                   "课程内容只读，本页不提供搜索、进度或跳转授权。";
        out.reason = "跨版本课程引用不显示（绑定版本：" +
                     SafeDisplay(course.revision, 64, "未知") +
                     "，当前版本：" + SafeDisplay(currentRevision, 64, "未知") + "）。";
        return out;
    }

    std::string body = "课程引用：" + std::to_string(course.refs.size()) + " 条（只读元数据）\n";
    body += capabilityText + "\n";
    int index = 0;
    for (const CourseRefView& ref : course.refs)
    {
        ++index;
        const std::string& courseId = ref.courseId.empty() ? ref.passageId : ref.courseId;
        body += "\n" + std::to_string(index) + ". " + SafeDisplay(courseId, 128, "未命名课程");
        if (!ref.chapter.empty())
        {
            body += " · " + SafeDisplay(ref.chapter, 128, "未分章");
        }
        body += "\n   标题：" + SafeDisplay(ref.title.empty() ? ref.passageId : ref.title, 256,
                                          "未提供标题");
        body += "\n   链接：" + SafeCourseUrlText(ref.url, 512) + "（只读文本，不会自动打开）";
        body += "\n";
    }
    body += "\n限制：\n";
    body += "- 只读摘要：课程正文未抓取、未缓存，不显示进度或掌握度。\n";
    body += "- 不提供课程搜索、跳转授权或任何网络请求；链接文本不可点击。\n";
    for (const std::string& limitation : course.limitations)
    {
        body += "- " + SafeDisplay(limitation, 200, "") + "\n";
    }
    if (demo)
    {
        body += "\n（演示数据：不是真实课程引用，能力与进度都不可用。）\n";
    }

    out.available = !demo && capability.IsReady();
    out.summary = (demo ? std::string("演示 · ") : std::string()) +
                  std::to_string(course.refs.size()) + " 条课程引用";
    out.body = std::move(body);
    if (demo)
    {
        out.reason = "演示数据不是真实课程引用；课程能力与进度不可用。";
    }
    else if (!capability.IsReady())
    {
        out.reason = capabilityText + "；仅显示只读引用元数据。";
    }
    else
    {
        out.reason.clear();
    }
    return out;
}

// ---------------------------------------------------------------------------
// 检查器 / 学习记录：仍无可信 wire 合同，只生成诚实不可用文本
// ---------------------------------------------------------------------------

namespace {

UnavailableSectionView ProjectUnavailableSection(const char* capabilityName,
                                                 const Capability& capability,
                                                 const std::string& intro,
                                                 const std::string& boundary)
{
    UnavailableSectionView out;
    out.summary = "尚未接通";
    const std::string capabilityText = CapabilityLabelText(capabilityName, capability);
    out.body = intro + "\n" + capabilityText + "\n" + boundary;
    out.reason = "没有可信的只读数据源（v1 合同未定义）；" + capabilityText;
    return out;
}

}  // namespace

UnavailableSectionView ProjectCheckerSection(const Capability& capability)
{
    return ProjectUnavailableSection(
        "检查器能力", capability,
        "检查器与状态尚未接入真实结果：v1 合同没有 checker 字段或聚合路由。",
        "边界：不把 rule_match / 诊断当作检查器通过；不显示静态全绿、分数或进度；"
        "不提供运行、执行或导出入口。");
}

UnavailableSectionView ProjectRecordSection(const Capability& capability)
{
    return ProjectUnavailableSection(
        "学习记录能力", capability,
        "学习记录尚未定义合同（DEC-07）：没有 record wire 字段或只读路由。",
        "边界：不显示错题、统计、画像或历史；不提供导出、删除或历史写入。");
}

LocationActionView ProjectLocationAction(bool trustedResolverAttached, bool eligibleTarget)
{
    LocationActionView out;
    if (!trustedResolverAttached)
    {
        // 默认边界：v1 没有受信 NavigationBridge/定位合同，按钮保持禁用并说明原因。
        out.reason = "来源定位尚未接入：缺少受信 NavigationBridge；"
                     "本页不访问文件、不猜测路径，也不会自动打开任何位置。";
        return out;
    }
    if (!eligibleTarget)
    {
        out.reason = "该证据没有可定位的当前版本目标（只读引用，不含路径）。";
        return out;
    }
    out.available = true;
    return out;
}

RecoveryActionView ProjectRecoveryActions(const std::string& teachingStatus,
                                          bool activeTeachingRun, bool busy)
{
    RecoveryActionView out;
    if (!activeTeachingRun)
    {
        out.recoverReason = "当前没有进行中的教学运行：刷新证据与恢复教学都不可用。";
        return out;
    }
    // native host 只在 WaitingEvidence 接受 kind=refresh_evidence；
    // Running/WaitingStudent/WaitingApproval/RecoveryRequired 下点它必然 action_denied，
    // 因此这些状态一律不显示该按钮（不是“禁用后仍可点”）。
    out.refreshEvidenceVisible = !busy && teachingStatus == "WaitingEvidence";
    out.refreshEvidenceEnabled = out.refreshEvidenceVisible;
    out.recoverVisible = teachingStatus == "RecoveryRequired";
    out.recoverEnabled = out.recoverVisible && !busy;
    if (!out.recoverVisible)
    {
        // 普通 WaitingStudent / WaitingEvidence / Running 都不是恢复入口。
        out.recoverReason = "后端未报告 RecoveryRequired：恢复教学不可用。";
    }
    else if (busy)
    {
        out.recoverReason = "教学请求进行中，完成后才能恢复。";
    }
    return out;
}

std::string VerificationSummaryText(const VerificationView& verification)
{    if (!verification.present)
    {
        return std::string();
    }
    // 只转述后端声明的字段：没有 detail/params/路径；也不把 ready/can_observe 说成成功。
    std::string text = "步骤：";
    text += std::to_string(verification.completedSteps);
    text += "/";
    text += std::to_string(verification.stepCount);
    text += "\n最近观察步骤：";
    text += verification.hasLastObservedStep
                ? "第 " + std::to_string(verification.lastObservedStepIndex + 1) + " 步"
                : std::string("未观察");
    text += "\njob_id：";
    text += verification.jobId.empty() ? std::string("未提供") : verification.jobId;
    text += "\njob_state：";
    text += verification.jobState.empty() ? std::string("未提供") : verification.jobState;
    text += "（后端声明）\n结果证据就绪：";
    text += verification.resultEvidenceReady ? "是（后端声明，仅展示）" : "否";
    text += "\n可观察：";
    text += verification.canObserve ? "是（后端声明；不触发执行）" : "否";
    text += "\n限制：只读；不等于已验证，也不表示通过；不触发执行、批准或授权。";
    return text;
}

std::string PredictionSummaryText(const PredictionView& prediction)
{
    if (!prediction.present)
    {
        return std::string();
    }
    // 状态/结果只作中文转述，避免把 raw `observed/passed` 之类词当成结论。
    const auto statusLabel = [](const std::string& status) -> std::string {
        if (status == "idle") return "空闲";
        if (status == "disabled") return "未启用";
        if (status == "awaiting_prediction") return "等待预测";
        if (status == "observing") return "观察中";
        if (status == "observed") return "已观察";
        if (status == "unavailable") return "不可用";
        if (status == "expired") return "已过期";
        if (status == "revalidation_required") return "需重新验证";
        if (status == "cancelled") return "已取消";
        return "未知状态";
    };
    const auto outcomeLabel = [](const std::string& outcome) -> std::string {
        if (outcome == "matches") return "一致";
        if (outcome == "differs") return "不一致";
        if (outcome == "inconclusive") return "无法判定";
        return "未知结果";
    };
    const auto originLabel = [](const std::string& origin) -> std::string {
        if (origin == "fixture") return "离线示例";
        if (origin == "tool") return "工具读取";
        return "未知来源";
    };
    std::string text = "后端状态：" + statusLabel(prediction.status) + "（后端声明）";
    if (!prediction.reason.empty()) text += "\n原因：" + prediction.reason;
    if (!prediction.artifactId.empty()) text += "\n产物：" + prediction.artifactId;
    text += "\n信号：";
    text += prediction.signalFullName.empty() ? std::string("未提供") : prediction.signalFullName;
    text += "（id：";
    text += prediction.signalId >= 0 ? std::to_string(prediction.signalId) : std::string("未提供");
    text += "）\n窗口 ticks：";
    text += prediction.startTick.empty() ? std::string("未提供") : prediction.startTick;
    text += "-";
    text += prediction.endTick.empty() ? std::string("未提供") : prediction.endTick;
    text += "，采样 ";
    text += prediction.sampleTick.empty() ? std::string("未提供") : prediction.sampleTick;
    text += "；timescale ";
    text += prediction.timescale.empty() ? std::string("未提供") : prediction.timescale;
    text += "；位宽 ";
    text += prediction.width > 0 ? std::to_string(prediction.width) : std::string("未提供");
    if (!prediction.outcome.empty())
    {
        text += "\n比较结果：" + outcomeLabel(prediction.outcome) + "（后端声明；仅该采样点）";
    }
    if (!prediction.origin.empty())
    {
        text += "\n证据来源：" + originLabel(prediction.origin) + "（后端声明）";
    }
    if (!prediction.evidenceId.empty()) text += "\n证据 ID：" + prediction.evidenceId;
    if (!prediction.limitation.empty()) text += "\n限制：" + prediction.limitation;
    text += "\n说明：只读；不触发预测提交、不执行工具、不授予权限；也不构成正确性结论。";
    return text;
}

// ===========================================================================
// 8. CapabilitySet
// ===========================================================================

CapabilitySet::CapabilitySet()
{
    course = Capability::Unavailable(
        "v1 /capabilities 报告 course.available=false；U2 课程未在生产暴露",
        "course_refs 为空，课程分区不得显示内容");
    teachingCore = Capability::Unknown(
        "未协商：core_bridge 只在 /capabilities 报告可用性，/teaching 未暴露（DEC-06）",
        "core available != 桌面已能执行 U2 动作");
    model = Capability::Unknown("capabilities 尚未返回 model 状态", "不伪造 model_used");
    edaExecution = Capability::Unavailable("execution.can_run_eda=false", "v1 不提供 EDA 执行");
    record = Capability::Unavailable("DEC-07：学习记录合同未定义，无导出/删除接口",
                                     "SQLite 是教学事务日志，不是掌握度库");
    backendCancel = Capability::Unavailable(
        "DEC-06：v1 没有 cancel 路由，U2 cancel 未接入", "取消只能是本地丢弃，不是终态");
    checkers = Capability::Unavailable("v1 没有 checker 聚合接口（R07）", "禁止静态全绿");
}

const Capability& CapabilitySet::Get(CapabilityKey key) const
{
    switch (key)
    {
    case CapabilityKey::Course:
        return course;
    case CapabilityKey::TeachingCore:
        return teachingCore;
    case CapabilityKey::Model:
        return model;
    case CapabilityKey::EdaExecution:
        return edaExecution;
    case CapabilityKey::Record:
        return record;
    case CapabilityKey::BackendCancel:
        return backendCancel;
    case CapabilityKey::Checkers:
        return checkers;
    }
    return course;
}

Capability& CapabilitySet::Get(CapabilityKey key)
{
    switch (key)
    {
    case CapabilityKey::Course:
        return course;
    case CapabilityKey::TeachingCore:
        return teachingCore;
    case CapabilityKey::Model:
        return model;
    case CapabilityKey::EdaExecution:
        return edaExecution;
    case CapabilityKey::Record:
        return record;
    case CapabilityKey::BackendCancel:
        return backendCancel;
    case CapabilityKey::Checkers:
        return checkers;
    }
    return course;
}

// ===========================================================================
// 9. Subscription
// ===========================================================================

Subscription::Subscription(std::shared_ptr<Control> control, std::uint64_t id) noexcept
    : m_control(std::move(control)), m_id(id)
{
}

Subscription::~Subscription()
{
    Reset();
}

Subscription::Subscription(Subscription&& other) noexcept
    : m_control(std::move(other.m_control)), m_id(other.m_id)
{
    other.m_id = 0;
}

Subscription& Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        m_control = std::move(other.m_control);
        m_id = other.m_id;
        other.m_id = 0;
    }
    return *this;
}

void Subscription::Reset() noexcept
{
    if (!m_control)
    {
        m_id = 0;
        return;
    }
    // 先标 dead：即使是在非 owner 线程解绑，也保证不会再回调（不动物理容器）。
    m_control->alive.store(false, std::memory_order_release);
    // 非 owner 线程只接触独立控制块：模型可能正于 owner 线程析构。
    // 即使把 owner 改成 atomic 指针，先读指针再调用也不能保证模型仍活着。
    if (std::this_thread::get_id() == m_control->ownerThread &&
        m_control->owner != nullptr && m_id != 0)
    {
        m_control->owner->Unsubscribe(m_id);
    }
    m_control.reset();
    m_id = 0;
}

bool Subscription::Active() const noexcept
{
    return m_control != nullptr && m_control->alive.load(std::memory_order_acquire);
}

// ===========================================================================
// 10. AgentViewModel
// ===========================================================================

AgentViewModel::AgentViewModel() : m_ownerThread(std::this_thread::get_id())
{
}

AgentViewModel::~AgentViewModel()
{
    // 令牌可能比模型活得久：把 owner 置空，令牌随后 Reset 就只剩空壳。
    for (ListenerEntry& entry : m_listeners)
    {
        if (entry.control)
        {
            entry.control->owner = nullptr;
            entry.control->alive.store(false, std::memory_order_release);
        }
    }
    m_listeners.clear();
}

bool AgentViewModel::IsOwnerThread() const
{
    return std::this_thread::get_id() == m_ownerThread;
}

const char* AgentViewModel::LastForeignThreadOp() const
{
    const char* operation = m_lastForeignThreadOp.load(std::memory_order_acquire);
    return operation != nullptr ? operation : "none";
}

unsigned long long AgentViewModel::ForeignThreadCallCount() const
{
    return m_foreignThreadCalls.load(std::memory_order_relaxed);
}

bool AgentViewModel::EnsureOwnerThread(const char* operation)
{
    if (std::this_thread::get_id() == m_ownerThread)
    {
        return true;
    }
    // 写之前拒绝：不改任何业务状态，也不碰监听器容器。诊断用 atomic，不抛异常。
    m_lastForeignThreadOp.store(operation, std::memory_order_release);
    m_foreignThreadCall.store(true, std::memory_order_release);
    m_foreignThreadCalls.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool AgentViewModel::MatchesFrozenEnvelope(const RequestContext& ctx) const
{
    // 允许的投递信封**只有** BeginRun 时冻结的那一个：拿当前 UI 状态重建的 ctx 即使
    // 字段"看起来一样"，也会因为 level（或适配器顺手改的字段）不同而被拒。
    if (!m_inflight.has_value())
    {
        return true;   // 没有在途冻结（应由 identity 检查兜底）
    }
    return ctx == *m_inflight;
}

// --- 订阅 ---

Subscription AgentViewModel::Subscribe(Listener listener)
{
    if (!EnsureOwnerThread("Subscribe"))
    {
        return Subscription{};
    }
    if (!listener)
    {
        return Subscription{};
    }
    auto control = std::make_shared<Subscription::Control>();
    control->owner = this;
    const std::uint64_t id = m_nextListenerId++;
    m_listeners.push_back(ListenerEntry{id, std::move(listener), std::move(control)});
    return Subscription(m_listeners.back().control, id);
}

void AgentViewModel::Unsubscribe(std::uint64_t id) noexcept
{
    if (!EnsureOwnerThread("Unsubscribe"))
    {
        // 非 owner：令牌已把槽标 dead，下一次 owner 线程通知时惰性清理。
        return;
    }
    for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it)
    {
        if (it->id == id)
        {
            m_listeners.erase(it);
            return;
        }
    }
}

AgentViewModel::ListenerEntry* AgentViewModel::FindEntry(std::uint64_t id)
{
    for (ListenerEntry& entry : m_listeners)
    {
        if (entry.id == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

void AgentViewModel::PruneDeadListeners()
{
    if (!IsOwnerThread())
    {
        return;
    }
    m_listeners.erase(std::remove_if(m_listeners.begin(), m_listeners.end(),
                                     [](const ListenerEntry& entry) {
                                         return !entry.control ||
                                                !entry.control->alive.load(std::memory_order_acquire);
                                     }),
                      m_listeners.end());
}

void AgentViewModel::Notify()
{
    if (!EnsureOwnerThread("Notify"))
    {
        return;
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(m_listeners.size());
    for (const ListenerEntry& entry : m_listeners)
    {
        ids.push_back(entry.id);
    }
    // 逐个按 id 重查：回调内订阅/解绑都不会让迭代失效；本轮新增的不在 ids 里。
    for (const std::uint64_t id : ids)
    {
        ListenerEntry* entry = FindEntry(id);
        if (entry == nullptr || !entry->fn)
        {
            continue;
        }
        if (!entry->control || !entry->control->alive.load(std::memory_order_acquire))
        {
            continue;
        }
        const Listener callback = entry->fn;   // 值拷贝：回调内解绑后原对象可能已销毁
        callback();
    }
    PruneDeadListeners();
}

// --- 模式/形态/草稿/滚动（共享 UI 状态；切换不清数据）---

void AgentViewModel::SetMode(AppMode mode)
{
    if (!EnsureOwnerThread("SetMode"))
    {
        return;
    }
    if (m_mode == mode)
    {
        return;
    }
    m_mode = mode;
    Notify();
}

void AgentViewModel::SetForm(agent::gui::vm::Form form)
{
    if (!EnsureOwnerThread("SetForm"))
    {
        return;
    }
    if (m_form == form)
    {
        return;
    }
    m_form = form;
    Notify();
}

void AgentViewModel::SetDraft(std::string text)
{
    if (!EnsureOwnerThread("SetDraft"))
    {
        return;
    }
    if (m_draft == text)
    {
        return;
    }
    m_draft = std::move(text);
    Notify();
}

void AgentViewModel::SetScrollAnchor(agent::gui::vm::ScrollAnchor anchor)
{
    if (!EnsureOwnerThread("SetScrollAnchor"))
    {
        return;
    }
    if (m_scrollAnchor == anchor)
    {
        return;
    }
    m_scrollAnchor = std::move(anchor);
    Notify();
}

void AgentViewModel::ClearScrollAnchor()
{
    SetScrollAnchor(agent::gui::vm::ScrollAnchor{});
}

// --- 绑定与运行 ---

void AgentViewModel::RecordReject(ApplyResult result, std::string reason)
{
    m_lastRejectReason = std::string(ApplyResultToken(result)) + ": " + std::move(reason);
}

void AgentViewModel::ResetSessionViews()
{
    m_stages.clear();
    m_omittedStages = 0;
    m_messages.clear();
    m_cards.clear();
    m_evidence.clear();
    m_plan = PlanView{};
    m_course = CourseView{};
    m_verification = VerificationView{};
    m_prediction = PredictionView{};
    m_record = RecordView{};
    m_checkers.clear();

    m_status = StatusView{};
    m_runId.clear();
    m_inflight.reset();
    m_applied.reset();
    m_localDiscard = false;
    m_localDiscardRunId.clear();
    m_localDiscardGeneration = 0;
    m_cancelPending = false;

    m_cursorKind = CursorKind::None;
    m_lastU2Sequence = -1;
    m_lastV1Sequence = Version::Unknown();
    m_seenEventIds.clear();
    m_seenActionIds.clear();
    m_appliedEvents = 0;

    m_hydratedVersion = Version::Unknown();
    m_hydratedStatus = RunStatus::Unknown;
    m_hydrationKey.clear();
    // 注意：capabilities / mode / form / draft / scrollAnchor 不在这里复位。
}

void AgentViewModel::InvalidateRunFacts()
{
    m_stages.clear();
    m_omittedStages = 0;
    // run 级事实失效：不带着新 revision/generation 标签继续显示旧 run。
    m_runId.clear();
    m_inflight.reset();
    m_applied.reset();
    m_localDiscard = false;
    m_localDiscardRunId.clear();
    m_localDiscardGeneration = 0;
    m_cancelPending = false;

    m_cards.clear();
    m_evidence.clear();
    m_plan = PlanView{};
    m_course = CourseView{};
    m_verification = VerificationView{};
    m_prediction = PredictionView{};

    StatusView fresh;
    fresh.projectId = m_projectId;
    fresh.revision = m_revision;
    fresh.generation = m_generation;
    fresh.sessionId = m_sessionId;
    fresh.level = Level::Unbound();
    m_status = fresh;

    m_hydratedVersion = Version::Unknown();
    m_hydratedStatus = RunStatus::Unknown;
    m_hydrationKey.clear();
}

bool AgentViewModel::BindSession(std::string projectId, std::string revision,
                                 std::uint64_t generation, std::string sessionId)
{
    if (!EnsureOwnerThread("BindSession"))
    {
        return false;
    }
    if (projectId.empty() || sessionId.empty())
    {
        RecordReject(ApplyResult::Rejected, "projectId 与 sessionId 不能为空");
        return false;
    }
    if (m_demoMode)
    {
        RecordReject(ApplyResult::DemoLocked, "演示模式下不能建立 live 会话绑定（先 ClearDemoData）");
        return false;
    }
    const bool sameProject = projectId == m_projectId;
    const bool sameRevision = revision == m_revision;
    const bool sameSession = sessionId == m_sessionId;
    const bool sameGeneration = generation == m_generation;

    // generation 倒退会让已失效的旧结果重新匹配，必须拒绝。
    if (sameProject && sameSession && generation < m_generation)
    {
        RecordReject(ApplyResult::Rejected,
                     "generation 不能倒退（否则旧结果会重新生效）：" +
                         JoinNumbers(generation, " < ", m_generation));
        return false;
    }
    if (sameProject && sameRevision && sameSession && sameGeneration)
    {
        return true;   // 完全相同的绑定：幂等，不动事实
    }

    const bool sessionScopedInvalid = !sameSession || !sameProject;
    m_projectId = std::move(projectId);
    m_revision = std::move(revision);
    m_generation = generation;
    m_sessionId = std::move(sessionId);

    if (sessionScopedInvalid)
    {
        ResetSessionViews();
    }
    else
    {
        InvalidateRunFacts();
    }
    m_status.projectId = m_projectId;
    m_status.revision = m_revision;
    m_status.generation = m_generation;
    m_status.sessionId = m_sessionId;

    m_lastRejectReason.clear();
    Notify();
    return true;
}

bool AgentViewModel::BeginRun(std::string sessionId, std::string runId, Level level)
{
    if (!EnsureOwnerThread("BeginRun"))
    {
        return false;
    }
    if (m_demoMode)
    {
        RecordReject(ApplyResult::DemoLocked, "演示模式下不发起 live 运行");
        return false;
    }
    if (sessionId.empty() || runId.empty())
    {
        RecordReject(ApplyResult::ForeignSession, "sessionId 与 runId 都不能为空");
        return false;
    }
    if (m_sessionId.empty())
    {
        RecordReject(ApplyResult::ForeignSession, "尚未 BindSession，不能 BeginRun");
        return false;
    }
    if (sessionId != m_sessionId)
    {
        RecordReject(ApplyResult::ForeignSession,
                     "BeginRun 的 session '" + sessionId + "' 不是当前绑定 '" + m_sessionId + "'");
        return false;
    }

    m_runId = std::move(runId);
    m_localDiscard = false;
    m_localDiscardRunId.clear();
    m_localDiscardGeneration = 0;
    m_cancelPending = false;

    // 冻结快照：之后 UI 改等级/learner 都不影响本次请求。
    m_inflight.emplace(m_projectId, m_revision, m_generation, m_sessionId, m_runId, m_learnerId, level);
    m_applied.reset();

    // 新 run 清空上一 run 的视图；消息、会话级分区、事件游标保留。
    m_stages.clear();
    m_omittedStages = 0;
    m_cards.clear();
    m_evidence.clear();
    m_plan = PlanView{};
    m_course = CourseView{};
    m_verification = VerificationView{};
    m_prediction = PredictionView{};
    m_hydratedVersion = Version::Unknown();
    m_hydratedStatus = RunStatus::Unknown;
    m_hydrationKey.clear();

    m_status.status = RunStatus::Unknown;
    m_status.rawStatus.clear();
    m_status.busy = true;
    m_status.request = RequestState::Busy;
    m_status.version = Version::Unknown();
    m_status.generation = m_generation;
    m_status.projectId = m_projectId;
    m_status.revision = m_revision;
    m_status.sessionId = m_sessionId;
    m_status.runId = m_runId;
    m_status.level = level;
    m_status.modelUsed.clear();
    m_status.reason.clear();
    m_status.limitations.clear();
    m_status.hydrated = false;
    m_status.origin = Origin::Live;

    m_lastRejectReason.clear();
    Notify();
    return true;
}

bool AgentViewModel::BeginRun(std::string runId)
{
    return BeginRun(m_sessionId, std::move(runId), m_uiLevel);
}

bool AgentViewModel::BindRun(std::string runId, Level level)
{
    return BeginRun(m_sessionId, std::move(runId), std::move(level));
}

std::uint64_t AgentViewModel::BumpGeneration()
{
    if (!EnsureOwnerThread("BumpGeneration"))
    {
        return m_generation;
    }
    if (m_generation == std::numeric_limits<std::uint64_t>::max())
    {
        // 不回绕到 0：0 会让任意旧 generation=0 的结果重新匹配。
        RecordReject(ApplyResult::Rejected, "generation 已达上限，不回绕");
        return m_generation;
    }
    ++m_generation;
    m_stages.clear();
    m_omittedStages = 0;
    m_status.generation = m_generation;
    // 代际前进 = 当前请求上下文失效：旧 run 绑定/在途快照/前端 busy 都清掉，
    // 但不抹掉已渲染的卡片与消息（那是历史事实，不是"当前在途"）。
    m_inflight.reset();
    m_runId.clear();
    m_status.runId.clear();
    m_status.busy = false;
    if (m_status.request == RequestState::Busy)
    {
        m_status.request = RequestState::Idle;
    }
    m_cancelPending = false;
    Notify();
    return m_generation;
}

bool AgentViewModel::SetGeneration(std::uint64_t generation)
{
    if (!EnsureOwnerThread("SetGeneration"))
    {
        return false;
    }
    if (generation < m_generation)
    {
        RecordReject(ApplyResult::Rejected,
                     "generation 不能倒退（否则旧结果会重新生效）：" +
                         JoinNumbers(generation, " < ", m_generation));
        return false;
    }
    if (generation == m_generation)
    {
        m_lastRejectReason.clear();
        return true;
    }
    m_generation = generation;
    m_stages.clear();
    m_omittedStages = 0;
    m_status.generation = m_generation;
    // 同 BumpGeneration：代际变化即当前请求上下文失效（保留已渲染内容）。
    m_inflight.reset();
    m_runId.clear();
    m_status.runId.clear();
    m_status.busy = false;
    if (m_status.request == RequestState::Busy)
    {
        m_status.request = RequestState::Idle;
    }
    m_cancelPending = false;
    m_lastRejectReason.clear();
    Notify();
    return true;
}

void AgentViewModel::SetLearnerId(std::string learnerId)
{
    if (!EnsureOwnerThread("SetLearnerId"))
    {
        return;
    }
    if (m_learnerId == learnerId)
    {
        return;
    }
    // learner 属于绑定；在途快照仍保留旧值（不可变语义）。
    m_learnerId = std::move(learnerId);
    Notify();
}

void AgentViewModel::SetLevel(Level level)
{
    if (!EnsureOwnerThread("SetLevel"))
    {
        return;
    }
    if (m_uiLevel == level)
    {
        return;
    }
    m_uiLevel = std::move(level);   // 只改 UI 选择，m_inflight 快照不动
    Notify();
}

void AgentViewModel::ApplyStatusRequestState(RunStatus status)
{
    if (IsTerminal(status))
    {
        m_status.request = RequestStateForStatus(status);
        m_status.busy = false;
        m_cancelPending = false;
        return;
    }
    if (m_cancelPending)
    {
        // 已受理取消：保持 pending/busy，禁止回到普通 Busy（202 != Cancelled）。
        m_status.request = RequestState::CancelRequested;
        m_status.busy = true;
        return;
    }
    m_status.request = RequestStateForStatus(status);
    m_status.busy = (m_status.request == RequestState::Busy);
}

// --- reducer ---

ApplyResult AgentViewModel::ApplyRunResult(const RequestContext& ctx, RunStatus status,
                                          std::string rawStatus, Version version,
                                          std::vector<CardView> cards, std::string modelUsed,
                                          std::vector<std::string> limitations)
{
    RunSnapshot snapshot;
    snapshot.status = status;
    snapshot.rawStatus = std::move(rawStatus);
    snapshot.version = std::move(version);
    snapshot.cards = std::move(cards);
    snapshot.modelUsed = std::move(modelUsed);
    snapshot.limitations = std::move(limitations);
    return ApplyRunResult(ctx, snapshot);
}

ApplyResult AgentViewModel::ApplyRunResult(const RequestContext& ctx, const RunSnapshot& snapshot)
{
    if (!EnsureOwnerThread("ApplyRunResult"))
    {
        return ApplyResult::WrongThread;
    }
    if (m_demoMode)
    {
        RecordReject(ApplyResult::DemoLocked, "演示模式下拒绝 live 运行结果");
        return ApplyResult::DemoLocked;
    }
    if (CardsHaveDemoPayload(snapshot.cards))
    {
        RecordReject(ApplyResult::DemoPayload,
                     "运行结果内含 Origin::Demo 卡片/证据：live 入口不接收演示数据（也不改标签）");
        return ApplyResult::DemoPayload;
    }
    if (snapshot.plan.present && snapshot.plan.origin == Origin::Demo)
    {
        RecordReject(ApplyResult::DemoPayload,
                     "运行结果内含 Origin::Demo 计划：live 入口不接收演示计划（也不改标签）");
        return ApplyResult::DemoPayload;
    }
    if (snapshot.course.origin == Origin::Demo)
    {
        RecordReject(ApplyResult::DemoPayload,
                     "运行结果内含 Origin::Demo 课程引用：live 入口不接收演示数据（也不改标签）");
        return ApplyResult::DemoPayload;
    }

    std::string reason;
    const ApplyResult identity =
        CheckIdentity(m_projectId, m_revision, m_generation, m_sessionId, m_runId, m_learnerId, ctx,
                      &reason);
    if (identity != ApplyResult::Applied)
    {
        RecordReject(identity, reason);
        return identity;
    }
    if (!MatchesFrozenEnvelope(ctx))
    {
        RecordReject(ApplyResult::Rejected,
                     "投递信封不是发请求时冻结的上下文（不得用当前 UI 状态重建）：level '" +
                         ctx.level.ToText() + "' vs 冻结 '" + m_inflight->level.ToText() + "'");
        return ApplyResult::Rejected;
    }
    if (m_localDiscard && m_localDiscardRunId == ctx.runId &&
        m_localDiscardGeneration == ctx.generation)
    {
        RecordReject(ApplyResult::LocallyDiscarded,
                     "该请求已被本地丢弃（后端未停止），结果不写入：" + ctx.runId);
        return ApplyResult::LocallyDiscarded;
    }
    if (snapshot.version.kind != Version::Kind::Unknown && !snapshot.version.valid)
    {
        RecordReject(ApplyResult::Rejected,
                     "版本不符合合同格式（拒绝比较）：'" + snapshot.version.ToText() + "'");
        return ApplyResult::Rejected;
    }

    const RunStatus current = m_status.status;
    if (IsTerminal(current) && snapshot.status != current)
    {
        RecordReject(ApplyResult::Regressed, "终态不可回退/改写：" +
                                                 std::string(RunStatusToken(current)) + " → " +
                                                 std::string(RunStatusToken(snapshot.status)));
        return ApplyResult::Regressed;
    }

    const bool bothVersionUnknown =
        snapshot.version.kind == Version::Kind::Unknown && m_status.version.kind == Version::Kind::Unknown;
    if (!bothVersionUnknown)
    {
        const VersionOrder order = CompareVersions(snapshot.version, m_status.version);
        if (order == VersionOrder::Incomparable)
        {
            RecordReject(ApplyResult::Rejected,
                         "版本族不相容（v1 十进制与 U2 整数之间没有'更新'语义）：结果 " +
                             std::string(snapshot.version.KindToken()) + "，当前 " +
                             std::string(m_status.version.KindToken()));
            return ApplyResult::Rejected;
        }
        if (order == VersionOrder::Older)
        {
            RecordReject(ApplyResult::Regressed, "版本回退：" + snapshot.version.ToText() + " < " +
                                                     m_status.version.ToText());
            return ApplyResult::Regressed;
        }
        if (order == VersionOrder::Same && snapshot.status != current)
        {
            RecordReject(ApplyResult::Rejected,
                         "同 state_version 出现不同状态（合同违约）：" + snapshot.version.ToText() +
                             " " + std::string(RunStatusToken(current)) + "≠" +
                             std::string(RunStatusToken(snapshot.status)));
            return ApplyResult::Rejected;
        }
    }

    // 同一 (revision, version, status) 只允许水合一次：重复的完整 payload 幂等，
    // 内容冲突则保留先到数据（旧/异数据不得覆盖新数据）。
    const std::string fingerprint = FingerprintSnapshot(snapshot);
    if (!bothVersionUnknown && snapshot.status == current && m_status.hydrated &&
        CompareVersions(snapshot.version, m_hydratedVersion) == VersionOrder::Same)
    {
        if (m_hydrationKey == fingerprint)
        {
            RecordReject(ApplyResult::Duplicated,
                         "同一份完整结果重复投递，幂等忽略：" + snapshot.version.ToText());
            return ApplyResult::Duplicated;
        }
        RecordReject(ApplyResult::Rejected,
                     "同版本同状态出现不同的完整结果，保留先到数据：" + snapshot.version.ToText());
        return ApplyResult::Rejected;
    }

    // 通过：写入。卡片来源补 Live（Demo 已在上面拒收）；等级取信封快照。
    m_cards = snapshot.cards;
    MarkCardsOriginLive(&m_cards);
    for (auto& card : m_cards) card.receiptRevision = ctx.revision;
    RebuildEvidence();
    // 计划只做只读投影：不产生任何批准/执行能力，也不保留 params。
    m_plan = snapshot.plan;
    m_plan.origin = Origin::Live;
    // 课程引用只做只读投影：没有课程正文、没有进度、没有跳转授权；能力状态如实记录。
    m_course = snapshot.course;
    m_course.origin = Origin::Live;
    m_course.capability = m_capabilities.course;
    m_course.revision = ctx.revision;
    // 验证进度只由 U2 teaching response 填充；v1 run 的 snapshot 保持 absent。
    m_verification = snapshot.verification;
    // prediction 也只由 U2 teaching response 填充；v1 run 保持 absent。
    m_prediction = snapshot.prediction;
    if (m_course.rawState.empty())
    {
        m_course.rawState = snapshot.rawStatus.empty() ? RunStatusToken(snapshot.status)
                                                       : snapshot.rawStatus;
    }
    if (!m_course.refs.empty() && !m_capabilities.course.IsReady())
    {
        m_course.limitations.push_back(
            m_capabilities.course.reason.empty()
                ? "课程能力未就绪（原因未知）：只显示引用元数据"
                : "课程能力未就绪：" + m_capabilities.course.reason);
    }

    m_status.status = snapshot.status;
    m_status.rawStatus =
        snapshot.rawStatus.empty() ? RunStatusToken(snapshot.status) : snapshot.rawStatus;
    if (snapshot.version.kind != Version::Kind::Unknown)
    {
        m_status.version = snapshot.version;
    }
    m_status.generation = ctx.generation;
    m_status.projectId = ctx.projectId;
    m_status.revision = ctx.revision;
    m_status.sessionId = ctx.sessionId;
    m_status.runId = ctx.runId;
    m_status.level = ctx.level;
    m_status.modelUsed = snapshot.modelUsed;
    m_status.reason = snapshot.reason;
    m_status.limitations = snapshot.limitations;
    m_status.origin = Origin::Live;
    ApplyStatusRequestState(snapshot.status);

    // 计划必须属于本次冻结的 revision：旧版本计划不显示、不缓存，也不借新 revision 复活。
    if (m_plan.present && !m_plan.revision.empty() && !ctx.revision.empty() &&
        m_plan.revision != ctx.revision)
    {
        m_plan = PlanView{};
        m_status.limitations.push_back("服务返回的验证计划绑定到其他代码版本，已隐藏。");
    }

    if (snapshot.status == RunStatus::Unknown)
    {
        // 未知状态：原文保留（rawStatus），前端按失败对待，绝不显示成功。
        m_status.reason = "未知后端状态（保留原文，不按成功处理）" +
                          (snapshot.reason.empty() ? std::string() : "：" + snapshot.reason);
    }

    m_status.hydrated = true;
    m_hydratedVersion = m_status.version;
    m_hydratedStatus = snapshot.status;
    m_hydrationKey = fingerprint;
    m_applied.emplace(ctx);
    m_lastRejectReason.clear();
    Notify();
    return ApplyResult::Applied;
}

void AgentViewModel::RecordPublicStage(const Event& event)
{
    // Called only after the complete reducer acceptance path. The current U2
    // contract has status/sequence, not public tool names, arguments or traces.
    std::string title;
    if (event.sequence.has_value() && event.status != RunStatus::Unknown)
        title = "运行状态更新";
    else if (event.type == "run/finished")
        title = "收到运行结果通知";
    else if (event.type == "run/failed")
        title = "收到运行失败通知";
    else
        return;  // session metadata and unknown extensions are not public stages

    StageView stage;
    const bool publicEventId = event.eventId.size() > 3 && event.eventId.rfind("ev-", 0) == 0 &&
        event.eventId[3] >= '1' && event.eventId[3] <= '9' &&
        std::all_of(event.eventId.begin() + 4, event.eventId.end(),
                    [](char c) { return c >= '0' && c <= '9'; });
    stage.id = publicEventId ? event.eventId :
               event.sequence.has_value() ? "sequence:" + std::to_string(*event.sequence) :
               !event.sequenceText.empty() ? "sequence:" + event.sequenceText :
               event.stateVersion.known() ? "version:" + event.stateVersion.canonical :
               "observation:" + std::to_string(m_appliedEvents);
    stage.title = std::move(title);
    stage.status = event.status;
    stage.summary = event.status == RunStatus::Unknown ? "事件未提供运行状态，等待权威结果" :
                    std::string("公开状态：") + RunStatusToken(event.status);
    stage.origin = Origin::Live;
    // Do not copy detail/rawStatus/action payloads. Their publicness is undefined.
    constexpr size_t limit = 64;
    if (m_stages.size() == limit) {
        m_stages.erase(m_stages.begin());
        ++m_omittedStages;
    }
    m_stages.push_back(std::move(stage));
}

ApplyResult AgentViewModel::ApplyEvent(const RequestContext& ctx, const Event& event)
{
    if (!EnsureOwnerThread("ApplyEvent"))
    {
        return ApplyResult::WrongThread;
    }
    if (m_demoMode)
    {
        RecordReject(ApplyResult::DemoLocked, "演示模式下拒绝 live 事件");
        return ApplyResult::DemoLocked;
    }
    if (event.origin == Origin::Demo)
    {
        RecordReject(ApplyResult::DemoPayload, "事件来源标为 Demo：live 入口不接收演示事件");
        return ApplyResult::DemoPayload;
    }
    if (CardsHaveDemoPayload(event.cards))
    {
        RecordReject(ApplyResult::DemoPayload, "事件内含 Origin::Demo 卡片/证据：live 入口不接收");
        return ApplyResult::DemoPayload;
    }

    std::string reason;
    const ApplyResult identity =
        CheckIdentity(m_projectId, m_revision, m_generation, m_sessionId, m_runId, m_learnerId, ctx,
                      &reason);
    if (identity != ApplyResult::Applied)
    {
        RecordReject(identity, reason);
        return identity;
    }
    if (!MatchesFrozenEnvelope(ctx))
    {
        RecordReject(ApplyResult::Rejected,
                     "投递信封不是发请求时冻结的上下文（不得用当前 UI 状态重建）：level '" +
                         ctx.level.ToText() + "' vs 冻结 '" + m_inflight->level.ToText() + "'");
        return ApplyResult::Rejected;
    }
    // wire 自带身份若与信封不一致，说明适配器串线：拒绝（信封是权威）。
    if (!event.sessionId.empty() && event.sessionId != ctx.sessionId)
    {
        RecordReject(ApplyResult::ForeignSession,
                     "事件自带 session '" + event.sessionId + "' 与投递信封 '" + ctx.sessionId + "' 不符");
        return ApplyResult::ForeignSession;
    }
    if (!event.runId.empty() && event.runId != ctx.runId)
    {
        RecordReject(ApplyResult::ForeignRun,
                     "事件自带 run '" + event.runId + "' 与投递信封 '" + ctx.runId + "' 不符");
        return ApplyResult::ForeignRun;
    }
    if (!event.learnerId.empty() && event.learnerId != ctx.learnerId)
    {
        RecordReject(ApplyResult::ForeignLearner,
                     "事件自带 learner '" + event.learnerId + "' 与投递信封 '" + ctx.learnerId +
                         "' 不符");
        return ApplyResult::ForeignLearner;
    }
    if (m_localDiscard && m_localDiscardRunId == ctx.runId &&
        m_localDiscardGeneration == ctx.generation)
    {
        RecordReject(ApplyResult::LocallyDiscarded, "该 run 已被本地丢弃，事件不再写入：" + ctx.runId);
        return ApplyResult::LocallyDiscarded;
    }

    if (event.sequence.has_value() && *event.sequence < 0)
    {
        RecordReject(ApplyResult::Rejected,
                     "U2 sequence 不能为负：'" + std::to_string(*event.sequence) + "'");
        return ApplyResult::Rejected;
    }
    Version v1Sequence;
    if (!event.sequenceText.empty())
    {
        v1Sequence = Version::FromV1EventSequence(event.sequenceText);
        if (!v1Sequence.valid)
        {
            RecordReject(ApplyResult::Rejected,
                         "v1 sequence 不符合 ^[0-9]+$：'" + event.sequenceText + "'");
            return ApplyResult::Rejected;
        }
    }
    if (event.stateVersion.kind != Version::Kind::Unknown && !event.stateVersion.valid)
    {
        RecordReject(ApplyResult::Rejected,
                     "事件 state_version 不符合合同格式：'" + event.stateVersion.ToText() + "'");
        return ApplyResult::Rejected;
    }

    const bool hasU2Sequence = event.sequence.has_value();
    const bool hasV1Sequence = v1Sequence.valid;
    if (hasU2Sequence)
    {
        if (m_cursorKind == CursorKind::V1Decimal)
        {
            RecordReject(ApplyResult::Rejected,
                         "事件序号族混用：会话已使用 v1 十进制 sequence，收到 U2 整数 sequence");
            return ApplyResult::Rejected;
        }
        if (m_cursorKind == CursorKind::U2Int)
        {
            if (*event.sequence < m_lastU2Sequence)
            {
                RecordReject(ApplyResult::Regressed, "旧 sequence " +
                                                         std::to_string(*event.sequence) + " < 已应用 " +
                                                         std::to_string(m_lastU2Sequence));
                return ApplyResult::Regressed;
            }
            if (*event.sequence == m_lastU2Sequence)
            {
                RecordReject(ApplyResult::Duplicated,
                             "sequence 重复（" + std::to_string(*event.sequence) + "），幂等忽略");
                return ApplyResult::Duplicated;
            }
        }
    }
    else if (hasV1Sequence)
    {
        if (m_cursorKind == CursorKind::U2Int)
        {
            RecordReject(ApplyResult::Rejected,
                         "事件序号族混用：会话已使用 U2 整数 sequence，收到 v1 十进制 sequence");
            return ApplyResult::Rejected;
        }
        if (m_cursorKind == CursorKind::V1Decimal)
        {
            const VersionOrder sequenceOrder = CompareVersions(v1Sequence, m_lastV1Sequence);
            if (sequenceOrder == VersionOrder::Older)
            {
                RecordReject(ApplyResult::Regressed, "旧 v1 sequence " + v1Sequence.ToText() +
                                                         " < 已应用 " + m_lastV1Sequence.ToText());
                return ApplyResult::Regressed;
            }
            if (sequenceOrder == VersionOrder::Same)
            {
                RecordReject(ApplyResult::Duplicated,
                             "v1 sequence 重复（" + v1Sequence.ToText() + "），幂等忽略");
                return ApplyResult::Duplicated;
            }
        }
    }

    if (!event.eventId.empty())
    {
        for (const std::string& seen : m_seenEventIds)
        {
            if (seen == event.eventId)
            {
                RecordReject(ApplyResult::Duplicated, "event_id 已应用，幂等忽略：" + event.eventId);
                return ApplyResult::Duplicated;
            }
        }
    }

    if (event.stateVersion.kind != Version::Kind::Unknown)
    {
        const VersionOrder order = CompareVersions(event.stateVersion, m_status.version);
        if (order == VersionOrder::Incomparable)
        {
            RecordReject(ApplyResult::Rejected,
                         "事件 state_version 与当前版本族不相容：" +
                             std::string(event.stateVersion.KindToken()) + " vs " +
                             std::string(m_status.version.KindToken()));
            return ApplyResult::Rejected;
        }
        if (order == VersionOrder::Older)
        {
            RecordReject(ApplyResult::Regressed, "事件 state_version 回退：" +
                                                     event.stateVersion.ToText() + " < " +
                                                     m_status.version.ToText());
            return ApplyResult::Regressed;
        }
        if (order == VersionOrder::Same && event.status != RunStatus::Unknown &&
            event.status != m_status.status)
        {
            RecordReject(ApplyResult::Rejected,
                         "同 state_version 不同状态（合同违约）：" + event.stateVersion.ToText());
            return ApplyResult::Rejected;
        }
    }

    if (IsTerminal(m_status.status) && event.status != RunStatus::Unknown &&
        event.status != m_status.status)
    {
        RecordReject(ApplyResult::Regressed,
                     "终态不可回退：" + std::string(RunStatusToken(m_status.status)) + " → " +
                         std::string(RunStatusToken(event.status)));
        return ApplyResult::Regressed;
    }

    // 每个事件都必须有去重键，否则无法区分"重放"与"新事件"（禁止盲目重放副作用）。
    const bool hasDedupeKey = hasU2Sequence || hasV1Sequence || !event.eventId.empty() ||
                              event.stateVersion.known();
    if (!hasDedupeKey)
    {
        RecordReject(ApplyResult::Rejected,
                     "事件缺少 event_id / sequence / state_version 去重键，拒绝盲目重放");
        return ApplyResult::Rejected;
    }

    const auto recordCursor = [this, &event, &v1Sequence]() {
        if (event.sequence.has_value())
        {
            m_cursorKind = CursorKind::U2Int;
            m_lastU2Sequence = *event.sequence;
        }
        else if (v1Sequence.valid)
        {
            m_cursorKind = CursorKind::V1Decimal;
            m_lastV1Sequence = v1Sequence;
        }
        if (!event.eventId.empty())
        {
            m_seenEventIds.push_back(event.eventId);
        }
        if (!event.actionId.empty())
        {
            // action_id 只作审计：同一 action 的后续事件（新 sequence）必须能被消费。
            m_seenActionIds.push_back(event.actionId);
        }
    };

    if (event.status == RunStatus::Unknown)
    {
        if (!event.rawStatus.empty())
        {
            RecordReject(ApplyResult::Rejected,
                         "事件状态未知（保留原文，不按成功处理）：'" + event.rawStatus + "'");
            return ApplyResult::Rejected;
        }
        // 无状态事件（例如 v1 session/created）：只推进游标，不动运行状态。
        recordCursor();
        ++m_appliedEvents;
        RecordPublicStage(event);
        m_lastRejectReason.clear();
        Notify();
        return ApplyResult::Applied;
    }

    m_status.status = event.status;
    m_status.rawStatus =
        event.rawStatus.empty() ? RunStatusToken(event.status) : event.rawStatus;
    if (event.stateVersion.known())
    {
        m_status.version = event.stateVersion;
    }
    m_status.generation = ctx.generation;
    m_status.projectId = ctx.projectId;
    m_status.revision = ctx.revision;
    m_status.sessionId = ctx.sessionId;
    m_status.runId = ctx.runId;
    m_status.level = ctx.level;
    m_status.origin = Origin::Live;
    ApplyStatusRequestState(event.status);
    m_status.reason.clear();
    if (!event.limitations.empty())
    {
        m_status.limitations = event.limitations;
    }
    MergeCards(&m_cards, event.cards, ctx.revision);
    RebuildEvidence();

    // 事件把状态推进到"未水合"的新 (version,status) 时，取消水合标记，
    // 允许随后到达的权威完整结果补齐 cards/modelUsed/limitations。
    if (m_status.hydrated && (!(m_hydratedVersion == m_status.version) ||
                              m_hydratedStatus != m_status.status))
    {
        m_status.hydrated = false;
        m_hydrationKey.clear();
    }

    recordCursor();
    ++m_appliedEvents;
    RecordPublicStage(event);
    m_lastRejectReason.clear();
    Notify();
    return ApplyResult::Applied;
}

ApplyResult AgentViewModel::ApplyMessage(const RequestContext& ctx, MessageView message)
{
    if (!EnsureOwnerThread("ApplyMessage"))
    {
        return ApplyResult::WrongThread;
    }
    if (m_demoMode)
    {
        RecordReject(ApplyResult::DemoLocked, "演示模式下拒绝 live 消息");
        return ApplyResult::DemoLocked;
    }
    if (message.origin == Origin::Demo)
    {
        RecordReject(ApplyResult::DemoPayload, "消息来源标为 Demo：live 入口不接收演示消息");
        return ApplyResult::DemoPayload;
    }
    std::string reason;
    const ApplyResult identity =
        CheckIdentity(m_projectId, m_revision, m_generation, m_sessionId, m_runId, m_learnerId, ctx,
                      &reason);
    if (identity != ApplyResult::Applied)
    {
        RecordReject(identity, reason);
        return identity;
    }
    if (!MatchesFrozenEnvelope(ctx))
    {
        RecordReject(ApplyResult::Rejected,
                     "投递信封不是发请求时冻结的上下文（不得用当前 UI 状态重建）：level '" +
                         ctx.level.ToText() + "' vs 冻结 '" + m_inflight->level.ToText() + "'");
        return ApplyResult::Rejected;
    }
    if (m_localDiscard && m_localDiscardRunId == ctx.runId &&
        m_localDiscardGeneration == ctx.generation)
    {
        RecordReject(ApplyResult::LocallyDiscarded, "该 run 已被本地丢弃，消息不再写入：" + ctx.runId);
        return ApplyResult::LocallyDiscarded;
    }
    message.origin = Origin::Live;
    m_messages.push_back(std::move(message));
    m_lastRejectReason.clear();
    Notify();
    return ApplyResult::Applied;
}

ApplyResult AgentViewModel::AppendLocalMessage(MessageView message)
{
    if (!EnsureOwnerThread("AppendLocalMessage"))
    {
        return ApplyResult::WrongThread;
    }
    if (m_demoMode)
    {
        RecordReject(ApplyResult::DemoLocked, "演示模式下不写入本地 live 消息（先 ClearDemoData）");
        return ApplyResult::DemoLocked;
    }
    if (message.origin == Origin::Demo)
    {
        RecordReject(ApplyResult::DemoPayload, "本地消息不接受 Origin::Demo 数据");
        return ApplyResult::DemoPayload;
    }
    // 本地用户消息没有网络身份，但仍是 live 事实（不是演示数据）。
    message.origin = Origin::Live;
    m_messages.push_back(std::move(message));
    m_lastRejectReason.clear();
    Notify();
    return ApplyResult::Applied;
}

void AgentViewModel::RebuildEvidence()
{
    m_evidence.clear();
    for (const CardView& card : m_cards)
    {
        for (const EvidenceView& evidence : card.evidence)
        {
            m_evidence.push_back(evidence);
        }
    }
}

// --- 取消：三种状态互不冒充 ---

bool AgentViewModel::BeginLocalCancel()
{
    if (!EnsureOwnerThread("BeginLocalCancel"))
    {
        return false;
    }
    if (m_cancelPending)
    {
        RecordReject(ApplyResult::Rejected,
                     "后端已受理取消请求，不能降级成「本地丢弃」（两件事不能互相冒充）");
        return false;
    }
    if (m_status.request != RequestState::Busy || !m_inflight)
    {
        RecordReject(ApplyResult::Rejected, "没有在途请求，本地丢弃没有意义");
        return false;
    }
    m_localDiscard = true;
    m_localDiscardGeneration = m_generation;
    m_localDiscardRunId = m_runId;
    // 只表达"前端不再等待/不再显示这条结果"；后端可能仍在跑。
    m_status.request = RequestState::CancellingLocal;
    m_status.busy = false;
    // status / rawStatus / version 一律不动：绝不产生 Completed/Cancelled 终态。
    m_lastRejectReason.clear();
    Notify();
    return true;
}

bool AgentViewModel::RequestBackendCancel()
{
    if (!EnsureOwnerThread("RequestBackendCancel"))
    {
        return false;
    }
    const Capability& capability = m_capabilities.backendCancel;
    if (!capability.IsReady())
    {
        RecordReject(ApplyResult::Rejected,
                     "后端取消不可用：" +
                         (capability.reason.empty() ? std::string("未提供原因") : capability.reason));
        return false;
    }
    if (m_status.request != RequestState::Busy)
    {
        RecordReject(ApplyResult::Rejected, "没有在途请求（或取消已受理），不能请求后端取消");
        return false;
    }
    m_localDiscard = false;   // 走真正的后端取消路径
    m_localDiscardRunId.clear();
    m_localDiscardGeneration = 0;
    m_cancelPending = true;
    // 202 只是"受理"：保持 pending/busy，等事件观察到 Cancelled 才算取消完成。
    m_status.request = RequestState::CancelRequested;
    m_status.busy = true;
    m_lastRejectReason.clear();
    Notify();
    return true;
}

// --- 能力 ---

void AgentViewModel::SetCapability(CapabilityKey key, Capability capability)
{
    if (!EnsureOwnerThread("SetCapability"))
    {
        return;
    }
    m_capabilities.Get(key) = std::move(capability);
    Notify();
}

void AgentViewModel::SetCapabilities(CapabilitySet set)
{
    if (!EnsureOwnerThread("SetCapabilities"))
    {
        return;
    }
    m_capabilities = std::move(set);
    Notify();
}

void AgentViewModel::SetCapabilityModes(std::vector<std::string> modes)
{
    if (!EnsureOwnerThread("SetCapabilityModes"))
    {
        return;
    }
    m_capabilities.modes = std::move(modes);
    Notify();
}

// --- demo fixture ---

ApplyResult AgentViewModel::LoadDemoFixture(DemoScenario scenario)
{
    if (!EnsureOwnerThread("LoadDemoFixture"))
    {
        return ApplyResult::WrongThread;
    }
    bool liveData = false;
    for (const MessageView& message : m_messages)
    {
        if (message.origin == Origin::Live)
        {
            liveData = true;
        }
    }
    for (const CardView& card : m_cards)
    {
        if (card.origin == Origin::Live)
        {
            liveData = true;
        }
    }
    if (!m_demoMode && (!m_sessionId.empty() || !m_projectId.empty() || liveData))
    {
        RecordReject(ApplyResult::LiveLocked,
                     "已有 live 会话/数据，演示 fixture 不得覆盖（演示与真实不可混淆）");
        return ApplyResult::LiveLocked;
    }

    m_demoMode = true;

    // 演示 id 只出现在视图里，不占用真实绑定（m_projectId/m_sessionId 保持为空）。
    const std::string demoProject = "demo-project";
    const std::string demoRevision = "demo-r12";
    const std::string demoSession = "demo-session";
    const std::string demoRun = "demo-run";
    const Level demoLevel = Level::V1("L2");
    const Version demoVersion = Version::FromV1Sequence("12");

    // 教学口径（本 fixture 只作为视觉占位，但内容必须正确）：
    //   always @(*) begin y = 0; if (en) y = a & b; end
    //   有默认赋值 → 所有分支都赋值 → **不会**推断锁存器。
    //   只有删掉默认赋值时才会推断锁存器；这不许写成"自赋值能消除锁存"。
    const char* kSnippet =
        "always @(*) begin\n    y = 0;\n    if (en) y = a & b;\nend";

    m_messages.clear();
    MessageView userMessage;
    userMessage.messageId = "demo-msg-1";
    userMessage.role = "user";
    userMessage.text = "帮我看看 counter.v 里这段组合逻辑：en=0 时 y 会不会保持上一个值？";
    userMessage.rawState = "demo";
    userMessage.runId = demoRun;
    userMessage.origin = Origin::Demo;
    userMessage.limitations.push_back("演示数据：不是真实提问，也不是真实后端回答");
    m_messages.push_back(std::move(userMessage));

    MessageView assistantMessage;
    assistantMessage.messageId = "demo-msg-2";
    assistantMessage.role = "assistant";
    assistantMessage.text =
        "这段 always @(*) 里 y = 0 是默认赋值，en=0 时 y 被赋 0，不会保持上一个值，"
        "综合也不会推断锁存器。下面只看证据与假设。";
    assistantMessage.rawState = "demo";
    assistantMessage.runId = demoRun;
    assistantMessage.origin = Origin::Demo;
    assistantMessage.limitations.push_back("演示数据：固定编号仅用于视觉状态，不代表真实工具结果");
    m_messages.push_back(std::move(assistantMessage));

    m_cards.clear();
    CardView ruleCard;
    ruleCard.cardId = "demo-card-1";
    ruleCard.kind = "diff";
    ruleCard.level = demoLevel;
    ruleCard.title = "组合 always 块已有默认赋值，不会推断锁存器";
    ruleCard.body = std::string(kSnippet) +
                    "\n\ny = 0 覆盖了 en=0 的分支，y 在所有路径都有赋值，因此这里的组合逻辑不产生锁存器。";
    ruleCard.issueId = "demo-issue-1";
    ruleCard.source = "rule";
    ruleCard.evidence.push_back(EvidenceView{"observation", "counter.v:12-16", kSnippet, Origin::Demo,
                                             {}});
    ruleCard.courseRefs.push_back("demo-passage-1");
    ruleCard.limitations.push_back("演示数据：规则路径只覆盖这一条分支覆盖结论");
    ruleCard.stateVersion = demoVersion;
    ruleCard.rawState = "demo";
    ruleCard.origin = Origin::Demo;
    ruleCard.receiptRevision = demoRevision;
    m_cards.push_back(std::move(ruleCard));

    CardView modelCard;
    modelCard.cardId = "demo-card-2";
    modelCard.kind = "hint";
    modelCard.level = demoLevel;
    modelCard.title = "假设：删掉 y = 0 之后才会推断锁存器";
    modelCard.body =
        "如果把默认赋值 y = 0 删掉，en=0 的分支就不再给 y 赋值，综合才会推断锁存器。"
        "这是需要用综合/仿真验证的假设，不是当前代码的事实。";
    modelCard.issueId = "demo-issue-1";
    modelCard.source = "model";
    modelCard.evidence.push_back(EvidenceView{
        "hypothesis", "counter.v:12-16", "删除默认赋值后出现 latch 推断（未验证）", Origin::Demo, {}});
    modelCard.limitations.push_back("演示数据：假设尚未验证，不得当成观察");
    modelCard.stateVersion = demoVersion;
    modelCard.rawState = "demo";
    modelCard.origin = Origin::Demo;
    modelCard.receiptRevision = demoRevision;
    m_cards.push_back(std::move(modelCard));
    RebuildEvidence();

    m_plan = PlanView{};
    m_plan.present = true;
    m_plan.planId = "demo-plan-1";
    m_plan.goal = "确认这段组合逻辑是否有锁存器风险";
    m_plan.steps.push_back(PlanStepView{"demo-step-1", "读敏感表与分支覆盖",
                                        "确认 y 在 always @(*) 的每条路径上都被赋值", "pending",
                                        Origin::Demo});
    m_plan.steps.push_back(PlanStepView{"demo-step-2", "跑一次综合",
                                        "看工具是否报告 latch 推断（当前代码预期没有）", "pending",
                                        Origin::Demo});
    m_plan.steps.push_back(PlanStepView{"demo-step-3", "对比删除默认赋值的差异",
                                        "若要复现 latch，再去掉 y = 0 后重新综合", "pending",
                                        Origin::Demo});
    m_plan.maxJobs = 1;
    m_plan.stopConditions.push_back("综合失败则停止");
    m_plan.revision = demoRevision;
    m_plan.requiresApproval = true;   // 合同要求；DEC-05 下批准入口始终禁用
    m_plan.rawState = "demo";
    m_plan.origin = Origin::Demo;
    m_plan.limitations.push_back("演示数据：批准按钮仍然禁用（DEC-05）");

    m_course = CourseView{};
    m_course.capability = m_capabilities.course;   // 能力如实保留（不可用就不可用）
    m_course.refs.push_back(CourseRefView{"demo-passage-1",
                                          "组合 always 块必须覆盖所有分支",
                                          "counter.v", Origin::Demo, {}});
    m_course.refs.push_back(CourseRefView{"demo-passage-2",
                                          "默认赋值与锁存器推断",
                                          "counter.v", Origin::Demo, {}});
    m_course.rawState = "demo";
    m_course.origin = Origin::Demo;
    m_course.revision = demoRevision;
    m_course.limitations.push_back("演示数据：课程能力仍以 capabilities 为准（当前不可用）");

    m_record = RecordView{};
    m_record.capability = m_capabilities.record;
    m_record.present = false;
    m_record.rawState = "demo";
    m_record.origin = Origin::Demo;
    m_record.limitations.push_back("演示数据：DEC-07 未定义记录合同，导出/删除都不可用");

    // 七个检查器只作布局占位：一律 Unknown，绝不造"全绿"。
    m_checkers.clear();
    const char* checkerIds[7] = {"lint", "latch", "cdc", "timing", "resource", "sim", "synth"};
    const char* checkerLabels[7] = {"语法", "锁存器", "跨时钟域", "时序", "资源", "仿真", "综合"};
    for (int index = 0; index < 7; ++index)
    {
        CheckerView checker;
        checker.checkerId = checkerIds[index];
        checker.label = checkerLabels[index];
        checker.verdict = CheckerView::Verdict::Unknown;
        checker.rawState = "demo";
        checker.reason = "演示占位：v1 没有 checker 聚合接口（R07），不显示通过";
        checker.origin = Origin::Demo;
        checker.limitations.push_back("演示数据：不是真实工具结论");
        m_checkers.push_back(std::move(checker));
    }

    m_status = StatusView{};
    m_status.status = RunStatus::WaitingStudent;   // 演示文案，不是后端事实
    m_status.rawStatus = "demo:WaitingStudent";
    m_status.busy = false;
    m_status.request = RequestState::Idle;
    m_status.version = demoVersion;
    m_status.generation = 0;
    m_status.projectId = demoProject;
    m_status.revision = demoRevision;
    m_status.sessionId = demoSession;
    m_status.runId = demoRun;
    m_status.level = demoLevel;
    m_status.modelUsed = "false";
    m_status.limitations.push_back("演示数据：状态不是后端事实，模式/形态/草稿不受影响");
    m_status.hydrated = false;
    m_status.origin = Origin::Demo;

    m_stages.clear();
    m_omittedStages = 0;
    if (scenario != DemoScenario::Quick) {
        const char* deepTitles[] = {"读取公开上下文", "检索公开证据", "整理问题摘要",
                                    "制定验证建议", "输出提示与边界"};
        const char* diffTitles[] = {"三路演示分发", "汇聚演示答卷", "比对演示差异",
                                    "演示成本说明", "展示对比表"};
        for (size_t i = 0; i < 5; ++i) {
            m_stages.push_back(StageView{"demo-stage:" + std::to_string(i),
                scenario == DemoScenario::Diff ? diffTitles[i] : deepTitles[i],
                "演示数据：未执行工具，不是内部推理或真实进度", RunStatus::Completed, Origin::Demo});
        }
    }
    if (scenario != DemoScenario::Deep) {
        m_messages.clear();
        m_cards.clear();
        MessageView example;
        example.messageId = scenario == DemoScenario::Quick ? "demo-quick" : "demo-diff";
        example.role = "assistant";
        example.origin = Origin::Demo;
        example.text = scenario == DemoScenario::Quick ?
            "演示快问：组合逻辑需覆盖所有赋值分支；这不是验收结论。" :
            "演示对比（未实际并行调用，速度与成本未实测）\n\n"
            "| 方案 | 展示范围 |\n| --- | --- |\n| 快问 | 要点 |\n| 深答 | 解释 |\n| 深答+工具 | 证据说明（演示） |";
        m_messages.push_back(std::move(example));
        RebuildEvidence();
    }

    m_runId.clear();
    m_inflight.reset();
    m_applied.reset();
    m_localDiscard = false;
    m_localDiscardRunId.clear();
    m_localDiscardGeneration = 0;
    m_cancelPending = false;
    m_cursorKind = CursorKind::None;
    m_lastU2Sequence = -1;
    m_lastV1Sequence = Version::Unknown();
    m_seenEventIds.clear();
    m_seenActionIds.clear();
    m_appliedEvents = 0;
    m_hydratedVersion = Version::Unknown();
    m_hydratedStatus = RunStatus::Unknown;
    m_hydrationKey.clear();
    m_lastRejectReason.clear();
    Notify();
    return ApplyResult::Applied;
}

bool AgentViewModel::ClearDemoData()
{
    if (!EnsureOwnerThread("ClearDemoData"))
    {
        return false;
    }
    if (!m_demoMode)
    {
        return false;
    }
    m_demoMode = false;
    ResetSessionViews();
    m_lastRejectReason.clear();
    Notify();
    return true;
}

}  // namespace agent::gui::vm
