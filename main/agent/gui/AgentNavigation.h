#pragma once
//
// AgentNavigation — AGUI-T09 局部顶栏、功能 rail、aw-head 形态入口与教学辅助导航
//
// 设计源：GUI/agent-gui/index.html H87–102（.titlebar）、H309–325（.rail/.rb/.lv-seg）、
//         H365–445（品牌/模式/等级/rail 七导航/四形态）、H193–207（.aw-head/.aw-tools/
//         .aw-status）、H988–1012（setForm 同步）、H732–763（setMode/newAsk/六分区跳转）；
//         spec.md §4.1/§4.2/§5.1/§5.4/§5.6/§5.7；design-review.md C03/C07/C15、DEC-10/14。
//
// 边界（本文件是唯一动作/分区注册表，但**不是**第二套状态）：
//   * 模式(学习/工作)、提示级别、草稿、消息、能力/可用性、demo 标记**全部来自**
//     `vm::AgentViewModel`；本层只做「模型 → 控件」投影与「用户意图 → 模型/协调器」回调。
//     因此 nav 控件**不持有**模型引用、不订阅模型、不缓存业务状态（避免模型先析构）。
//   * 形态(Center/Dock/Float/Pet)的唯一真源是 `AgentFormCoordinator`；rail 与 aw-head
//     两组按钮的 pressed 都由工作区用同一个 `SetFormState()` 回写，绝不各存一份。
//   * 禁止新增 quick/deep/diff 模式选择器；禁止把 E4/model_used=false/policy/课程/
//     检查器写成常量。模型缺字段一律显式「未知/未接通」，不得冒充成功。
//   * 装饰性元素（顶栏红绿灯）只绘制，不建子窗口、不绑定任何动作。
//   * 所有颜色/字号/圆角/间距只取 AgentTokens / UiTheme 的 Agent 命名空间（DEC-13）；
//     本文件与 AgentNavigation.cpp 不得出现 hex 或 wxColour(r,g,b) 字面量。
//
// 单位：全部 DIP（CSS px == DIP），交给 wx 前经 tokens::CssPxToDip / UiFontPx。

#include "agent/gui/AgentFormCoordinator.h"  // agent::gui::AgentForm
#include "agent/gui/AgentIcons.h"            // icons::IconId
#include "agent/gui/AgentViewModel.h"        // vm::AppMode / vm::Level / vm::AgentViewModel
#include "ui/UiTheme.h"                      // UiTheme::Subscription（主题订阅句柄）

#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/string.h>

#include <functional>
#include <vector>

class wxWindow;

namespace agent::gui::controls {
class AgentButton;
class AgentPill;
class AgentSegmented;
}  // namespace agent::gui::controls

