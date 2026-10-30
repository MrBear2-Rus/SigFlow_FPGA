#include "agent/gui/AgentWireProjection.h"
#include "agent/gui/AgentSelectionContext.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace agent::gui {
namespace {
std::string String(const Json::Value& value, const char* key)
{
    return value.isObject() && value[key].isString() ? value[key].asString() : std::string();
}

std::vector<std::string> Strings(const Json::Value& value)
{
    std::vector<std::string> result;
    if (value.isArray()) for (const auto& item : value) if (item.isString()) result.push_back(item.asString());
    return result;
}

/// 只接受规范 opaque ID：长度 1..256、仅 [A-Za-z0-9_-]（与选择证据/服务端同一口径）。
/// `path` / `file` / `absolute_path` 这类字段根本不在白名单里，因此永远不会被投影；
/// 冒充 ID 的本机路径（`rtl/top.v`、`C:\…`、`\\server\…`）也过不了 opaque 校验。
std::string OpaqueId(const Json::Value& value, const char* key)
{
    const auto text = String(value, key);
    return selection::SafeOpaqueId(text) ? text : std::string();
}

/// 行号只接受 JSON 整数（1..2^31-1）。字符串 `"12"`、浮点 `12.5`、0、负数、
/// 超范围与缺失都不算：没有范围就不猜，也不用 0 冒充第 1 行。
bool PositiveLine(const Json::Value& value, const char* key, int& line)
{
    const auto& item = value[key];
    if (item.isInt())
    {
        const int candidate = item.asInt();
        if (candidate >= 1)
        {
            line = candidate;
            return true;
        }
        return false;
    }
    if (item.isUInt())
    {
        const auto candidate = item.asUInt();
        if (candidate >= 1u && candidate <= 2147483647u)
        {
            line = static_cast<int>(candidate);
            return true;
        }
    }
    return false;
}

/// 证据引用投影。
///   * 字符串 ref：旧行为，原样保留（不解析、不打开）；
///   * 对象 ref：只投影规范 ID 与合法行号范围；start_line/end_line 必须同时合法且
///     start <= end 才置 hasLineRange，否则完全丢弃（保留字段默认 0/false）。
void ProjectRef(const Json::Value& value, vm::EvidenceView& evidence)
{
    if (value.isString())
    {
        evidence.ref = value.asString();
        return;
    }
    if (!value.isObject()) return;
    evidence.sourceId = OpaqueId(value, "source_id");
    evidence.artifactId = OpaqueId(value, "artifact_id");
    evidence.jobId = OpaqueId(value, "job_id");
    const auto evidenceId = OpaqueId(value, "evidence_id");
    evidence.ref = !evidence.sourceId.empty() ? evidence.sourceId
                  : !evidence.artifactId.empty() ? evidence.artifactId
                  : !evidence.jobId.empty() ? evidence.jobId
                                            : evidenceId;
    int start = 0;
    int end = 0;
    if (PositiveLine(value, "start_line", start) && PositiveLine(value, "end_line", end) &&
        start <= end)
    {
        evidence.startLine = start;
        evidence.endLine = end;
        evidence.hasLineRange = true;
    }
}

vm::Capability Flag(const Json::Value& object, const char* flag, const char* reason)
{
    if (!object.isObject() || !object[flag].isBool()) return vm::Capability::Unknown("服务未提供能力状态");
    return vm::Capability::FromReadyFlag(object[flag].asBool(), String(object, reason));
}

// ---------------------------------------------------------------------------
// v1 plan-card（只读）：严格白名单投影，任何不合法都 fail closed。
//
// 合同：contracts/edu-agent/v1/schemas/plan-card.schema.json
//   * plan_id ^plan-[1-9][0-9]*$、steps 1..16、max_jobs 1..16、
//     requires_approval/snapshot_required 恒为 true、state_version ^[0-9]+$；
//   * step：step_id ^s[1-9][0-9]*$、capability 只允许 eda.synth/eda.sim.build/eda.sim.run、
//     params 只允许受限标量键（出现 path/file/command/script/workdir/output 等键即拒绝）。
//
// 边界：**只读**。params 的值/键都不会进入 PlanView；不产生批准、授权、执行、
// 取消能力，也不携带任何路径、命令或脚本。
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxPlanIdBytes = 64;
constexpr std::size_t kMaxGoalBytes = 512;
constexpr std::size_t kMaxDescriptionBytes = 256;
constexpr std::size_t kMaxStopConditionBytes = 200;
constexpr std::size_t kMaxPlanSteps = 16;
constexpr std::size_t kMaxStepDetailBytes = 320;

std::string ClampUtf8(std::string text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    text.resize(cut);
    text += "…";
    return text;
}

bool Digits(const std::string& text, std::size_t minLength, std::size_t maxLength)
{
    if (text.size() < minLength || text.size() > maxLength) return false;
    return std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool PlanIdOk(const std::string& planId)
{
    if (planId.size() < 6 || planId.size() > kMaxPlanIdBytes) return false;
    if (planId.compare(0, 5, "plan-") != 0) return false;
    if (planId[5] < '1' || planId[5] > '9') return false;
    return Digits(planId.substr(5), 1, kMaxPlanIdBytes - 5);
}

bool StepIdOk(const std::string& stepId)
{
    if (stepId.size() < 2 || stepId.size() > kMaxPlanIdBytes || stepId[0] != 's') return false;
    if (stepId[1] < '1' || stepId[1] > '9') return false;
    return Digits(stepId.substr(1), 1, kMaxPlanIdBytes - 1);
}

bool CapabilityAllowed(const std::string& capability)
{
    return capability == "eda.synth" || capability == "eda.sim.build" || capability == "eda.sim.run";
}

/// 只允许 [A-Za-z0-9._:-] 的规范文本（revision 等）。
bool CanonicalToken(const std::string& text, std::size_t maxLength)
{
    if (text.empty() || text.size() > maxLength) return false;
    return std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == ':' || ch == '-';
    });
}

/// 受限单行文本（goal/description/stop_conditions/disabled_reason）：
/// 非空、无控制字符、不是本机路径；有界裁剪后写入 PlanView。
bool DisplayText(const Json::Value& value, std::size_t maxBytes, std::string& out)
{
    if (!value.isString()) return false;
    std::string text = value.asString();
    if (text.empty()) return false;
    for (char& ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20u || byte == 0x7Fu) ch = ' ';
    }
    if (vm::LooksLikeLocalPath(text)) return false;
    text = ClampUtf8(std::move(text), maxBytes);
    out = std::move(text);
    return !out.empty();
}

