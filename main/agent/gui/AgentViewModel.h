#pragma once
//
// AgentViewModel — AGUI-T03 类型化 ViewModel（纯状态逻辑）
//
// 目标（implementation.md AGUI-T03；S §5/6/12；STA-001/002/003/006/009/010；SYS-003/004）：
//   让四种承载形态（center/dock/float/pet）与所有组件消费**同一份**事实模型，并把四类
//   东西彻底分开：
//     * UI 形态/模式（AppMode / Form）与共享草稿、滚动锚点；
//     * 业务状态（RunStatus / CardView / PlanView / Capability …）；
//     * 异步请求（不可变 RequestContext 投递信封 + generation + ApplyResult 拒绝原因）；
//     * 演示数据（Origin::Demo + LoadDemoFixture 唯一入口；Demo payload 一律拒收）。
//
// 硬约束（本文件从设计上就不具备以下能力）：
//   * 纯 C++20 + 标准库：不 include wxWidgets、不 include jsoncpp、不联网、不读文件、
//     不写日志、不起线程/定时器。线程只有「调用方必须在 owner 线程」的强制契约：
//     非 owner 线程的**写入**在改状态之前被拒绝，并以 atomic 诊断记录（不抛异常）。
//   * 不自造后端字段：v1 与 U2 的真实字段见 phase0/README.md §2、contracts/edu-agent。
//     未知枚举/未知能力保留原文并标 Unknown，**绝不默认成成功/通过/已取消**。
//   * 不改写 DEC-05/06/07 未决语义：U2 等级不做 integer+1（Level 无升级/转换 API）；
//     取消分「本地丢弃」(CancellingLocal)、「后端已受理取消」(CancelRequested)、
//     「真正终态取消」(Cancelled) 三种状态。
//   * 不持久化任何凭据/请求体：这里只有 id、状态、版本与受限文本。
//
// 异步写入的唯一形态（防"收包后拿当前上下文伪造旧请求身份"）：
//
//     const RequestContext snapshot = model.Inflight().value();   // 发请求时冻结
//     ... 轮询/回调 ...
//     model.ApplyEvent(snapshot, event);          // envelope 校验 project/revision/
//     model.ApplyRunResult(snapshot, snapshot2);  // generation/session/run/learner
//
//   上下文必须来自**发起该请求时**冻结的 RequestContext；不得用当前 UI 状态重建。
//   本地用户消息走 AppendLocalMessage（无网络身份，模型标 Origin::Live）。
//
// 状态迁移表（ApplyRunResult / ApplyEvent 的判定顺序；逐条理由见 .cpp 顶部）：
//
//   ApplyRunResult(snapshot)
//     1 非 owner 线程                          → WrongThread（未改任何状态）
//     2 DemoMode                               → DemoLocked
//     3 payload 内含 Origin::Demo（含嵌套证据）  → DemoPayload（不洗标签）
//     4 generation 过期                         → StaleGeneration
//     5 project/revision/session/run 不符       → ForeignProject/Revision/Session/Run
//     6 learner（已绑定时缺失或不同）            → ForeignLearner
//     6b 信封 != BeginRun 时冻结的那个快照       → Rejected（禁止用当前 UI 状态重建）
//     7 该在途请求已被本地丢弃                   → LocallyDiscarded
//     8 版本本身非法（U2 前导零 / ≥2^63 …）      → Rejected
//     9 当前终态却要改写                         → Regressed
//    10 版本更旧                                 → Regressed
//    11 版本族不相容（v1 十进制 vs U2 整数…）     → Rejected
//    12 同版本同状态、尚未水合                   → Applied（补齐权威完整结果）
//    13 同版本同状态、已水合同一份               → Duplicated
//    14 同版本同状态、已水合但内容不同           → Rejected（保留先到数据）
//    15 通过                                     → Applied
//
//   ApplyEvent(snapshot, event)
//     1 非 owner 线程 → WrongThread      2 DemoMode → DemoLocked
//     3 Demo payload → DemoPayload       4–6 identity + 冻结信封 + wire 一致性
//     7 本地丢弃 → LocallyDiscarded      8 U2 sequence 为负 / v1 sequence 非法 → Rejected
//     9 序号族混用 → Rejected           10 旧序号 → Regressed / 同序号 → Duplicated
//    11 event_id 已见 → Duplicated      12 state_version 更旧 → Regressed
//    13 同版本不同状态 → Rejected       14 终态回退 → Regressed
//    15 未知状态且带原文 → Rejected     16 无状态事件（cursor-only）→ Applied
//    17 通过 → Applied
//
//   终态集合见 IsTerminal：Completed/Cancelled/Failed/BudgetExceeded（U2）与
//   Succeeded/Degraded（v1）。Unknown 既不是终态也不是成功。
//

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agent::gui::vm {

// ===========================================================================
// 1. 基础类型：模式 / 形态 / 可用性 / 来源
// ===========================================================================

enum class AppMode
{
    Learning,
    Working
};

enum class Form
{
    Center,
    Dock,
    Float,
    Pet
};

/// 能力/分区可用性。Unknown = 没有拿到可信字段，**不得**当作 Ready。
enum class Availability
{
    Unknown,
    Unavailable,
    Loading,
    Ready,
    Failed
};

/// 数据来源。Live 只能由可信适配器的 live 入口写入，Demo 只能由 LoadDemoFixture 写入。
/// live 入口**拒收** Demo payload（不"收下再改标签"）。
enum class Origin
{
    Unknown,
    Live,
    Demo
};

const char* ModeToken(AppMode mode);
const char* FormToken(Form form);
const char* AvailabilityToken(Availability availability);
const char* OriginToken(Origin origin);

struct Capability
{
    Availability state = Availability::Unknown;
    std::string reason;
    std::string detail;

    /// /capabilities 的 `{name,ready,reason,plugin_version}`：ready 是 bool。
    static Capability FromReadyFlag(bool ready, std::string reason, std::string detail = {});
    static Capability Unknown(std::string reason, std::string detail = {});
    static Capability Unavailable(std::string reason, std::string detail = {});

    bool IsReady() const { return state == Availability::Ready; }
    bool operator==(const Capability&) const = default;
};

// ===========================================================================
// 2. 版本：十进制字符串（不经 double、不降成 int64）与 U2 整数 sequence
// ===========================================================================

/// 版本序。**跨族一律 Incomparable**，reducer 不得按枚举顺序把跨族当"更新"。
enum class VersionOrder
{
    Older,
    Same,
    Newer,
    Incomparable
};

/// 后端版本/序号。五种形态对应真实合同：
///   * V1Sequence      : `agent-run.state_version`，`^[0-9]+$`（长度无界，可 > 2^64）；
///   * V1EventSequence : `agent-event.sequence`，`^[0-9]+$`，是 events?after 的游标；
///   * U2StateVersion  : `u2-state.state_version`，`^(0|[1-9][0-9]{0,18})$` 且 < 2^63；
///   * U2Sequence      : `u2-events.sequence` / `high_watermark`，JSON 整数 0..INT64_MAX。
/// 十进制族以规范化字符串（去前导零）为权威比较值；value/fitsInt64 只是方便字段，
/// 超出 int64 时 fitsInt64=false 且 value 无意义——**不能**用它比较。
struct Version
{
    enum class Kind
    {
        Unknown,
        V1Sequence,
        V1EventSequence,
        U2StateVersion,
        U2Sequence
    };

    Kind kind = Kind::Unknown;
    std::string raw;        ///< 后端原文
    std::string canonical;  ///< 规范化十进制（十进制族）；U2Sequence 为十进制文本
    std::int64_t value = 0; ///< 仅 U2Sequence 与 fitsInt64 时可信
    bool fitsInt64 = false;
    bool valid = false;     ///< 是否符合该 kind 的合同格式

    Version() = default;

    static Version Unknown();
    /// v1 run state_version：`^[0-9]+$`，任意长度。
    static Version FromV1Sequence(std::string_view text);
    /// v1 event sequence（游标）：`^[0-9]+$`，任意长度。
    static Version FromV1EventSequence(std::string_view text);
    /// U2 state_version：`^(0|[1-9][0-9]{0,18})$` 且数值 < 2^63（与 host 上限一致）。
    /// 前导零、19 位以上、≥2^63 一律 valid=false（保留原文供显示）。
    static Version FromU2StateVersion(std::string_view text);
    /// U2 整数 sequence：负数 valid=false。
    static Version FromU2Sequence(std::int64_t value);
    /// U2 整数 sequence 的十进制文本形态：`^(0|[1-9][0-9]*)$` 且 <= INT64_MAX。
    static Version FromU2SequenceText(std::string_view text);

    /// 规范化十进制：空串或含非数字 → false；去前导零（"0" 保留）。
    static bool NormalizeDecimal(std::string_view text, std::string* canonical);
    /// 有界字段（U2 整数族）用：逐字符 int64，溢出/非法 → false。
    /// **不要**用它处理 v1 的 `^[0-9]+$` 字段（可能是 18446744073709551615）。
    static bool ParseDecimalInt64(std::string_view text, std::int64_t* out);

    bool known() const { return kind != Kind::Unknown && valid; }
    bool isDecimalFamily() const
    {
        return kind == Kind::V1Sequence || kind == Kind::V1EventSequence ||
               kind == Kind::U2StateVersion;
    }
    const char* KindToken() const;
    std::string ToText() const;
    std::string CanonicalText() const { return canonical; }

    /// 语义相等：同 kind 且同规范值（raw 差异不算不同版本）。
    bool operator==(const Version& rhs) const;
};

/// 同族？Unknown 不算任何族。
bool SameVersionFamily(const Version& lhs, const Version& rhs);
/// 版本序。同族按规范值精确比较（十进制族按长度+字典序，绝不转 double）；
/// 跨族、任一非法 → Incomparable；kind==Unknown 是"还没有版本"的占位（最旧）。
VersionOrder CompareVersions(const Version& lhs, const Version& rhs);

// ===========================================================================
// 3. 运行状态：U2 九态 + v1 三态 + Unknown
// ===========================================================================

/// 统一运行状态。Failed 同时承载 U2 的 "Failed" 与 v1 的 "failed"（同一语义）。
enum class RunStatus
{
    Unknown,
    Running,
    WaitingStudent,
    WaitingEvidence,
    WaitingApproval,
    RecoveryRequired,
    Completed,
    Cancelled,
    Failed,
    BudgetExceeded,
    Succeeded,
    Degraded
};

/// 从后端原文映射；未识别返回 Unknown 并把原文写入 rawOut（rawOut 可为 nullptr）。
RunStatus RunStatusFromWire(std::string_view wire, std::string* rawOut);

const char* RunStatusToken(RunStatus status);
bool IsTerminal(RunStatus status);
bool IsWaiting(RunStatus status);
bool IsSuccess(RunStatus status);
bool IsDegraded(RunStatus status);

// ===========================================================================
// 4. 等级：显式未绑定（DEC-06 / T24 之前不定义映射）
// ===========================================================================

/// v1 是字符串 "L1".."L4"，U2 是整数 0..3，语义**尚未对齐**（DEC-06）。
/// 只有「未绑定 / v1 原文 / U2 整数」三态，**不提供 +1、升级或互转 API**。
struct Level
{
    enum class Kind
    {
        Unbound,
        V1String,
        U2Integer
    };

    Kind kind = Kind::Unbound;
    std::string text;
    int integer = 0;

    static Level Unbound();
    static Level V1(std::string text);
    static Level U2(int integer);

    bool bound() const { return kind != Kind::Unbound; }
    const char* KindToken() const;
    std::string ToText() const;

    bool operator==(const Level&) const = default;
};

// ===========================================================================
// 5. 请求上下文（不可变快照 / 投递信封）
// ===========================================================================

/// 一次请求的可信上下文。成员全 const，按值拷贝即快照。
/// 语义（B02/B03）：在途请求保存自己的快照；提交后 UI 改等级/切项目/改 revision 都
/// 不会改变在途请求，也不会让它的结果/事件写进新的绑定。空字符串**不是**通配符。
struct RequestContext
{
    const std::string projectId;
    const std::string revision;
    const std::uint64_t generation;
    const std::string sessionId;
    const std::string runId;
    const std::string learnerId;
    const Level level;

    RequestContext();
    RequestContext(std::string projectId, std::string revision, std::uint64_t generation,
                   std::string sessionId, std::string runId, std::string learnerId, Level level);

    RequestContext WithTarget(std::string projectId, std::string revision) const;
    RequestContext WithGeneration(std::uint64_t generation) const;
    RequestContext WithSession(std::string sessionId) const;
    RequestContext WithRun(std::string runId) const;
    RequestContext WithLearner(std::string learnerId) const;
    RequestContext WithLevel(Level level) const;

    bool HasTarget() const { return !projectId.empty(); }
    bool HasSession() const { return !sessionId.empty(); }
    bool HasRun() const { return !runId.empty(); }
    bool HasLearner() const { return !learnerId.empty(); }

    bool operator==(const RequestContext&) const = default;
};

// ===========================================================================
// 6. 视图：字段对齐 phase0 §2 的真实 DTO
// ===========================================================================

/// 波形证据的受限引用（只读）。字段来自已通过 Gateway `whole-signals-v1` 校验的
/// evidence.ref 对象：只有 opaque artifact ID、信号元数据、十进制 tick 窗口、
/// 有限 completeness 与跳变计数。**绝不保存 transitions/VCD 内容/路径**
/// （证据对象里的 transitions 数组根本不会被读取），也不做任何网络查询。
struct WaveformRefView
{
    std::string artifactId;    ///< opaque artifact_id（可为空 = 未提供）
    std::int64_t signalId = -1;          ///< >=0；-1 = 未提供
    std::string signalName;    ///< 有界安全文本（如 TOP.top.clk）
    std::string startTick;     ///< 1..20 位十进制字符串（64 位 tick）
    std::string endTick;
    std::string timescale;     ///< 有界安全文本（如 1ns）
    std::string completeness;  ///< exact / complete / partial
    std::int64_t transitionCount = -1;   ///< >=0；-1 = 未提供
    bool hasRange = false;     ///< start/end 都合法且 start <= end
    bool hasMetadata = false;  ///< true 仅当上述字段通过严格校验并已投影

    bool operator==(const WaveformRefView&) const = default;
};

/// 证据。只有后端明确写 hypothesis 才算假设、明确写 observation 才算观察；未知 kind
/// 两者都**不是**（B04：不把假设默默写成观察）。
///
/// N4：追加受限结构化来源元数据（source_id/artifact_id/job_id/start_line/end_line），
/// 由 AgentWireProjection 从 evidence.ref **对象**投影而来。追加在既有字段之后，
/// 因此所有既有聚合初始化写法保持不变。这里只有规范 opaque ID 与已验证的整数行号：
/// 没有路径/文件名，`source_id` 也不会被猜成文件；`hasLineRange == false` 表示
/// “没有可信范围”，不猜测、不用 0 冒充第 1 行。
struct EvidenceView
{
    std::string kind;
    std::string ref;
    std::string note;
    Origin origin = Origin::Unknown;
    std::vector<std::string> limitations;
    std::string sourceId;    ///< 规范 opaque 来源 ID（可为空）
    std::string artifactId;  ///< 规范 opaque 产物 ID（可为空）
    std::string jobId;       ///< 规范 Gateway job_id（可为空）
    int startLine = 0;       ///< 1 基、含端点；0 = 未知
    int endLine = 0;
    bool hasLineRange = false;  ///< true 仅当范围来自合法整数且 1 <= start <= end
    WaveformRefView waveform;   ///< 追加：kind=waveform 时的只读引用（默认无元数据）

    bool IsHypothesis() const;
    bool IsObservation() const;
    std::string KindLabel() const;
    /// 受限纯文本引用（无 wx、无路径、有长度上限）：
    ///   "source_id=source-… · lines=12-16" / "job_id=job-…" / "artifact_id=…"；
    ///   waveform 元数据存在时返回
    ///   "波形：artifact · signal · ticks a-b · exact（只读引用，不含波形内容）"。
    /// 没有任何结构化元数据时退回原始字符串 ref（既有字符串 ref 显示行为不变）。
    /// 只在 hasLineRange 为真时追加 lines，绝不猜测行号。
    std::string ReferenceText() const;

    bool operator==(const EvidenceView&) const = default;
};

/// 教学卡片。v1 字段：card_id/kind/level/title/body/issue_id/source(rule|model)/
/// evidence/course_refs/limitations/expired/state_version。
struct CardView
{
    std::string cardId;
    std::string kind;
    Level level;
    std::string title;
    std::string body;                 ///< 受限纯文本；不得当 HTML/脚本执行
    std::string issueId;
    std::string source;               ///< "rule" | "model" | 原文
    std::vector<EvidenceView> evidence;
    std::vector<std::string> courseRefs;
    std::vector<std::string> limitations;
    bool expired = false;
    Version stateVersion;
    std::string rawState;
    Origin origin = Origin::Unknown;
    /// Local provenance from the accepted frozen envelope; NOT a backend field.
    /// Empty for legacy/unattributed cards; never borrow the current revision.
    std::string receiptRevision;

    bool operator==(const CardView&) const = default;
};

struct PlanStepView
{
    std::string stepId;
    std::string title;
    std::string detail;
    std::string state;                ///< 原文；未知/空都**不是** completed
    Origin origin = Origin::Unknown;
    std::string capability;           ///< 规范能力名（追加字段；空 = 未提供）

    bool operator==(const PlanStepView&) const = default;
};

/// 计划卡。requires_approval 只表达合同要求，**不表达授权**（DEC-05 批准始终禁用）。
/// 只保存安全摘要：没有 params、路径、命令或脚本；完整参数永不出现在这一层。
struct PlanView
{
    bool present = false;
    std::string planId;
    std::string goal;
    std::vector<PlanStepView> steps;
    int maxJobs = 0;
    std::vector<std::string> stopConditions;
    std::string revision;
    bool requiresApproval = false;
    std::string rawState;
    Origin origin = Origin::Unknown;
    std::vector<std::string> limitations;
    std::string stateVersion;          ///< wire state_version 原文（追加字段；只读）
    bool snapshotRequired = false;     ///< wire snapshot_required（追加字段）

    bool operator==(const PlanView&) const = default;
};

/// 验证计划分区的只读文本投影（无 wx）：available=false 时只展示原因，不展示业务结论。
struct PlanSectionView
{
    bool available = false;
    std::string summary;   ///< 折叠标题摘要（如 "plan-1 · 3 步"）
    std::string body;      ///< 展开正文（bounded；无路径/命令/凭据）
    std::string reason;    ///< 不可用原因（tooltip/正文补充）

    bool operator==(const PlanSectionView&) const = default;
};

/// 把只读 PlanView 投影成 GUI 纯文本。绝不启用审批/执行：正文明确写出
/// “仅查看；审批、授权与执行尚未接入”。没有 plan、演示数据、跨版本计划都如实
/// 标注不可用，不伪造“已批准/已完成”。
PlanSectionView ProjectPlanSection(const PlanView& plan, const std::string& currentRevision,
                                   bool demoMode);

/// 文本是否像本机绝对路径/UNC/带分隔符的父目录穿越（GUI 计划/证据文本一律不显示）。
/// 只做字符串形态判断，不访问文件系统；相对文件名不算本机路径。
bool LooksLikeLocalPath(const std::string& text);

struct CourseRefView
{
    std::string passageId;
    std::string title;
    std::string ref;
    Origin origin = Origin::Unknown;
    std::vector<std::string> limitations;
    // 追加字段（v1 teaching-card `course_refs` 的只读投影；空 = 未提供）。
    std::string courseId;   ///< 规范 course_id（允许 [A-Za-z0-9._:-]，≤128）
    std::string chapter;    ///< 安全显示文本（≤128）
    std::string url;        ///< 仅 https://（≤512）；空 = 没有链接。永不自动打开/抓取。

    bool operator==(const CourseRefView&) const = default;
};

/// 课程分区。capability 决定能不能用；demo 的 refs 带 Origin::Demo，不改 capability。
struct CourseView
{
    Capability capability;
    std::vector<CourseRefView> refs;
    std::string rawState;
    Origin origin = Origin::Unknown;
    std::vector<std::string> limitations;
    std::string revision;   ///< 追加：写入时的绑定 revision（空 = 未知）；跨版本必须隐藏

    bool operator==(const CourseView&) const = default;
};

/// 课程引用分区的只读文本投影（无 wx）：不抓取、不搜索、不可点击。
struct CourseSectionView
{
    bool available = false;
    std::string summary;   ///< 折叠标题摘要（如 "2 条课程引用"）
    std::string body;      ///< bounded 正文（course_id/chapter/title/https 元数据 + 限制）
    std::string reason;    ///< 不可用/能力未就绪的原因

    bool operator==(const CourseSectionView&) const = default;
};

/// 把只读课程引用投影成 GUI 纯文本。没有 refs → 诚实“尚未生成”；demo/跨版本/
/// 能力未就绪都明确限制；url 只作为文本显示，绝不变成可点击链接或发起网络请求。
CourseSectionView ProjectCourseSection(const CourseView& course, const Capability& capability,
                                       const std::string& currentRevision, bool demoMode);

/// 仍无可信 wire 合同的只读分区（检查器 / 学习记录）的诚实不可用投影。
/// 只输出固定边界说明 + capability 原因：不显示静态全绿、分数、进度、画像、
/// 历史，也不提供运行/导出/删除入口。
struct UnavailableSectionView
{
    bool available = false;   ///< 目前恒为 false（没有可信数据源）
    std::string summary;      ///< 折叠标题摘要
    std::string body;         ///< 固定边界说明 + 能力状态
    std::string reason;       ///< 不可用原因（含能力原因）

    bool operator==(const UnavailableSectionView&) const = default;
};

UnavailableSectionView ProjectCheckerSection(const Capability& capability);
UnavailableSectionView ProjectRecordSection(const Capability& capability);

/// 「定位来源」动作的默认接线边界：**没有受信 NavigationBridge 时恒不可用**。
/// 只产出禁用原因文本；不伪造定位目标、不访问文件、不猜测路径。
struct LocationActionView
{
    bool available = false;
    std::string reason;   ///< 不可用原因（固定文案，无路径/凭据）

    bool operator==(const LocationActionView&) const = default;
};

LocationActionView ProjectLocationAction(bool trustedResolverAttached, bool eligibleTarget);

/// 教学动作分区的控件可见性/可用性（纯文本投影，无 wx）：
///   * “刷新证据”只在后端 teaching_status == "WaitingEvidence"、有活跃 run 且非 busy
///     时显示/可用（native host 只在 WaitingEvidence 接受 kind=refresh_evidence；
///     Running/WaitingStudent/WaitingApproval/RecoveryRequired/终态一律不显示）；
///   * “恢复教学”只在后端 teaching_status == "RecoveryRequired" 时可见/可用；
///   * 普通 WaitingStudent 不显示恢复入口，并给出明确的不可用原因。
/// 这里只决定显示语义；不伪造动作、不发送请求。
struct RecoveryActionView
{
    bool refreshEvidenceVisible = false;
    bool refreshEvidenceEnabled = false;
    bool recoverVisible = false;
    bool recoverEnabled = false;
    std::string recoverReason;   ///< 未显示/不可用的固定原因（无路径/凭据）

    bool operator==(const RecoveryActionView&) const = default;
};

RecoveryActionView ProjectRecoveryActions(const std::string& teachingStatus,
                                          bool activeTeachingRun, bool busy);

/// U2 committed state 的只读验证进度（来自 host/shared_adapter.py 冻结的字段：
/// completed_steps / step_count / last_observed_step_index / job_id / job_state /
/// result_evidence_ready / can_observe）。只保存有界安全摘要：
/// `result_evidence_ready`/`can_observe` 只是**后端声明**，不等于已验证，也不触发执行。
struct VerificationView
{
    bool present = false;
    int completedSteps = 0;
    int stepCount = 0;
    bool hasLastObservedStep = false;   ///< false = 后端未观察过步骤（wire null）
    int lastObservedStepIndex = 0;      ///< 0 基，且 < stepCount
    std::string jobId;                  ///< opaque job_id；空 = 未提供
    std::string jobState;               ///< 规范 job_state；空 = 未提供
    bool resultEvidenceReady = false;   ///< 后端声明，仅展示
    bool canObserve = false;            ///< 后端声明，仅展示；不新增动作

    bool operator==(const VerificationView&) const = default;
};

/// 只读验证进度摘要（无 wx、无路径、有界）：明确“只读；不等于已验证，也不表示通过”。
/// 非 present 返回空串。
std::string VerificationSummaryText(const VerificationView& verification);

/// U2 committed teaching response 的 prediction 只读摘要。只保留展示所需字段：
/// status/reason/artifact_id/信号/窗口 tick/width/timescale/outcome/origin/evidence_id/
/// limitation。`challenge_id`/`expected`/`observed`/project/revision/snapshot/job/
/// expires 等动作或绑定字段**不进入**本 DTO；params/path/command/script/凭据也不进入。
struct PredictionView
{
    bool present = false;
    std::string status;        ///< 后端状态（idle/awaiting_prediction/observed/…）
    std::string reason;        ///< 规范原因码（可空）
    std::string artifactId;    ///< opaque artifact ID（可空）
    int signalId = -1;         ///< -1 = 未提供
    std::string signalFullName;  ///< 有界安全文本（可空）
    std::string startTick;     ///< 十进制 tick（可空）
    std::string endTick;
    std::string sampleTick;
    int width = 0;             ///< 0 = 未提供
    std::string timescale;     ///< 有界安全文本（可空）
    std::string outcome;       ///< matches/differs/inconclusive（可空）
    std::string origin;        ///< fixture/tool（可空）
    std::string evidenceId;    ///< opaque evidence ID（可空）
    std::string limitation;    ///< 有界安全文本（可空）

    bool operator==(const PredictionView&) const = default;
};

/// 只读预测摘要（无 wx、无路径、有界）：明确“只读；不触发预测提交、不执行工具、
/// 不授予权限”，也不把 outcome 说成验证通过/设计正确。非 present 返回空串。
std::string PredictionSummaryText(const PredictionView& prediction);

/// 学习记录分区（DEC-07 未定义合同：默认 Unavailable，不可导出/不可删）。
struct RecordView
{
    Capability capability;
    bool present = false;
    std::string summary;
    std::string exportFormat;
    bool deletable = false;
    std::string rawState;
    Origin origin = Origin::Unknown;
    std::vector<std::string> limitations;

    bool operator==(const RecordView&) const = default;
};

/// 单项检查结论。Unknown/NotRun **不是** pass；禁止静态全绿（R07）。
struct CheckerView
{
    enum class Verdict
    {
        Unknown,
        NotRun,
        Pass,
        Fail,
        Skipped
    };

    std::string checkerId;
    std::string label;
    Verdict verdict = Verdict::Unknown;
    std::string rawState;
    std::string reason;
    Origin origin = Origin::Unknown;
    std::vector<std::string> limitations;

    bool operator==(const CheckerView&) const = default;
};

const char* VerdictToken(CheckerView::Verdict verdict);

/// 一条对话消息。rawState 是后端原始状态（不是前端 busy）。
struct MessageView
{
    std::string messageId;
    std::string role;
    std::string text;
    std::string rawState;
    std::string runId;
    Origin origin = Origin::Unknown;
    std::vector<std::string> limitations;

    bool operator==(const MessageView&) const = default;
};

// ===========================================================================
// 7. 请求生命周期 / 事件 / reducer 结果
// ===========================================================================

/// 前端请求生命周期。三种取消相关状态互不冒充。
enum class RequestState
{
    Idle,
    Busy,
    CancellingLocal,   ///< 已本地丢弃：后端可能仍在跑，**不是**终态
    CancelRequested,   ///< 后端已受理取消（202）：pending，**不是**终态
    Cancelled,         ///< 观察到真正终态 Cancelled
    Completed,         ///< 观察到终态（成功/降级）
    Failed             ///< 失败/预算超限/未知状态按失败对待
};

const char* RequestStateToken(RequestState state);

/// reducer 结果。适配器/测试必须能区分原因，不能只看 bool。
enum class ApplyResult
{
    Applied,
    Duplicated,          ///< 同一 sequence/event_id 或同一已水合结果，幂等忽略
    StaleGeneration,
    ForeignProject,
    ForeignRevision,
    ForeignSession,
    ForeignRun,
    ForeignLearner,
    Regressed,           ///< 旧版本/旧序号/终态回退
    LocallyDiscarded,
    DemoLocked,          ///< DemoMode 下拒绝 live 写入
    LiveLocked,          ///< 已有 live 会话/数据，拒绝 demo fixture 覆盖
    DemoPayload,         ///< live 入口收到 Origin::Demo 数据（拒收，不洗标签）
    WrongThread,         ///< 非 owner 线程的写入请求（未改任何状态）
    Rejected             ///< 其它不合法（合同违约、未知状态、缺去重键等）
};

const char* ApplyResultToken(ApplyResult result);

/// 一条后端事件。v1（agent-event.schema.json）：sequence 是十进制字符串、event_id 唯一、
/// type 是 session/created 之类。U2（u2-events.schema.json）：sequence/action_id/
/// state_version/status。
/// **action_id 不是事件唯一 ID**：同一 action 的 stage_verification 与 commit 会发多条
/// 不同事件（host/store.py），因此去重键是 event_id 或 sequence，action_id 只作审计。
struct Event
{
    std::string sessionId;                 ///< wire 可能不带；带了必须与信封一致
    std::string runId;
    std::string eventId;                   ///< v1 `event_id`（^ev-[1-9][0-9]*$）
    std::string actionId;                  ///< U2 `action_id`：审计用，非唯一 ID
    std::optional<std::int64_t> sequence;  ///< U2 整数 sequence（0..INT64_MAX）
    std::string sequenceText;              ///< v1 十进制 sequence（^[0-9]+$，可超 int64）
    Version stateVersion;                  ///< v1/U2 state_version（v1 事件通常没有）
    RunStatus status = RunStatus::Unknown;
    std::string rawStatus;                 ///< 后端状态原文（未知也保留）
    std::string type;                      ///< v1 `type` 原文
    std::string detail;                    ///< 受限公开摘要，不是思维链
    std::vector<CardView> cards;
    std::vector<std::string> limitations;
    std::string learnerId;
    Origin origin = Origin::Unknown;
};

/// 一次运行的权威完整结果（v1 create-run 的 data / U2 state 投影）。
struct RunSnapshot
{
    RunStatus status = RunStatus::Unknown;
    std::string rawStatus;
    Version version;
    std::vector<CardView> cards;
    std::string modelUsed;            ///< 后端布尔原文 "true"/"false"/""（不猜）
    std::vector<std::string> limitations;
    std::string reason;
    /// v1 plan-card 的只读投影（追加字段）。present=false 表示这次结果没有计划；
    /// 绝不携带可执行授权：审批/执行入口始终不由此字段驱动。
    PlanView plan;
    /// v1 teaching-card `course_refs` 的只读投影（追加字段）。refs 为空 = 没有引用；
    /// 只有规范元数据，没有课程正文、进度或跳转授权。
    CourseView course;
    /// U2 teaching response 的只读验证进度（追加字段）。present=false = 没有该字段；
    /// v1 run 永不填充它（ProjectTeachingRun 是唯一写入路径）。
    VerificationView verification;
    /// U2 teaching response 的只读 prediction 摘要（追加字段）。present=false = 没有该字段；
    /// v1 run 永不填充它（ProjectTeachingRun 是唯一写入路径）。
    PredictionView prediction;

    bool ModelUsed() const { return modelUsed == "true"; }
};

// ===========================================================================
// 8. 能力集合与状态投影
// ===========================================================================

enum class CapabilityKey
{
    Course,
    TeachingCore,
    Model,
    EdaExecution,
    Record,
    BackendCancel,
    Checkers
};

const char* CapabilityKeyToken(CapabilityKey key);

/// 默认值全部来自 phase0 §2 实测事实（无一是 Ready）。只有 SetCapability* 能改。
struct CapabilitySet
{
    Capability course;
    Capability teachingCore;
    Capability model;
    Capability edaExecution;
    Capability record;
    Capability backendCancel;
    Capability checkers;
    std::vector<std::string> modes;
    std::string protocol;

    CapabilitySet();
    const Capability& Get(CapabilityKey key) const;
    Capability& Get(CapabilityKey key);

    bool operator==(const CapabilitySet&) const = default;
};

/// 后端原始状态与前端 busy 分开表达。
struct StatusView
{
    RunStatus status = RunStatus::Unknown;
    std::string rawStatus;
    bool busy = false;
    RequestState request = RequestState::Idle;

    Version version;
    std::uint64_t generation = 0;
    std::string projectId;
    std::string revision;
    std::string sessionId;
    std::string runId;
    Level level;
    std::string modelUsed;
    std::string reason;
    std::vector<std::string> limitations;
    /// 当前 (version,status) 是否已由权威完整结果补齐（events 只给状态时是 false）。
    bool hydrated = false;
    Origin origin = Origin::Unknown;

    bool ModelUsed() const { return modelUsed == "true"; }

    bool operator==(const StatusView&) const = default;
};

struct ScrollAnchor
{
    std::string messageId;   ///< 空 = 贴在末尾（默认跟随）
    int offsetPx = 0;

    bool operator==(const ScrollAnchor&) const = default;
};

/// T14: public lifecycle observations, not hidden reasoning or invented tool steps.
struct StageView
{
    std::string id;          ///< accepted event id/sequence; demo ids are explicitly prefixed
    std::string title;       ///< locally mapped, allowlisted event label
    std::string summary;     ///< public status only; never Event::detail/raw trace
    RunStatus status = RunStatus::Unknown;
    Origin origin = Origin::Unknown;
    bool operator==(const StageView&) const = default;
};

enum class DemoScenario { Quick, Deep, Diff };

// ===========================================================================
// 9. 订阅令牌（RAII、可重入安全、可活过模型）
// ===========================================================================

class AgentViewModel;

/// 订阅令牌：析构/Reset 即解绑；只能移动。
/// * 在 owner 线程 Reset → 立即从模型移除；
/// * 在**非** owner 线程 Reset → 只把该槽标成 dead（atomic），不动物理容器，下一次
///   owner 线程通知时惰性清理；不抛异常。
/// * 可以比模型活得久（模型析构会把 owner 置空）。
class Subscription
{
public:
    Subscription() noexcept = default;
    ~Subscription();

    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    /// 幂等。
    void Reset() noexcept;
    bool Active() const noexcept;

private:
    friend class AgentViewModel;
    struct Control
    {
        const std::thread::id ownerThread = std::this_thread::get_id();
        AgentViewModel* owner = nullptr;   ///< 仅 owner 线程读写；模型析构置 nullptr
        std::atomic<bool> alive{true};     ///< 任意线程可置 false
    };

    Subscription(std::shared_ptr<Control> control, std::uint64_t id) noexcept;

    std::shared_ptr<Control> m_control;
    std::uint64_t m_id = 0;
};

// ===========================================================================
// 10. AgentViewModel：绑定 + reducer + 共享 UI 状态 + demo fixture
// ===========================================================================

/// 单一事实模型的纯状态实现。不可拷贝/移动（令牌与监听器持有 this）。
///
/// 线程契约：**所有**写入/订阅/退订必须在 owner 线程（构造该对象的线程）调用。
/// 非 owner 线程调用在改状态之前被拒绝：Apply* 返回 WrongThread、bool 接口返回 false、
/// void 接口 no-op、Subscribe 返回 inactive 令牌；并记录 SawForeignThreadCall()/
/// LastForeignThreadOp()/ForeignThreadCallCount()（atomic，不抛异常）。读取同样应在
/// owner 线程进行；本类不加锁。
class AgentViewModel
{
public:
    using Listener = std::function<void()>;

    AgentViewModel();
    ~AgentViewModel();

    AgentViewModel(const AgentViewModel&) = delete;
    AgentViewModel& operator=(const AgentViewModel&) = delete;
    AgentViewModel(AgentViewModel&&) = delete;
    AgentViewModel& operator=(AgentViewModel&&) = delete;

    // ----- 线程/所有权诊断 -----
    bool IsOwnerThread() const;
    bool SawForeignThreadCall() const { return m_foreignThreadCall.load(std::memory_order_acquire); }
    const char* LastForeignThreadOp() const;
    unsigned long long ForeignThreadCallCount() const;

    // ----- 模式与形态（切换不清空会话/草稿/滚动锚点）-----
    AppMode Mode() const { return m_mode; }
    void SetMode(AppMode mode);
    Form CurrentForm() const { return m_form; }
    void SetForm(Form form);

    // ----- 共享草稿与滚动锚点（所有形态共用）-----
    const std::string& Draft() const { return m_draft; }
    void SetDraft(std::string text);
    void SetScrollAnchor(ScrollAnchor anchor);
    void ClearScrollAnchor();

    // ----- 绑定与运行 -----
    /// 显式绑定。任何变化（project/revision/generation/session）都会让旧 run 事实失效：
    /// run 绑定、在途快照、卡片/证据/计划、busy/状态清空，session/project 变化时连
    /// 会话内分区一起清空。**不会**带着新 revision/generation 标签继续显示旧 run。
    /// 同 project+session 时 generation 不允许倒退（否则旧结果会重新生效）→ 返回 false。
    bool BindSession(std::string projectId, std::string revision, std::uint64_t generation,
                     std::string sessionId);
    bool BindRun(std::string runId, Level level);
    /// 冻结本次请求的 RequestContext（含 level 与 learner 快照），busy=true。
    bool BeginRun(std::string sessionId, std::string runId, Level level);
    bool BeginRun(std::string runId);   ///< 用当前 session 与 UI 等级

    /// 单调递增；到 UINT64_MAX 不回绕（返回原值并记录原因）。
    /// 代际前进即当前请求上下文失效：清 run 绑定/在途快照/busy，但保留已渲染内容。
    std::uint64_t BumpGeneration();
    /// 只允许前进；g < 当前 → false（防止旧结果复活）。语义同 BumpGeneration。
    bool SetGeneration(std::uint64_t generation);
    std::uint64_t Generation() const { return m_generation; }
    const std::string& BoundProjectId() const { return m_projectId; }
    const std::string& BoundRevision() const { return m_revision; }
    const std::string& BoundSessionId() const { return m_sessionId; }
    const std::string& BoundRunId() const { return m_runId; }
    const std::string& BoundLearnerId() const { return m_learnerId; }
    void SetLearnerId(std::string learnerId);
    /// 发请求时冻结的上下文；轮询事件必须复用它（不得用当前 UI 状态重建）。
    std::optional<RequestContext> Inflight() const { return m_inflight; }
    bool HasInflight() const { return m_inflight.has_value(); }

    Level CurrentLevel() const { return m_uiLevel; }
    void SetLevel(Level level);

    // ----- reducer（异步入口必须带不可变信封）-----
    ApplyResult ApplyRunResult(const RequestContext& ctx, const RunSnapshot& snapshot);
    /// 便捷重载：散参转成 RunSnapshot（语义相同）。
    ApplyResult ApplyRunResult(const RequestContext& ctx, RunStatus status, std::string rawStatus,
                               Version version, std::vector<CardView> cards, std::string modelUsed,
                               std::vector<std::string> limitations);
    ApplyResult ApplyEvent(const RequestContext& ctx, const Event& event);
    /// 后端投递的消息（同样要信封）。
    ApplyResult ApplyMessage(const RequestContext& ctx, MessageView message);
    /// 本地用户/助手消息（无网络身份；模型标 Origin::Live；Demo payload 拒收）。
    ApplyResult AppendLocalMessage(MessageView message);

    // ----- 取消（三种状态互不冒充）-----
    /// 本地丢弃：只表示"前端不再等待/不再显示这条结果"，后端可能仍在跑。
    /// 只置 CancellingLocal、busy=false，绝不产生终态；该 run 的后续写入被拒。
    bool BeginLocalCancel();
    /// 请求后端取消：仅 Capabilities().backendCancel==Ready 时可用（默认不可用）。
    /// 受理后 request=CancelRequested 且**保持 pending/busy**，直到真实终态。
    bool RequestBackendCancel();
    bool LocalDiscardActive() const { return m_localDiscard; }
    bool CancelPending() const { return m_cancelPending; }
    RequestState Request() const { return m_status.request; }

    // ----- 能力（唯一写入口；fixture/live 数据路径都不改）-----
    const CapabilitySet& Capabilities() const { return m_capabilities; }
    void SetCapability(CapabilityKey key, Capability capability);
    void SetCapabilities(CapabilitySet set);
    void SetCapabilityModes(std::vector<std::string> modes);

    // ----- demo fixture（唯一 demo 入口）-----
    /// 写入全部 Origin::Demo 的示例数据并置 DemoMode；已在 DemoMode 时幂等重载。
    /// 已有 live 会话/数据 → LiveLocked 且不改任何字段。不写 capabilities、不造 Pass。
    ApplyResult LoadDemoFixture(DemoScenario scenario = DemoScenario::Deep);
    /// 退出演示的唯一方式；非 DemoMode 返回 false。
    bool ClearDemoData();
    bool DemoMode() const { return m_demoMode; }

    // ----- 查询 -----
    const std::vector<MessageView>& Messages() const { return m_messages; }
    const std::vector<StageView>& Stages() const { return m_stages; }
    std::uint64_t OmittedStageCount() const { return m_omittedStages; }
    const std::vector<CardView>& Cards() const { return m_cards; }
    const std::vector<EvidenceView>& Evidence() const { return m_evidence; }
    const PlanView& Plan() const { return m_plan; }
    const CourseView& Course() const { return m_course; }
    const VerificationView& Verification() const { return m_verification; }
    const PredictionView& Prediction() const { return m_prediction; }
    const RecordView& Record() const { return m_record; }
    const std::vector<CheckerView>& Checkers() const { return m_checkers; }
    const StatusView& Status() const { return m_status; }
    const std::string& LastRejectReason() const { return m_lastRejectReason; }
    /// 当前 (version,status) 是否已由权威完整结果补齐。
    bool Hydrated() const { return m_status.hydrated; }
    /// U2 整数游标；-1 = 没有。
    std::int64_t LastSequence() const { return m_lastU2Sequence; }
    Version LastV1Sequence() const { return m_lastV1Sequence; }
    std::uint64_t AppliedEventCount() const { return m_appliedEvents; }
    const std::vector<std::string>& AppliedActionIds() const { return m_seenActionIds; }
    std::optional<RequestContext> AppliedContext() const { return m_applied; }

    // ----- 订阅 -----
    /// 回调在 owner 线程同步调用；重入安全：回调内订阅/解绑不会使迭代失效，
    /// 本轮新增的监听器不在本轮被调用。返回 RAII 令牌。
    Subscription Subscribe(Listener listener);
    std::size_t ListenerCount() const { return m_listeners.size(); }

private:
    friend class Subscription;

    enum class CursorKind
    {
        None,
        U2Int,
        V1Decimal
    };

    struct ListenerEntry
    {
        std::uint64_t id = 0;
        Listener fn;
        std::shared_ptr<Subscription::Control> control;
    };

    bool EnsureOwnerThread(const char* operation);
    bool MatchesFrozenEnvelope(const RequestContext& ctx) const;
    void Unsubscribe(std::uint64_t id) noexcept;
    void Notify();
    void PruneDeadListeners();
    void RebuildEvidence();
    void RecordPublicStage(const Event& event);
    void ResetSessionViews();
    void InvalidateRunFacts();
    void ApplyStatusRequestState(RunStatus status);
    void RecordReject(ApplyResult result, std::string reason);
    ListenerEntry* FindEntry(std::uint64_t id);

    AppMode m_mode = AppMode::Learning;
    Form m_form = Form::Center;
    std::string m_draft;
    ScrollAnchor m_scrollAnchor;

    std::string m_projectId;
    std::string m_revision;
    std::string m_sessionId;
    std::string m_runId;
    std::string m_learnerId;
    std::uint64_t m_generation = 0;
    Level m_uiLevel = Level::V1("L2");
    std::optional<RequestContext> m_inflight;
    std::optional<RequestContext> m_applied;

    bool m_localDiscard = false;
    std::uint64_t m_localDiscardGeneration = 0;
    std::string m_localDiscardRunId;
    bool m_cancelPending = false;

    StatusView m_status;
    std::vector<MessageView> m_messages;
    std::vector<StageView> m_stages;
    std::uint64_t m_omittedStages = 0;
    std::vector<CardView> m_cards;
    std::vector<EvidenceView> m_evidence;
    PlanView m_plan;
    CourseView m_course;
    VerificationView m_verification;
    PredictionView m_prediction;
    RecordView m_record;
    std::vector<CheckerView> m_checkers;

    CapabilitySet m_capabilities;
    bool m_demoMode = false;

    CursorKind m_cursorKind = CursorKind::None;
    std::int64_t m_lastU2Sequence = -1;
    Version m_lastV1Sequence;
    std::vector<std::string> m_seenEventIds;
    std::vector<std::string> m_seenActionIds;
    std::uint64_t m_appliedEvents = 0;

    /// 已水合的权威完整结果指纹（同版本同状态只补齐一次，冲突保留先到数据）。
    Version m_hydratedVersion;
    RunStatus m_hydratedStatus = RunStatus::Unknown;
    std::string m_hydrationKey;

    std::string m_lastRejectReason;
    std::vector<ListenerEntry> m_listeners;
    std::uint64_t m_nextListenerId = 1;

    std::thread::id m_ownerThread;
    std::atomic<bool> m_foreignThreadCall{false};
    std::atomic<const char*> m_lastForeignThreadOp{nullptr};
    std::atomic<unsigned long long> m_foreignThreadCalls{0};

public:
    // --- 与设计同名的别名访问器（Form() / ScrollAnchor()）---
    // 声明了与类型同名的成员函数后，类体内再写裸 `Form` / `ScrollAnchor` 会解析成函数
    // 而不是类型（[basic.scope.class]）。这两个访问器**必须**保持为类体最后一段声明，
    // 新成员一律加在它们之前。
    Form Form() const { return m_form; }
    const ScrollAnchor& ScrollAnchor() const { return m_scrollAnchor; }
};

}  // namespace agent::gui::vm