namespace agent::gui::nav {

// ===========================================================================
// 1. 动作注册表（rail 与 aw-head 共用；顺序 = index.html 视觉顺序）
// ===========================================================================

enum class Action : int
{
    NewAsk = 0,     ///< .rb.primary 新建提问（清草稿 + 回 center，保留历史）
    ToggleGuide,    ///< rGuide：学习/工作双向联动（pressed = 学习模式）
    JumpIssue,      ///< 当前问题
    JumpEvidence,   ///< 证据链
    JumpPlan,       ///< 验证计划
    JumpCourse,     ///< 课程引用
    JumpRecord,     ///< 学习记录
    JumpChecker,    ///< 检查器与状态
    FormCenter,     ///< qCenter / aCenter
    FormDock,       ///< qDock / aDock
    FormFloat,      ///< qFloat / aFloat
    FormPet,        ///< qPet / aPet
    Count
};

/// 按钮类别（决定视觉与 pressed 语义）。
enum class Group
{
    Primary,  ///< .rb.primary 蓝渐变、不可 toggle
    Toggle,   ///< rGuide：pressed 表示「学习路径显示中」
    Jump,     ///< 六个分区直达：可点击（目标区给内容或明确原因）
    Form      ///< 四形态：pressed 表示当前形态
};

struct Item
{
    Action action;
    Group group;
    icons::IconId icon;         ///< AGUI-T06 稳定图标 ID
    const char* id;             ///< 稳定英文 ID（"newAsk"/"rGuide"/"qCenter"…），日志与测试用
    const char* label;          ///< 中文短标签（tooltip）
    const char* accessibleName; ///< 读屏名称（原型 aria-label）
    int order;                  ///< 视觉顺序（0 起，连续）
};

/// 12 个动作，`order` 连续；`Catalog().size() == static_cast<int>(Action::Count)`。
const std::vector<Item>& Catalog();

/// 未找到返回 nullptr（不抛异常）。
const Item* Find(Action action);
const Item* FindById(const char* id);

/// 形态动作 ↔ AgentForm。非形态动作返回 false / AgentForm::None。
bool IsFormAction(Action action);
AgentForm FormOf(Action action);
bool ActionForForm(AgentForm form, Action* out);

// ===========================================================================
// 2. 教学辅助分区（七导航目标）
//
// T09 只提供「可见 + 可展开 + 定位 + 诚实占位/不可用原因」；内容由 T15–T19 填充。
// Guide 是学习路径面板（T10 填内容），由 rGuide 控制显示。
// ===========================================================================

enum class Section : int
{
    Guide = 0,
    Issue,
    Evidence,
    Plan,
    Course,
    Record,
    Checker,
    Count
};

const std::vector<Section>& SectionCatalog();
const char* SectionId(Section section);     ///< "guide"/"issue"/…（稳定）
const char* SectionTitle(Section section);  ///< "学习路径"/"当前问题"/…（界面标题）

/// Guide→ToggleGuide，其余→对应 Jump*。越界返回 Action::Count。
Action SectionAction(Section section);
bool SectionForAction(Action action, Section* out);

/// 分区状态：可用性/摘要/正文全部来自模型或显式未知；不得伪造数据。
struct SectionProjection
{
    bool available = false;   ///< false → 只能展示原因，不得展示业务结论
    wxString summary;         ///< 折叠标题右侧摘要（原型 .val，如「尚未接通」）
    wxString body;            ///< 展开后的正文（诚实占位或不可用原因）
    wxString reason;          ///< 不可用原因（tooltip / 正文补充）；available 时可为空
};

SectionProjection ProjectSection(const vm::AgentViewModel& model, Section section);

// ===========================================================================
// 3. 顶栏/状态条投影
//
// 原型 H388 的「规则模式 · model_used=false」与 H643 的「policy edu-default-v1」
// 都是固定演示串。这里全部改成模型投影：v1 run DTO 没有 policy 字段（只有
// ui-receipt 合同有），因此 policy 恒为显式未知，直到 T22/T24 提供可信来源。
// ===========================================================================

struct StatusProjection
{
    wxString statusText;   ///< 后端状态（未知显式「状态未知」）
    wxString ruleMode;     ///< 「规则模式」/「模型模式」/「模式未知」
    wxString modelUsed;    ///< 「model_used=true/false/未知」
    wxString policy;       ///< 「policy 未知」或真实 policy
    wxString session;      ///< 「session 未绑定」或 session 短标识
    bool demo = false;     ///< 模型 demo 标记（唯一来源：AgentViewModel::DemoMode()）
    bool busy = false;     ///< 前端请求中（StatusView::busy，与后端状态分开）
};

StatusProjection ProjectStatus(const vm::AgentViewModel& model);

/// 「状态未知 · 规则模式 · model_used 未知 │ policy 未知 │ session 未绑定」
wxString ComposeStatusLine(const StatusProjection& status);

// ===========================================================================
// 4. 提示级别的**非猜测**映射
//
// v1 合同是字符串 L1–L3，U2 是内部整数且语义未经确认（DEC-06）。因此只有
// V1("L1".."L3") 映射为 1..3；L4 永不可选；U2/未知一律 0（未知），**不做 +1**。
// ===========================================================================

int LevelNumber(const vm::Level& level);      ///< 1..3；其它一律 0
vm::Level LevelForNumber(int number);         ///< 1..3 → V1("Ln")；其它 → Unbound

// ===========================================================================
// 5. 顶栏（Agent 页内品牌/装饰灯/学习-工作分段/L1–L4/演示标记/状态胶囊）
//
// 布局：单行放不下时按视觉顺序换行（与 H89 flex-wrap 同序）：灯 → 品牌 → 模式 →
// 分隔线 → 提示级别 → 演示标记 → 状态胶囊。右对齐只在单行时生效。
// ≤560（NarrowBreakpoint）：隐藏品牌副标、演示标记改紧凑文案「演示」（**不隐藏**，
// C03/DEC-10：窄屏也必须可辨认）。
// ===========================================================================

class AgentTopBar final : public wxPanel
{
public:
    explicit AgentTopBar(wxWindow* parent);
    ~AgentTopBar() override;