/// 受限标量（opts/strategy）：无分隔符、无引号、无空白；只用于合法性判定，
/// 值本身**不会**进入 GUI。
bool ScalarOption(const std::string& text, std::size_t maxLength)
{
    if (text.empty() || text.size() > maxLength) return false;
    return std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
               ch == '+' || ch == ':' || ch == '=' || ch == ',';
    });
}

bool Identifier(const std::string& text)
{
    if (text.empty() || text.size() > 128) return false;
    if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') ||
          text[0] == '_'))
        return false;
    return std::all_of(text.begin() + 1, text.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '$';
    });
}

/// params 只允许合同白名单键；出现 path/file/command/script/workdir/output 等
/// 任何其它键即 fail closed。值只校验形状，绝不投影。
bool SafeParams(const Json::Value& params)
{
    if (!params.isObject()) return false;
    for (auto it = params.begin(); it != params.end(); ++it)
    {
        const std::string key = it.name();
        if (key == "top_module" || key == "testbench")
        {
            if (!it->isString() || !Identifier(it->asString())) return false;
        }
        else if (key == "strategy")
        {
            if (!it->isString() || !ScalarOption(it->asString(), 64)) return false;
        }
        else if (key == "duration_ticks")
        {
            if (!it->isString() || !Digits(it->asString(), 1, 20)) return false;
        }
        else if (key == "build_step_id")
        {
            if (!it->isString() || !StepIdOk(it->asString())) return false;
        }
        else if (key == "opts")
        {
            if (!it->isArray() || it->size() > 16) return false;
            for (const auto& option : *it)
            {
                if (!option.isString() || !ScalarOption(option.asString(), 128)) return false;
            }
        }
        else
        {
            return false;
        }
    }
    return true;
}

/// plan-card 独有的结构字段。教学卡不会携带这些字段（见
/// agent/src/sigflow_edu_agent/cards.py 的 build_teaching_card）。
/// 注意：`state_version` **不在此列**——它是教学卡的常规合同字段，
/// 单独出现不能作为“这是计划卡”的判别式；但对象**没有** card_id 时，
/// 只有 state_version 也说明它不可能是教学卡（按其 plan 分支 fail closed）。
bool HasPlanStructureField(const Json::Value& item)
{
    for (const char* key : {"plan_id", "goal", "steps", "max_jobs", "stop_conditions",
                            "requires_approval", "snapshot_required", "revision"})
    {
        if (item.isMember(key)) return true;
    }
    return false;
}

bool LooksLikePlanCard(const Json::Value& item)
{
    if (!item.isObject()) return false;
    // 带 card_id 的对象只要出现任何 plan-card 结构字段，就按畸形计划处理：
    // 进入 plan 分支后 fail closed（绝不回落成普通教学卡）。
    if (item.isMember("card_id")) return HasPlanStructureField(item);
    // 没有 card_id：任何一个 plan 字段（含 state_version / revision）都说明它不是教学卡。
    return HasPlanStructureField(item) || item.isMember("state_version");
}

std::string JoinStepIds(const std::vector<std::string>& ids)
{
    if (ids.empty()) return "无";
    std::string joined;
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        if (index > 0) joined += ", ";
        joined += ids[index];
    }
    return joined;
}

/// 依赖图必须是 DAG（16 步上限，直接 DFS）。
bool HasCycle(const std::vector<std::vector<std::size_t>>& edges)
{
    enum class Mark
    {
        White,
        Gray,
        Black
    };
    std::vector<Mark> marks(edges.size(), Mark::White);
    struct Frame
    {
        std::size_t node;
        std::size_t next;
    };
    for (std::size_t start = 0; start < edges.size(); ++start)
    {
        if (marks[start] != Mark::White) continue;
        std::vector<Frame> stack{{start, 0}};
        marks[start] = Mark::Gray;
        while (!stack.empty())
        {
            Frame& frame = stack.back();
            if (frame.next >= edges[frame.node].size())
            {
                marks[frame.node] = Mark::Black;
                stack.pop_back();
                continue;
            }
            const std::size_t next = edges[frame.node][frame.next++];
            if (marks[next] == Mark::Gray) return true;
            if (marks[next] == Mark::White)
            {
                marks[next] = Mark::Gray;
                stack.push_back({next, 0});
            }
        }
    }
    return false;
}

