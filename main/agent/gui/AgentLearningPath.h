#pragma once
//
// AgentLearningPath — AGUI-T10 六步学习路径与状态投影
//
// 设计源：GUI/agent-gui/index.html H115–133（.guide/.pane-h/.steps/.step/.guide-note）、
//         H687–708（STEPS 与点击 toast）、spec.md §4.3/§12.1、design-review.md CMP-005。
//
// 边界（本文件是六步标题与状态的唯一注册表，但不是第二套业务真源）：
//   * 六个标题是设计固定文本，只能改措辞不能增删；
//   * 状态**只**来自 ProjectPath()：demo 仅当 AgentViewModel::DemoMode() 为真，
//     否则六步一律 Unknown（当前工程没有可信的路径聚合来源，等 T24/受控合同）；
//   * 禁止从工具安装、打开项目、计划步骤、RunStatus、checker 或课程掌握反推六步状态；
//   * 点击只做本地导航提示与选中，不调用 EDA / 课程 / 后端，不修改投影与模型；
//   * 选中项是导航态，与后端教学状态（active 等）是两件事，视觉与 API 都分开；
//   * 所有颜色/字号/间距只取 AgentTokens / UiTheme 的 Agent 命名空间（DEC-13）；
//   * 用户可见文案不出现开发任务编号。
//
#include "agent/gui/AgentViewModel.h"
#include "ui/UiTheme.h"

#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/string.h>

#include <functional>
#include <vector>

namespace agent::gui::path {

/// 六步稳定 ID（顺序即设计顺序；只能追加，不得重排）。
enum class StepId : int
{
    Schematic = 0,  ///< 原理图设计
    CodeSync,       ///< 图码同步
    Simulation,     ///< 仿真验证
    Synthesis,      ///< 综合与资源
    Debug,          ///< 教学调试
    Summary,        ///< 练习小结
    Count
};

/// 步骤状态。Unknown 是 live 的诚实默认值，绝不等于 Todo。
enum class Status : int
{
    Unknown = 0,
    Todo,
    Active,
    Done,
    Recheck
};

struct StepDef
{
    StepId id;
    const char* key;    ///< 稳定英文键（日志/测试用，不是界面文案）
    const char* title;  ///< 设计固定标题（中文）
};

/// 六项，`Registry().size() == static_cast<int>(StepId::Count)`。
const std::vector<StepDef>& Registry();
const StepDef* Find(StepId id);

/// "unknown"/"todo"/"active"/"done"/"recheck"（日志用，大小写敏感）。
const char* StatusToken(Status status);
/// 未知 / 待办 / 进行中 / 已完成 / 需复测（界面文案）。
wxString StatusLabel(Status status);

struct StepView
{
    StepId id = StepId::Schematic;
    Status status = Status::Unknown;
    wxString title;   ///< 来自 Registry()
    wxString label;   ///< StatusLabel()
    wxString detail;  ///< 来源/未知原因（tooltip 与可访问帮助文本）
    bool operator==(const StepView&) const = default;
};

struct Projection
{
    std::vector<StepView> steps;  ///< 恒 6 项，Registry() 顺序
    bool demo = false;            ///< 唯一来源：AgentViewModel::DemoMode()
    wxString summary;             ///< 「进展未接入」/「演示数据 · 非真实学习进展」
    wxString reason;              ///< 未知/演示的整体说明
    bool operator==(const Projection&) const = default;
};

/// 唯一投影：demo → 原型固定六态；否则六步全 Unknown。
Projection ProjectPath(const vm::AgentViewModel& model);

/// 组件测试/gallery 专用显式演示数据（等价于 demo 模型下的投影）。
/// 不得被正式工作区隐式调用：只有 DemoMode() 才是入口。
Projection DemoPath();

/// 原生六步列表（.steps）：6 个真实 wxControl 步骤按钮 + 顶部状态行 + 底部导航提示。
/// 组件不持有模型、不发请求、不读写配置；投影由宿主用 ProjectPath() 注入。
class AgentLearningPath final : public wxPanel
{
public:
    explicit AgentLearningPath(wxWindow* parent);
    ~AgentLearningPath() override;

    /// 程序化投影；返回 true 表示内容有变化（否则不重绘）。
    bool SetProjection(const Projection& projection);
    const Projection& ProjectionData() const { return m_projection; }

    /// 点击步骤后的导航回调（可选）。回调里不得启动工具/请求。
    void SetNavigationHandler(std::function<void(StepId)> handler);
    /// 程序化选中（导航态，不改投影）；notify=true 时调用导航回调。
    void Select(StepId id, bool notify = false);
    StepId Selected() const { return m_selected; }
    /// 最近一次本地导航提示（toast 尚未实现时的显式替代）。
    wxString NavigationNote() const { return m_note; }

    // ---- 诊断（测试/夹具用；不是样式 API）----
    wxWindow* ButtonWindow(StepId id) const;
    wxString AccessibleName(StepId id) const;
    wxString DetailOf(StepId id) const;
    int Arrange(int width);  ///< SetSize(0,0,width,height) 并摆放步骤；返回高度
    int PreferredHeightPx() const { return m_height; }
    int StepHeightPx() const { return m_stepHeight; }
    wxRect SummaryRectPx() const { return m_summaryRect; }
    wxRect NoteRectPx() const { return m_noteRect; }

private:
    void ApplyTheme();
    void OnPaint(wxPaintEvent& event);

    struct Entry;  // 定义在 .cpp：单个步骤的原生按钮
    std::vector<Entry*> m_entries;
    Projection m_projection;
    std::function<void(StepId)> m_handler;
    StepId m_selected = StepId::Count;
    wxString m_note;
    int m_stepHeight = 0;
    int m_height = 0;
    wxRect m_summaryRect;
    wxRect m_noteRect;
    UiTheme::Subscription m_theme;
};

}  // namespace agent::gui::path