    /// 用户意图回调（模型写入由工作区执行；本控件不持有模型）。
    void SetIntentHandlers(std::function<void(vm::AppMode)> modeHandler,
                           std::function<void(int)> levelHandler);

    // ---- 程序化投影（不发业务事件）----
    void SetMode(vm::AppMode mode);
    vm::AppMode Mode() const { return m_mode; }
    void SetLevel(int level);        ///< 1..3 选中该项；其它值只清除选中（未知显式，绝不点亮 L4）
    int Level() const { return m_level; }
    void SetDemo(bool demo);
    bool Demo() const { return m_demo; }
    void SetStatus(const StatusProjection& status);
    const StatusProjection& Status() const { return m_status; }

    // ---- 布局：按可用宽度重排并 SetSize(0,0,width,height)，返回高度 ----
    int Arrange(int width);
    int PreferredHeightPx() const { return m_height; }
    int MinHeightPx() const;

    // ---- 诊断（测试/夹具用；不是样式 API）----
    wxWindow* ModeWindow() const;     ///< AgentSegmented*
    wxWindow* LevelWindow(int level); ///< 1..4；越界 nullptr
    wxWindow* DemoWindow() const;     ///< AgentPill*（demo=false 时隐藏）
    wxString BrandText() const;       ///< 完整品牌字符串（含副标）
    wxString DemoText() const;        ///< 当前演示标记文案（宽/窄）
    wxString StatusText() const;      ///< 状态胶囊完整文案
    wxRect LightsRectPx() const;      ///< 装饰灯区域（内部**无**子窗口）
    wxRect StatusRectPx() const;      ///< 状态胶囊矩形
    wxRect BrandRectPx() const;       ///< 品牌文字矩形
    bool IsNarrow() const { return m_narrow; }

private:
    void OnPaint(wxPaintEvent& event);
    void ApplyTheme();
    void MeasureBrand(int* primaryWidth, int* secondaryWidth) const;

    std::function<void(vm::AppMode)> m_modeHandler;
    std::function<void(int)> m_levelHandler;
    vm::AppMode m_mode = vm::AppMode::Learning;
    int m_level = 2;
    bool m_demo = false;
    bool m_narrow = false;
    int m_height = 0;
    StatusProjection m_status;

    controls::AgentSegmented* m_modeSegment = nullptr;
    controls::AgentButton* m_levels[4] = {};
    controls::AgentPill* m_demoPill = nullptr;
    wxRect m_lightsRect;
    wxRect m_brandRect;
    wxRect m_statusRect;
    wxRect m_dividerRect;
    UiTheme::Subscription m_theme;
};

// ===========================================================================
// 6. 功能 rail（58 DIP 竖排，≤960 横排换行；图标按钮 .rb）
//
// 顺序（H399–436）：logo、newAsk、分隔、七导航（rGuide…rChk）、弹性空白、
// 四形态。竖排时四形态贴底；横排时按顺序换行，**不隐藏任何按钮**。
// 按钮尺寸 38（≤560 用 34）；图标 18（stroke 1.9 由 AgentIcons 绘制）。
// ===========================================================================

class AgentRail final : public wxPanel
{
public:
    explicit AgentRail(wxWindow* parent);
    ~AgentRail() override;

    void SetActionHandler(std::function<void(Action)> handler);

    /// 程序化 pressed（只对 Toggle / Form 有意义，其余参数被忽略）。
    void SetPressed(Action action, bool pressed);
    bool IsPressed(Action action) const;
    void SetFormPressed(AgentForm form);

    void SetCompact(bool compact);
    bool IsCompact() const { return m_compact; }

    /// 竖排：忽略 width（用 AgentMetrics::RailWidth），返回自然高度。
    /// 横排：在 width 内换行排布，返回实际高度。
    int Arrange(int width);
    int PreferredWidthPx() const;   ///< 竖排 58；横排 0
    int PreferredHeightPx() const { return m_height; }