/// 把一张 plan-card 投影成只读 vm::PlanView。返回 false 表示结构非法（调用方 fail closed）。
bool ProjectPlanCard(const Json::Value& item, vm::PlanView& plan, std::string& error)
{
    const std::string planId = String(item, "plan_id");
    if (!PlanIdOk(planId))
    {
        error = "计划卡片的 plan_id 不符合 ^plan-[1-9][0-9]*$。";
        return false;
    }
    std::string goal;
    if (!DisplayText(item["goal"], kMaxGoalBytes, goal))
    {
        error = "计划卡片缺少可显示的 goal。";
        return false;
    }
    const std::string revision = String(item, "revision");
    if (!CanonicalToken(revision, 128))
    {
        error = "计划卡片的 revision 缺失或格式非法。";
        return false;
    }
    const std::string stateVersion = String(item, "state_version");
    if (!Digits(stateVersion, 1, 20))
    {
        error = "计划卡片的 state_version 必须是十进制字符串。";
        return false;
    }
    if (!item["requires_approval"].isBool() || !item["requires_approval"].asBool())
    {
        error = "计划卡片必须声明 requires_approval=true（本 GUI 不提供批准入口）。";
        return false;
    }
    if (!item["snapshot_required"].isBool() || !item["snapshot_required"].asBool())
    {
        error = "计划卡片必须声明 snapshot_required=true。";
        return false;
    }
    if (!item["max_jobs"].isInt() || item["max_jobs"].asInt() < 1 || item["max_jobs"].asInt() > 16)
    {
        error = "计划卡片的 max_jobs 必须是 1..16 的整数。";
        return false;
    }
    const auto& conditions = item["stop_conditions"];
    if (!conditions.isArray() || conditions.empty() || conditions.size() > 16)
    {
        error = "计划卡片的 stop_conditions 必须是 1..16 条。";
        return false;
    }
    std::vector<std::string> stopConditions;
    for (const auto& condition : conditions)
    {
        std::string text;
        if (!DisplayText(condition, kMaxStopConditionBytes, text))
        {
            error = "计划卡片的 stop_conditions 含缺失或不可显示的条目。";
            return false;
        }
        stopConditions.push_back(std::move(text));
    }
    const auto& steps = item["steps"];
    if (!steps.isArray() || steps.empty() || steps.size() > kMaxPlanSteps)
    {
        error = "计划卡片的 steps 必须是 1..16 步。";
        return false;
    }

    std::vector<vm::PlanStepView> views;
    std::vector<std::string> stepIds;
    std::vector<std::vector<std::string>> dependencies;
    views.reserve(steps.size());
    stepIds.reserve(steps.size());
    dependencies.reserve(steps.size());
    for (const auto& step : steps)
    {
        if (!step.isObject())
        {
            error = "计划卡片的步骤不是对象。";
            return false;
        }
        const std::string stepId = String(step, "step_id");
        if (!StepIdOk(stepId) ||
            std::find(stepIds.begin(), stepIds.end(), stepId) != stepIds.end())
        {
            error = "计划卡片的 step_id 非法或重复。";
            return false;
        }
        const std::string capability = String(step, "capability");
        if (!CapabilityAllowed(capability))
        {
            error = "计划步骤的能力不在 eda.synth/eda.sim.build/eda.sim.run 白名单内。";
            return false;
        }
        std::string description;
        if (!DisplayText(step["description"], kMaxDescriptionBytes, description))
        {
            error = "计划步骤缺少可显示的 description。";
            return false;
        }
        if (!step["params"].isObject() || !SafeParams(step["params"]))
        {
            error = "计划步骤的 params 含未授权键或非法值（不投影、不放行）。";
            return false;
        }
        const auto& dependsOn = step["depends_on"];
        if (!dependsOn.isArray() || dependsOn.size() > kMaxPlanSteps)
        {
            error = "计划步骤的 depends_on 必须是数组。";
            return false;
        }
        std::vector<std::string> stepDependencies;
        for (const auto& dependency : dependsOn)
        {
            if (!dependency.isString())
            {
                error = "计划步骤的依赖不是字符串 ID。";
                return false;
            }
            const std::string dependencyId = dependency.asString();
            if (!StepIdOk(dependencyId) || dependencyId == stepId)
            {
                error = "计划步骤的依赖 ID 非法或自依赖。";
                return false;
            }
            stepDependencies.push_back(dependencyId);
        }
        if (!step["max_jobs"].isInt() || step["max_jobs"].asInt() != 1)
        {
            error = "计划步骤的 max_jobs 必须恒为 1。";
            return false;
        }
        const bool hasEnabled = step["enabled"].isBool();
        const bool enabled = hasEnabled && step["enabled"].asBool();
        std::string disabledReason;
        const bool hasReason = step.isMember("disabled_reason");
        if (hasReason && !DisplayText(step["disabled_reason"], kMaxDescriptionBytes, disabledReason))
        {
            error = "计划步骤的 disabled_reason 不可显示。";
            return false;
        }
        if (hasEnabled && !enabled && !hasReason)
        {
            error = "计划步骤声明不可用但缺少 disabled_reason。";
            return false;
        }

        vm::PlanStepView view;
        view.stepId = stepId;
        view.capability = capability;
        view.title = ClampUtf8(capability + " · " + description, 200);
        view.detail = "依赖：" + JoinStepIds(stepDependencies);
        if (hasReason && !enabled)
        {
            view.detail += " · 不可用：" + disabledReason;
        }
        view.detail = ClampUtf8(std::move(view.detail), kMaxStepDetailBytes);
        view.state = hasEnabled ? (enabled ? "enabled" : "disabled") : std::string();
        view.origin = vm::Origin::Live;
        views.push_back(std::move(view));
        stepIds.push_back(stepId);
        dependencies.push_back(std::move(stepDependencies));
    }

    // 依赖必须指向本计划内的步骤，且整张图必须无环。
    std::vector<std::vector<std::size_t>> edges(dependencies.size());
    for (std::size_t index = 0; index < dependencies.size(); ++index)
    {
        for (const std::string& dependencyId : dependencies[index])
        {
            const auto found = std::find(stepIds.begin(), stepIds.end(), dependencyId);
            if (found == stepIds.end())
            {
                error = "计划步骤依赖了计划外的 step_id。";
                return false;
            }
            edges[index].push_back(static_cast<std::size_t>(found - stepIds.begin()));
        }
    }
    if (HasCycle(edges))
    {
        error = "计划步骤依赖成环，拒绝投影。";
        return false;
    }

    plan = vm::PlanView{};
    plan.present = true;
    plan.planId = planId;
    plan.goal = goal;
    plan.steps = std::move(views);
    plan.maxJobs = item["max_jobs"].asInt();
    plan.stopConditions = std::move(stopConditions);
    plan.revision = revision;
    plan.requiresApproval = true;   // 合同恒为 true；本 GUI 仍不提供批准入口
    plan.snapshotRequired = true;
    plan.stateVersion = stateVersion;
    plan.origin = vm::Origin::Live;
    plan.rawState.clear();          // plan-card 没有运行状态；不编造
    return true;
}

// ---------------------------------------------------------------------------
// v1 teaching-card `course_refs`（只读）：严格白名单投影，非法即 fail closed。
//
// 合同：contracts/edu-agent/v1/schemas/teaching-card.schema.json `$defs.courseRef`
//   对象必须含 course_id/chapter/title（字符串）与 url（字符串或 null）。
//   只投影有界元数据：没有课程正文、没有 HTML、没有进度、没有跳转授权。
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxCourseRefsPerCard = 8;
constexpr std::size_t kMaxCourseRefsPerRun = 16;
constexpr std::size_t kMaxCourseIdBytes = 128;
constexpr std::size_t kMaxChapterBytes = 128;
constexpr std::size_t kMaxCourseTitleBytes = 256;
constexpr std::size_t kMaxCourseUrlBytes = 512;

/// course_id 只接受规范 token（与 revision 同一字符集），拒绝路径/空白/脚本。
bool CourseIdOk(const std::string& courseId)
{
    return CanonicalToken(courseId, kMaxCourseIdBytes);
}

/// 明显的凭据/脚本样式判定由 AgentSelectionContext 统一提供
/// （`selection::LooksLikeSensitiveText`）：课程引用、波形 timescale、恢复问题
/// 共用同一份口径，避免各处各写一份安全规则。

/// url 只允许空/null 或 https://<safe-host>…；拒绝凭据、反斜杠、控制字符、
/// 相对/本机路径与其它协议。**不发起任何请求、不解析 HTML。**
bool SafeCourseUrl(const Json::Value& value, std::string& out)
{
    out.clear();
    if (value.isNull()) return true;
    if (!value.isString()) return false;
    std::string url = value.asString();
    if (url.empty()) return true;
    if (url.size() > kMaxCourseUrlBytes) return false;
    if (url.rfind("https://", 0) != 0) return false;
    const std::string rest = url.substr(8);
    if (rest.empty()) return false;
    const std::size_t hostEnd = rest.find_first_of("/?#");
    const std::string host =
        rest.substr(0, hostEnd == std::string::npos ? rest.size() : hostEnd);
    if (host.empty() || host.front() == '.' || host.back() == '.') return false;
    for (const char ch : host)
    {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
        if (!safe) return false;
    }
    if (host.find("..") != std::string::npos) return false;
    for (const char ch : url)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20u || byte == 0x7Fu) return false;   // 空白/控制字符
        if (ch == '\\' || ch == '@' || ch == '"' || ch == '<' || ch == '>' || ch == '\'')
        {
            return false;   // 凭据/转义/HTML 注入面
        }
    }
    out = std::move(url);
    return true;
}

/// 解析一条 course_ref：对象（合同形状）或旧字符串（passageId/title 兼容）。
/// 所有字符串都先做**原始长度**上限检查（超长直接 fail closed，不截断），
/// 再做安全显示文本检查（无控制字符、非本机路径）与敏感文本检查
/// （凭据 key/value、脚本/本地协议）。错误只返回固定说明，绝不回显输入。
bool ParseCourseRef(const Json::Value& entry, vm::CourseRefView& ref)
{
    ref = vm::CourseRefView{};
    ref.origin = vm::Origin::Live;
    const auto within = [](const Json::Value& value, std::size_t maxBytes) {
        return value.isString() && !value.asString().empty() && value.asString().size() <= maxBytes;
    };
    if (entry.isString())
    {
        if (!within(entry, kMaxCourseTitleBytes)) return false;
        std::string text;
        if (!DisplayText(entry, kMaxCourseTitleBytes, text)) return false;
        if (selection::LooksLikeSensitiveText(text)) return false;
        ref.passageId = text;
        ref.title = text;
        return true;
    }
    if (!entry.isObject()) return false;
    const std::string courseId = String(entry, "course_id");
    if (!CourseIdOk(courseId)) return false;
    if (!within(entry["chapter"], kMaxChapterBytes)) return false;
    if (!within(entry["title"], kMaxCourseTitleBytes)) return false;
    std::string chapter;
    if (!DisplayText(entry["chapter"], kMaxChapterBytes, chapter)) return false;
    std::string title;
    if (!DisplayText(entry["title"], kMaxCourseTitleBytes, title)) return false;
    if (selection::LooksLikeSensitiveText(courseId) || selection::LooksLikeSensitiveText(chapter) ||
        selection::LooksLikeSensitiveText(title))
    {
        return false;
    }
    if (!entry.isMember("url")) return false;
    std::string url;
    if (!SafeCourseUrl(entry["url"], url)) return false;
    ref.courseId = courseId;
    ref.chapter = chapter;
    ref.title = title;
    ref.url = url;
    return true;
}

/// 同一 run 只允许一个一致的课程引用集合：完全重复的去重，冲突（同 course_id/chapter
/// 但标题或链接不同）直接 fail closed。
bool AppendCourseRef(const vm::CourseRefView& ref, std::vector<vm::CourseRefView>& refs,
                     std::string& error)
{
    for (const vm::CourseRefView& existing : refs)
    {
        if (existing.courseId == ref.courseId && existing.passageId == ref.passageId &&
            existing.chapter == ref.chapter)
        {
            if (existing.title == ref.title && existing.url == ref.url) return true;
            error = "同一 run 内的课程引用不一致（course_id/chapter 相同但标题或链接不同）。";
            return false;
        }
    }
    if (refs.size() >= kMaxCourseRefsPerRun)
    {
        error = "课程引用超过单 run 上限。";
        return false;
    }
    refs.push_back(ref);
    return true;
}

// ---------------------------------------------------------------------------
// 波形证据（kind=waveform）：只投影受限元数据，绝不读取 transitions/VCD。
//
// 事实来源：agent/src/sigflow_edu_agent/dispatch.py `_verified_waveform_evidence`
// 在 Gateway whole-signals-v1 通过 exact 校验后生成的 ref：
//   artifact_id / signal_id / signal_name / start_tick / end_tick / timescale /
//   completeness / transition_count。
// 规则：opaque artifact ID；signal_id 有界非负整数；signal_name 有界安全文本
// （无控制符/路径/凭据）；tick 为 1..20 位十进制且 start<=end；timescale 有界
// 安全文本；completeness ∈ {exact, complete, partial}；transition_count 有界
// 非负整数。字段缺失/非法整份 run fail closed；错误只返回固定说明。
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxSignalNameBytes = 256;
constexpr std::size_t kMaxTimescaleBytes = 32;
constexpr std::int64_t kMaxWaveformCounter = 2147483647;  // int32 上限

/// 十进制 tick：1..20 位、只含 [0-9]（64 位 tick 的十进制字符串）。
bool TickText(const Json::Value& value, std::string& out)
{
    if (!value.isString()) return false;
    const std::string text = value.asString();
    if (text.empty() || text.size() > 20) return false;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9') return false;
    }
    out = text;
    return true;
}

/// 十进制字符串比较（去前导零后先比长度再比字典序，不经过整数类型）。
int CompareDecimal(const std::string& lhs, const std::string& rhs)
{
    const auto strip = [](const std::string& text) {
        const std::size_t first = text.find_first_not_of('0');
        return first == std::string::npos ? std::string("0") : text.substr(first);
    };
    const std::string left = strip(lhs);
    const std::string right = strip(rhs);
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    if (left == right) return 0;
    return left < right ? -1 : 1;
}