    // ---- 诊断 ----
    wxWindow* ButtonWindow(Action action) const;
    wxWindow* LogoWindow() const;   ///< 品牌缩略（装饰性，不可聚焦）
    bool IsButtonEnabled(Action action) const;
    wxString DisabledReason(Action action) const;
    void SetButtonEnabled(Action action, bool enabled, const wxString& reason);

private:
    void ApplyTheme();
    bool PressedState(Action action) const;
    int ButtonSizePx() const;

    struct Entry;
    std::function<void(Action)> m_handler;
    std::vector<Entry*> m_entries;
    wxWindow* m_logo = nullptr;
    bool m_compact = false;
    bool m_guidePressed = false;
    AgentForm m_form = AgentForm::None;
    int m_height = 0;
    UiTheme::Subscription m_theme;
};

// ===========================================================================
// 7. aw-head 四形态入口（.aw-tools .wt 30×28，与 rail 语义并联）
// ===========================================================================

class AgentFormSwitch final : public wxPanel
{
public:
    explicit AgentFormSwitch(wxWindow* parent);
    ~AgentFormSwitch() override;

    void SetActionHandler(std::function<void(Action)> handler);
    // 命名避开 winspool.h 的 `#define SetForm SetFormW`（UNICODE 下会改掉成员名）。
    void SetFormSelection(AgentForm form);
    AgentForm CurrentForm() const { return m_form; }

    int PreferredWidthPx() const;
    int PreferredHeightPx() const;
    int Arrange(int x, int y);   ///< 在父窗口坐标下定位，返回宽度

    wxWindow* ButtonWindow(Action action) const;

private:
    void ApplyTheme();

    struct Entry;
    std::function<void(Action)> m_handler;
    std::vector<Entry*> m_entries;
    AgentForm m_form = AgentForm::None;
    UiTheme::Subscription m_theme;
};

// ===========================================================================
// 8. 对话头（.aw-head）：芯语 + 回答模式标签（未知显式）+ 四形态按钮
//
// 回答模式标签来自真实结果（T12/T22 投影）；T09 只有「未知」一种诚实值，
// 不得写死「深答 · 思考链」。
// ===========================================================================

class AgentConversationHeader final : public wxPanel
{
public:
    explicit AgentConversationHeader(wxWindow* parent);
    ~AgentConversationHeader() override;

    void SetActionHandler(std::function<void(Action)> handler);
    // 同 AgentFormSwitch：避开 winspool.h 的 SetForm 宏。
    void SetFormSelection(AgentForm form);
    AgentForm CurrentForm() const;
    void SetModeTag(const wxString& tag);   ///< 空 → 「回答模式未知」
    wxString ModeTag() const;
    AgentFormSwitch* Forms() const { return m_forms; }

    /// 在父窗口坐标 (x,y) 起按 width 排布；返回高度。
    int Arrange(int x, int y, int width);
    int PreferredHeightPx() const { return m_height; }
    wxRect TitleRectPx() const { return m_titleRect; }
    wxRect TagRectPx() const { return m_tagRect; }
    wxString TitleText() const;
    bool IsTagShown() const { return m_tagShown; }

private:
    void OnPaint(wxPaintEvent& event);
    void ApplyTheme();

    AgentFormSwitch* m_forms = nullptr;
    wxString m_tag;
    int m_height = 0;
    bool m_tagShown = false;
    wxRect m_titleRect;
    wxRect m_tagRect;
};

// ===========================================================================
// 9. 对话状态条（.aw-status）：状态/规则模式/model_used/policy/session
//
// 全部分段来自 AgentNavigation::ProjectStatus(模型)；本控件只显示。窄宽度时
// 省略号裁剪，但 Text() 仍返回完整文案，ToolTip 也保留全文。
// ===========================================================================

class AgentStatusStrip final : public wxPanel
{
public:
    explicit AgentStatusStrip(wxWindow* parent);
    ~AgentStatusStrip() override;

    void SetSegments(const std::vector<wxString>& segments);
    std::vector<wxString> Segments() const { return m_segments; }
    wxString Text() const;                 ///< 完整「A │ B │ C」
    int Arrange(int width);                ///< 返回高度
    int PreferredHeightPx() const { return m_height; }

private:
    void OnPaint(wxPaintEvent& event);
    void ApplyTheme();

    std::vector<wxString> m_segments;
    int m_height = 0;
    UiTheme::Subscription m_theme;
};

}  // namespace agent::gui::nav