bool NonNegativeInt(const Json::Value& value, const char* key, std::int64_t maxValue,
                    std::int64_t& out)
{
    const auto& item = value[key];
    if (item.isInt())
    {
        const std::int64_t candidate = item.asInt();
        if (candidate >= 0 && candidate <= maxValue)
        {
            out = candidate;
            return true;
        }
        return false;
    }
    if (item.isUInt())
    {
        const auto candidate = item.asUInt();
        if (candidate <= static_cast<unsigned int>(maxValue))
        {
            out = static_cast<std::int64_t>(candidate);
            return true;
        }
    }
    return false;
}

/// 把一条 waveform 证据的 ref 投影成受限 WaveformRefView。
/// 字符串 ref 保持旧行为（原样显示、无结构化元数据）；对象 ref 严格校验。
bool ProjectWaveformRef(const Json::Value& value, vm::EvidenceView& evidence, std::string& error)
{
    if (value.isString())
    {
        evidence.ref = value.asString();
        return true;
    }
    if (!value.isObject())
    {
        error = "波形证据缺少可解析的引用。";
        return false;
    }
    vm::WaveformRefView wave;
    wave.artifactId = OpaqueId(value, "artifact_id");
    if (wave.artifactId.empty())
    {
        error = "波形证据的 artifact_id 缺失或不是 opaque ID。";
        return false;
    }
    if (!NonNegativeInt(value, "signal_id", kMaxWaveformCounter, wave.signalId))
    {
        error = "波形证据的 signal_id 不是有界非负整数。";
        return false;
    }
    const auto withinString = [](const Json::Value& item, std::size_t maxBytes) {
        return item.isString() && !item.asString().empty() && item.asString().size() <= maxBytes;
    };
    if (!withinString(value["signal_name"], kMaxSignalNameBytes) ||
        !DisplayText(value["signal_name"], kMaxSignalNameBytes, wave.signalName) ||
        selection::LooksLikeSensitiveText(wave.signalName))
    {
        error = "波形证据的 signal_name 缺失、超长或不可显示。";
        return false;
    }
    if (!TickText(value["start_tick"], wave.startTick) ||
        !TickText(value["end_tick"], wave.endTick) || CompareDecimal(wave.startTick, wave.endTick) > 0)
    {
        error = "波形证据的 tick 区间缺失、非法或反向。";
        return false;
    }
    // timescale 与 signal_name 一样是后端提供的显示文本：除长度/路径外，
    // 还必须拒绝凭据 key/value（api_key=/token:/secret=/password=/authorization:）
    // 与脚本/本地协议样式（javascript:/data:/file:/vbscript:），但正常 "1ns"/"1ps" 不受影响。
    if (!withinString(value["timescale"], kMaxTimescaleBytes) ||
        !DisplayText(value["timescale"], kMaxTimescaleBytes, wave.timescale) ||
        selection::LooksLikeSensitiveText(wave.timescale))
    {
        error = "波形证据的 timescale 缺失、超长或不可显示。";
        return false;
    }
    wave.completeness = String(value, "completeness");
    if (wave.completeness != "exact" && wave.completeness != "complete" &&
        wave.completeness != "partial")
    {
        error = "波形证据的 completeness 不在 exact/complete/partial 之内。";
        return false;
    }
    if (!NonNegativeInt(value, "transition_count", kMaxWaveformCounter, wave.transitionCount))
    {
        error = "波形证据的 transition_count 不是有界非负整数。";
        return false;
    }
    wave.hasRange = true;
    wave.hasMetadata = true;
    evidence.artifactId = wave.artifactId;
    evidence.ref = wave.artifactId;
    evidence.waveform = std::move(wave);
    return true;
}

// ---------------------------------------------------------------------------
// U2 committed state `verification`（只读摘要）：字段集合由 host/shared_adapter.py
// 冻结为 completed_steps/step_count/last_observed_step_index/job_id/job_state/
// result_evidence_ready/can_observe；未知键、错误类型、越界/负数、非法 job_id 或
// job_state、非法索引关系都整 run fail-closed。绝不复制 detail/params/路径。
// ---------------------------------------------------------------------------

bool VerificationInt(const Json::Value& value, int& out)
{
    if (value.isInt())
    {
        out = value.asInt();
        return true;
    }
    if (value.isUInt() && value.asUInt() <= 2147483647u)
    {
        out = static_cast<int>(value.asUInt());
        return true;
    }
    return false;
}

bool ProjectVerification(const Json::Value& data, vm::VerificationView& verification,
                         std::string& error)
{
    verification = vm::VerificationView{};
    if (!data.isMember("verification") || data["verification"].isNull()) return true;
    const Json::Value& value = data["verification"];
    if (!value.isObject())
    {
        error = "U2 状态里的验证进度格式无效。";
        return false;
    }
    static const char* const kKeys[] = {"completed_steps", "step_count",
                                        "last_observed_step_index", "job_id", "job_state",
                                        "result_evidence_ready", "can_observe"};
    for (const char* key : kKeys)
    {
        if (!value.isMember(key))
        {
            error = "U2 状态里的验证进度缺少必需字段。";
            return false;
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it)
    {
        const std::string name = it.name();
        bool known = false;
        for (const char* key : kKeys) known = known || name == key;
        if (!known)
        {
            error = "U2 状态里的验证进度包含未知字段。";
            return false;
        }
    }
    int completed = 0;
    int count = 0;
    if (!VerificationInt(value["completed_steps"], completed) || completed < 0 ||
        !VerificationInt(value["step_count"], count) || count < 1 || count > 1024 ||
        completed > count)
    {
        error = "U2 状态里的验证进度步骤计数无效。";
        return false;
    }
    bool hasLast = false;
    int last = 0;
    if (!value["last_observed_step_index"].isNull())
    {
        if (!VerificationInt(value["last_observed_step_index"], last) || last < 0 ||
            last >= count)
        {
            error = "U2 状态里的验证进度最近观察步骤无效。";
            return false;
        }
        hasLast = true;
    }
    std::string jobId;
    if (!value["job_id"].isNull())
    {
        if (!value["job_id"].isString() ||
            !selection::SafeOpaqueId(value["job_id"].asString()))
        {
            error = "U2 状态里的验证进度 job_id 非法。";
            return false;
        }
        jobId = value["job_id"].asString();
    }
    std::string jobState;
    if (!value["job_state"].isNull())
    {
        static const char* const kJobStates[] = {
            "Created", "Validating", "Queued", "Running", "ValidatingArtifact", "Cancelling",
            "Succeeded", "Failed", "Cancelled", "TimedOut", "Rejected", "Unknown"};
        if (!value["job_state"].isString())
        {
            error = "U2 状态里的验证进度 job_state 非法。";
            return false;
        }
        const std::string state = value["job_state"].asString();
        bool known = false;
        for (const char* candidate : kJobStates) known = known || state == candidate;
        if (!known)
        {
            error = "U2 状态里的验证进度 job_state 非法。";
            return false;
        }
        jobState = state;
    }
    if (!value["result_evidence_ready"].isBool() || !value["can_observe"].isBool())
    {
        error = "U2 状态里的验证进度布尔字段无效。";
        return false;
    }
    verification.present = true;
    verification.completedSteps = completed;
    verification.stepCount = count;
    verification.hasLastObservedStep = hasLast;
    verification.lastObservedStepIndex = hasLast ? last : 0;
    verification.jobId = std::move(jobId);
    verification.jobState = std::move(jobState);
    verification.resultEvidenceReady = value["result_evidence_ready"].asBool();
    verification.canObserve = value["can_observe"].asBool();
    return true;
}

// ---------------------------------------------------------------------------
// U2 committed teaching response `prediction`（只读摘要）。
// 公开键集合由 host/shared_adapter.py 冻结（23 个）；未知键、错误类型、非法
// status/outcome/origin、反向/非法 tick、路径/凭据、坏 sha256/challenge 都 fail-closed。
// challenge/expected/observed/project/revision/snapshot/job/expires 只用于校验，
// **不进入** GUI 摘要。
// ---------------------------------------------------------------------------

bool IsSha256Text(const std::string& text)
{
    std::string hex = text;
    if (hex.rfind("sha256:", 0) == 0) hex = hex.substr(7);
    if (hex.size() != 64) return false;
    for (const char ch : hex)
    {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

bool IsBitStringText(const std::string& text)
{
    if (text.empty() || text.size() > 8192) return false;
    for (const char ch : text)
    {
        if (ch != '0' && ch != '1' && ch != 'x' && ch != 'X' && ch != 'z' && ch != 'Z')
        {
            return false;
        }
    }
    return true;
}

bool IsLowerHex32(const std::string& text)
{
    if (text.size() != 32) return false;
    for (const char ch : text)
    {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool ProjectPrediction(const Json::Value& data, vm::PredictionView& prediction, std::string& error)
{
    prediction = vm::PredictionView{};
    if (!data.isMember("prediction") || data["prediction"].isNull()) return true;
    const Json::Value& value = data["prediction"];
    if (!value.isObject())
    {
        error = "U2 状态里的预测摘要格式无效。";
        return false;
    }
    static const char* const kKeys[] = {
        "status", "reason", "challenge_id", "artifact_id", "signal_id", "start_tick",
        "end_tick", "sample_tick", "full_name", "width", "timescale", "artifact_hash",
        "project_id", "revision", "snapshot_id", "job_id", "expires_at_epoch", "expected",
        "observed", "outcome", "origin", "evidence_id", "limitation"};
    for (auto it = value.begin(); it != value.end(); ++it)
    {
        const std::string name = it.name();
        bool known = false;
        for (const char* key : kKeys) known = known || name == key;
        if (!known)
        {
            error = "U2 状态里的预测摘要包含未知字段。";
            return false;
        }
    }
    const auto optionalString = [&value](const char* key, std::string& out) {
        if (!value.isMember(key) || value[key].isNull()) return true;
        if (!value[key].isString()) return false;
        out = value[key].asString();
        return true;
    };
    // status：必须是源码实际产生的枚举值。
    static const char* const kStatuses[] = {
        "idle", "disabled", "awaiting_prediction", "observing", "observed", "unavailable",
        "expired", "revalidation_required", "cancelled"};
    if (!value["status"].isString())
    {
        error = "U2 状态里的预测摘要缺少 status。";
        return false;
    }
    vm::PredictionView projected;
    projected.status = value["status"].asString();
    bool statusKnown = false;
    for (const char* candidate : kStatuses) statusKnown = statusKnown || projected.status == candidate;
    if (!statusKnown)
    {
        error = "U2 状态里的预测摘要 status 非法。";
        return false;
    }
    std::string reason;
    if (!optionalString("reason", reason) || (!reason.empty() && !CanonicalToken(reason, 64)))
    {
        error = "U2 状态里的预测摘要 reason 非法。";
        return false;
    }
    projected.reason = std::move(reason);

    std::string artifactId;
    if (!optionalString("artifact_id", artifactId) ||
        (!artifactId.empty() && !selection::SafeOpaqueId(artifactId)))
    {
        error = "U2 状态里的预测摘要 artifact_id 非法。";
        return false;
    }
    std::string challengeId;
    if (!optionalString("challenge_id", challengeId) ||
        (!challengeId.empty() && !IsLowerHex32(challengeId)))
    {
        error = "U2 状态里的预测摘要 challenge_id 非法。";
        return false;
    }
    std::string hash;
    if (!optionalString("artifact_hash", hash) || (!hash.empty() && !IsSha256Text(hash)))
    {
        error = "U2 状态里的预测摘要 artifact_hash 非法。";
        return false;
    }
    std::string expires;
    if (!optionalString("expires_at_epoch", expires) ||
        (!expires.empty() && !Digits(expires, 1, 20)))
    {
        error = "U2 状态里的预测摘要 expires_at_epoch 非法。";
        return false;
    }
    // 绑定字段：prediction 若声明 project/revision/snapshot，必须与外层 U2 state 一致。
    static const char* const kBindings[] = {"project_id", "revision", "snapshot_id"};
    for (const char* key : kBindings)
    {
        std::string claimed;
        if (!optionalString(key, claimed)) return false;
        if (!claimed.empty() && claimed != String(data, key))
        {
            error = "U2 状态里的预测摘要绑定与外层状态不一致。";
            return false;
        }
    }
    std::string jobId;
    if (!optionalString("job_id", jobId) || (!jobId.empty() && !selection::SafeOpaqueId(jobId)))
    {
        error = "U2 状态里的预测摘要 job_id 非法。";
        return false;
    }
    std::string expected;
    if (!optionalString("expected", expected) ||
        (!expected.empty() && !IsBitStringText(expected)))
    {
        error = "U2 状态里的预测摘要 expected 非法。";
        return false;
    }
    std::string observed;
    if (!optionalString("observed", observed) || (!observed.empty() && !IsBitStringText(observed)))
    {
        error = "U2 状态里的预测摘要 observed 非法。";
        return false;
    }

    // 信号与窗口（展示字段）。
    if (!optionalString("full_name", projected.signalFullName) ||
        (!projected.signalFullName.empty() &&
         (!DisplayText(value["full_name"], 256, projected.signalFullName) ||
          selection::LooksLikeSensitiveText(projected.signalFullName))))
    {
        error = "U2 状态里的预测摘要 full_name 非法。";
        return false;
    }
    if (value.isMember("signal_id") && !value["signal_id"].isNull())
    {
        if (!VerificationInt(value["signal_id"], projected.signalId) || projected.signalId < 0)
        {
            error = "U2 状态里的预测摘要 signal_id 非法。";
            return false;
        }
    }
    if (value.isMember("width") && !value["width"].isNull())
    {
        if (!VerificationInt(value["width"], projected.width) || projected.width < 1 ||
            projected.width > 4096)
        {
            error = "U2 状态里的预测摘要 width 非法。";
            return false;
        }
    }
    std::string timescale;
    if (!optionalString("timescale", timescale) ||
        (!timescale.empty() && (!DisplayText(value["timescale"], 32, timescale) ||
                                selection::LooksLikeSensitiveText(timescale))))
    {
        error = "U2 状态里的预测摘要 timescale 非法。";
        return false;
    }
    projected.timescale = std::move(timescale);
    std::string limitation;
    if (!optionalString("limitation", limitation) ||
        (!limitation.empty() &&
         (!DisplayText(value["limitation"], 256, limitation) ||
          selection::LooksLikeSensitiveText(limitation))))
    {
        error = "U2 状态里的预测摘要 limitation 非法。";
        return false;
    }
    projected.limitation = std::move(limitation);
    std::string outcome;
    if (!optionalString("outcome", outcome) ||
        (!outcome.empty() && outcome != "matches" && outcome != "differs" &&
         outcome != "inconclusive"))
    {
        error = "U2 状态里的预测摘要 outcome 非法。";
        return false;
    }
    projected.outcome = std::move(outcome);
    std::string origin;
    if (!optionalString("origin", origin) ||
        (!origin.empty() && origin != "fixture" && origin != "tool"))
    {
        error = "U2 状态里的预测摘要 origin 非法。";
        return false;
    }
    projected.origin = std::move(origin);
    std::string evidenceId;
    if (!optionalString("evidence_id", evidenceId) ||
        (!evidenceId.empty() && !selection::SafeOpaqueId(evidenceId)))
    {
        error = "U2 状态里的预测摘要 evidence_id 非法。";
        return false;
    }
    projected.evidenceId = std::move(evidenceId);

    // 三个 tick：只接受 1..20 位十进制；同时给出时要求 start <= sample <= end。
    const auto tickField = [&value](const char* key, std::string& out) {
        if (!value.isMember(key) || value[key].isNull()) return true;
        return TickText(value[key], out);
    };
    if (!tickField("start_tick", projected.startTick) ||
        !tickField("end_tick", projected.endTick) ||
        !tickField("sample_tick", projected.sampleTick))
    {
        error = "U2 状态里的预测摘要 tick 非法。";
        return false;
    }
    if (!projected.startTick.empty() && !projected.endTick.empty() &&
        CompareDecimal(projected.startTick, projected.endTick) > 0)
    {
        error = "U2 状态里的预测摘要 tick 区间反向。";
        return false;
    }
    if (!projected.sampleTick.empty() && !projected.startTick.empty() &&
        CompareDecimal(projected.sampleTick, projected.startTick) < 0)
    {
        error = "U2 状态里的预测摘要采样点越界。";
        return false;
    }
    if (!projected.sampleTick.empty() && !projected.endTick.empty() &&
        CompareDecimal(projected.sampleTick, projected.endTick) > 0)
    {
        error = "U2 状态里的预测摘要采样点越界。";
        return false;
    }
    projected.artifactId = std::move(artifactId);
    projected.present = true;
    prediction = std::move(projected);
    return true;
}

}  // namespace

bool ProjectV1Run(const Json::Value& data, vm::RunSnapshot& out, std::string& error)
{
    if (!data.isObject() || !data["state"].isString() || !data["state_version"].isString() ||
        !data["cards"].isArray() || !data["model_used"].isBool()) {
        error = "Agent 返回的运行结果缺少必要字段。";
        return false;
    }
    vm::RunSnapshot result;
    result.rawStatus = data["state"].asString();
    result.status = vm::RunStatusFromWire(result.rawStatus, nullptr);
    result.version = vm::Version::FromV1Sequence(data["state_version"].asString());
    if (!result.version.valid || (result.status != vm::RunStatus::Succeeded &&
        result.status != vm::RunStatus::Degraded && result.status != vm::RunStatus::Failed)) {
        error = "Agent 返回了不兼容的运行状态或版本。";
        return false;
    }
    result.modelUsed = data["model_used"].asBool() ? "true" : "false";
    // 本次 run 的课程引用（跨卡片聚合；同一 run 只能有一个一致集合）。
    std::vector<vm::CourseRefView> courseRefs;
    for (const auto& item : data["cards"]) {
        // v1 plan-card 不是教学卡：它有 plan_id/goal/steps 而没有 card_id/kind/
        // title/body。合法就投影到只读 PlanView（一次运行最多一张），非法则
        // fail closed，绝不把它当普通教学卡渲染，也绝不把 params 带进界面。
        if (LooksLikePlanCard(item)) {
            if (item.isMember("card_id")) {
                error = "计划卡片同时携带 card_id：契约冲突，拒绝投影。";
                return false;
            }
            if (result.plan.present) {
                error = "同一运行返回了多张计划卡片，拒绝投影。";
                return false;
            }
            if (!ProjectPlanCard(item, result.plan, error)) return false;
            continue;
        }
        if (!item.isObject() || !item["card_id"].isString() || !item["kind"].isString() ||
            !item["title"].isString() || !item["body"].isString()) {
            error = "Agent 返回了无法显示的卡片。";
            return false;
        }
        vm::CardView card;
        card.cardId = String(item, "card_id");
        card.kind = String(item, "kind");
        card.title = String(item, "title");
        card.body = String(item, "body");
        card.issueId = String(item, "issue_id");
        card.source = String(item, "source");
        if (item["level"].isString()) card.level = vm::Level::V1(item["level"].asString());
        if (item["expired"].isBool()) card.expired = item["expired"].asBool();
        if (item["state_version"].isString()) card.stateVersion = vm::Version::FromV1Sequence(item["state_version"].asString());
        card.limitations = Strings(item["limitations"]);
        card.origin = vm::Origin::Live;
        if (item["evidence"].isArray()) for (const auto& entry : item["evidence"]) {
            if (!entry.isObject()) continue;
            vm::EvidenceView evidence;
            evidence.kind = String(entry, "kind");
            if (evidence.kind == "waveform") {
                // 波形证据只投影受限元数据；transitions/VCD 内容永不读取。
                if (!ProjectWaveformRef(entry["ref"], evidence, error)) return false;
            } else {
                ProjectRef(entry["ref"], evidence);
            }
            evidence.note = String(entry, "note");
            evidence.limitations = Strings(entry["limitations"]);
            evidence.origin = vm::Origin::Live;
            card.evidence.push_back(std::move(evidence));
        }
        if (item.isMember("course_refs")) {
            const auto& refs = item["course_refs"];
            if (!refs.isArray() || refs.size() > kMaxCourseRefsPerCard) {
                error = "教学卡片的 course_refs 必须是长度有界的数组。";
                return false;
            }
            for (const auto& entry : refs) {
                vm::CourseRefView courseRef;
                if (!ParseCourseRef(entry, courseRef)) {
                    error = "教学卡片包含不可信的课程引用（缺字段/路径/不安全链接/超长/敏感文本）。";
                    return false;
                }
                // 既有 CardView.courseRefs 仍只保留有界显示标题（对话行兼容）。
                card.courseRefs.push_back(courseRef.title);
                if (!AppendCourseRef(courseRef, courseRefs, error)) return false;
            }
        }
        result.cards.push_back(std::move(card));
    }
    if (data["omitted"].isArray()) for (const auto& item : data["omitted"]) {
        const auto reason = String(item, "reason");
        if (!reason.empty()) result.limitations.push_back(reason);
    }
    if (!courseRefs.empty()) {
        // 每个 run 一个一致的只读课程引用集合：没有课程正文、没有进度、没有跳转授权。
        result.course.refs = std::move(courseRefs);
        result.course.origin = vm::Origin::Live;
        result.course.rawState = result.rawStatus;
        result.course.limitations.push_back(
            "课程引用只读：不抓取课程正文，不提供搜索、进度或跳转授权。");
        result.course.limitations.push_back("链接只作为文本展示，不会自动打开。");
    }
    out = std::move(result);
    return true;
}

vm::CapabilitySet ProjectCapabilities(const Json::Value& data)
{
    vm::CapabilitySet result;
    result.protocol = String(data, "protocol");
    result.model = Flag(data["model"], "available", "reason");
    result.course = Flag(data["course"], "available", "reason");
    result.teachingCore = Flag(data["teaching_core"], "available", "reason");
    result.edaExecution = Flag(data["execution"], "can_run_eda", "note");
    if (data["modes"].isArray()) for (const auto& mode : data["modes"]) {
        if (mode.isObject() && mode["available"].isBool() && mode["available"].asBool()) {
            auto name = String(mode, "id");
            if (!name.empty()) result.modes.push_back(std::move(name));
        }
    }
    return result;
}

bool ProjectTeachingRun(const Json::Value& data, vm::RunSnapshot& out, std::string& error)
{
    if (!data.isObject() || String(data, "schema_version") != "u1.u2-teaching.v1" ||
        !data["state_version"].isString() || !data["cards"].isArray()) {
        error = "报告教学返回了不兼容的状态。";
        return false;
    }
    const auto status = String(data, "teaching_status");
    std::string raw;
    const auto mapped = vm::RunStatusFromWire(status, &raw);
    const auto version = vm::Version::FromU2StateVersion(data["state_version"].asString());
    if (mapped == vm::RunStatus::Unknown || !version.valid) {
        error = "报告教学返回了未知状态或非法版本。";
        return false;
    }
    // verification 只从 U2 teaching response 解析（v1 run 永不携带它）。
    vm::VerificationView verification;
    if (!ProjectVerification(data, verification, error)) return false;
    // prediction 同理：只在 U2 teaching response 里解析。
    vm::PredictionView prediction;
    if (!ProjectPrediction(data, prediction, error)) return false;
    Json::Value compatible(Json::objectValue);
    compatible["state"] = "degraded";
    compatible["state_version"] = data["state_version"];
    // u2-state.schema.json 允许 additive `plan_card`（对象）。这里只把它送入既有的
    // 严格 ProjectPlanCard 白名单路径（作为兼容 payload 里的一张计划卡），
    // 不复制 params/path/command/script/workdir，也不把它当教学卡。
    Json::Value cards(Json::arrayValue);
    for (const auto& card : data["cards"]) cards.append(card);
    bool hasPlanCard = false;
    if (data.isMember("plan_card") && !data["plan_card"].isNull()) {
        if (!data["plan_card"].isObject()) {
            error = "U2 状态里的计划卡格式无效。";
            return false;
        }
        hasPlanCard = true;
        cards.append(data["plan_card"]);
    }
    compatible["cards"] = cards;
    compatible["omitted"] = data["omitted"];
    compatible["model_used"] = data["run"].isObject() && data["run"]["model_used"].isBool()
        ? data["run"]["model_used"] : Json::Value(false);
    vm::RunSnapshot projected;
    if (!ProjectV1Run(compatible, projected, error)) return false;
    if (hasPlanCard) {
        // 计划卡必须属于这份 committed state：revision/state_version 不一致即整 run 拒绝。
        if (!projected.plan.present) {
            error = "U2 状态里的计划卡未被识别。";
            return false;
        }
        if (projected.plan.revision != String(data, "revision")) {
            error = "U2 状态里的计划卡 revision 与状态不一致。";
            return false;
        }
        if (projected.plan.stateVersion != String(data, "state_version")) {
            error = "U2 状态里的计划卡 state_version 与状态不一致。";
            return false;
        }
    }
    out = std::move(projected);
    out.verification = std::move(verification);
    out.prediction = std::move(prediction);
    out.status = mapped;
    out.rawStatus = status;
    out.version = version;
    for (auto& card : out.cards)
        if (card.stateVersion.kind != vm::Version::Kind::Unknown)
            card.stateVersion = vm::Version::FromU2StateVersion(card.stateVersion.raw);
    if (!data["run"].isObject() || !data["run"]["model_used"].isBool()) out.modelUsed.clear();
    return true;
}
}  // namespace agent::gui
