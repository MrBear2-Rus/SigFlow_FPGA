//
// AgentLearningPath.cpp — 六步学习路径：注册表、状态投影与原生步骤列表（实现）
//
// 契约：main/agent/gui/AgentLearningPath.h（本文件只实现该头文件的声明，不改头文件）。
// 设计源：GUI/agent-gui/index.html H110–H133（.guide / .pane-h / .steps / .step / .dot /
//         .guide-note）、H687–H708（STEPS 与点击 toast）、spec.md §4.3 / §12.1、
//         main/agent/gui/AgentNavigation.cpp（自绘控件与焦点环口径）、
//         main/agent/gui/AgentControls.cpp（AgentButton / AgentPill 的绘制与输入口径）。
//
// 边界（与头文件注释一致）：
//   * 六步标题是设计固定文本（本文件是唯一注册表），只能改措辞、不能增删；
//   * 状态**只**来自 ProjectPath()：demo 仅当 AgentViewModel::DemoMode() 为真，否则六步 All
//     Unknown。**不**从 RunStatus / Messages / Capabilities / 卡片 / 计划步骤 / 检查器反推；
//   * 本文件不持有模型、不发请求、不读写文件与配置、不起定时器、不执行命令、不打开 URL；
//   * 点击步骤只做本地导航提示（m_note）+ 选中，不修改投影与模型；
//   * 颜色/字号/间距/圆角只取 AgentTokens / UiTheme 的 Agent 命名空间：本文件没有 hex、
//     没有 wxColour(r,g,b)、也没有第二套调色板；
//   * 用户可见文案不含开发任务编号（任务编号只出现在这类注释里）。
//
// ⚠️ 未验项：真机鼠标/键盘轨迹、IME、125%/150% DPI 逐像素对照、深色档视觉对照、
//    读屏实测（SetName/SetToolTip/wxAccessible 只保证"设置且可读回"）、原型截图比对。
//

#include "agent/gui/AgentLearningPath.h"

#include "agent/gui/AgentIcons.h"
#include "agent/gui/AgentTokens.h"
#include "ui/UiFocusRing.h"
#include "ui/UiFocusTrace.h"
#include "ui/UiTheme.h"

#include <wx/access.h>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/pen.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace agent::gui::path {

namespace {

// ---------------------------------------------------------------------------
// Token 短名（仍是单一来源，只是让布局代码可读；不引入任何本地色值）
// ---------------------------------------------------------------------------
using tokens::BorderRole;
using tokens::Component;

namespace M = UiTheme::AgentMetrics;
namespace S = UiTheme::AgentSpacing;
namespace T = UiTheme::AgentTypography;
namespace I = UiTheme::AgentIcon;

const UiTheme::AgentColours& A()
{
    return tokens::Colours();
}

int Dip(const wxWindow* window, int dip)
{
    return tokens::CssPxToDip(window, dip);
}

wxString U8(const char* text)
{
    return wxString::FromUTF8(text);
}

// ---------------------------------------------------------------------------
// 设计里有、UiTheme::Agent* 里尚无 token 的四个局部值（其余全部复用既有 token）。
// 每项都给出原型来源；不在布局代码里散布裸数字。
// ---------------------------------------------------------------------------
constexpr int kTitleStatusGapDip = 2;    ///< H131 .step .st{margin-top:2px}
constexpr int kDotMarginTopDip = 1;      ///< H122 .step .dot{margin-top:1px}
constexpr int kSelectedBarWidthDip = 3;  ///< 导航选中条（本任务新增的导航态标记，非原型样式）
constexpr int kSelectedBarInsetDip = 4;  ///< 选中条上下内缩

// ---------------------------------------------------------------------------
// 文案（用户可见；集中在此便于逐条对账，不含任何任务编号）
// ---------------------------------------------------------------------------
const char* const kLiveSummary = "进展未接入";
const char* const kLiveReason = "当前没有可信的学习路径来源，六步状态显示为未知。";
const char* const kLiveDetail = "学习进展尚未接入；不根据工具安装、项目打开或运行状态推断。";
const char* const kDemoSummary = "演示数据 · 非真实学习进展";
const char* const kDemoReason = "演示状态仅用于界面检查，不代表真实学习进展。";
const char* const kDemoDetail = "演示数据，不代表真实学习进展。";
const char* const kNoteInitial = "点击步骤只做定位，不重跑工具。";
const char* const kNoteLocatePrefix = "定位到：";
const char* const kNoteLocateSuffix = "（只导航，不重跑工具）";

// ---------------------------------------------------------------------------
// 圆角 / 环形图元（与 AgentControls / AgentNavigation 同一口径：
// 描边路径内缩半个线宽，避免被客户区裁掉一半）
// ---------------------------------------------------------------------------

double ClampRadius(double radius, const wxRect& rect)
{
    const double half = std::min(rect.width, rect.height) / 2.0;
    return std::max(0.0, std::min(radius, half));
}

void FillRound(wxGraphicsContext& gc, const wxRect& rect, double radius, const wxColour& colour)
{
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }
    wxGraphicsPath path = gc.CreatePath();
    path.AddRoundedRectangle(rect.x, rect.y, rect.width, rect.height, ClampRadius(radius, rect));
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(gc.CreateBrush(wxBrush(colour)));
    gc.FillPath(path);
}

void StrokeRound(wxGraphicsContext& gc, const wxRect& rect, double radius, const wxColour& colour,
                 int widthPx)
{
    const int width = std::max(1, widthPx);
    if (rect.width <= width || rect.height <= width) {
        return;
    }
    const double half = width / 2.0;
    wxGraphicsPath path = gc.CreatePath();
    path.AddRoundedRectangle(rect.x + half, rect.y + half, rect.width - width, rect.height - width,
                             std::max(0.0, ClampRadius(radius, rect) - half));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(width)));
    gc.StrokePath(path);
}

void StrokeBand(wxGraphicsContext& gc, const wxRect& rect, double radius, int width,
                const wxColour& colour)
{
    if (width <= 0 || rect.width <= width || rect.height <= width) {
        return;
    }
    const double half = width / 2.0;
    wxGraphicsPath path = gc.CreatePath();
    path.AddRoundedRectangle(rect.x + half, rect.y + half, rect.width - width, rect.height - width,
                             std::max(0.0, ClampRadius(radius, rect) - half));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(width)));
    gc.StrokePath(path);
}

/// 原型 H85 `:focus-visible{outline:2px;offset:2px}` + H257 `0 0 0 3px accent-soft`。
/// 只画在控件内部：wx 子窗口无法把描边画到自身客户区之外，offset 表现为内缩。
void DrawFocusRing(wxGraphicsContext& gc, const wxWindow* window, const wxRect& area,
                   double radius, const wxColour& ringColour)
{
    const int ring = std::max(1, Dip(window, M::FocusRing));
    const int glow = Dip(window, M::FocusGlow);
    const int offset = Dip(window, M::FocusRingOffset);

    wxRect band = area;
    band.Deflate(offset, offset);
    if (band.width <= 0 || band.height <= 0) {
        return;
    }
    const double bandRadius = ClampRadius(radius - offset, band);

    // ① 3 DIP accent-soft 光晕
    StrokeBand(gc, band, bandRadius, glow, A().accentSoft);

    // ② 2 DIP accent 环
    wxRect inner = band;
    inner.Deflate(glow, glow);
    if (inner.width <= 0 || inner.height <= 0) {
        return;
    }
    StrokeBand(gc, inner, std::max(0.0, bandRadius - glow), ring, ringColour);
}

// ---------------------------------------------------------------------------
// 文本
// ---------------------------------------------------------------------------

double TextWidth(wxGraphicsContext& gc, const wxString& text)
{
    double width = 0.0;
    double height = 0.0;
    gc.GetTextExtent(text, &width, &height);
    return width;
}

double TextHeight(wxGraphicsContext& gc)
{
    double width = 0.0;
    double height = 0.0;
    gc.GetTextExtent(U8("Ag"), &width, &height);
    return height;
}

/// 与测量口径完全一致的像素宽度（ceil + 1，与 AgentControls::MeasuredText 同式）。
int MeasureWidthPx(wxGraphicsContext& gc, const wxString& text)
{
    return static_cast<int>(std::ceil(TextWidth(gc, text))) + 1;
}

/// 单行省略（U+2026）。测量与绘制必须用同一个测量器，否则自然尺寸下也会被裁。
wxString EllipsizeToWidth(wxGraphicsContext& gc, const wxString& text, double maxWidth)
{
    if (maxWidth <= 0.0) {
        return wxString();
    }
    if (TextWidth(gc, text) <= maxWidth) {
        return text;
    }
    const wxString ellipsis(wxUniChar(0x2026));
    const double ellipsisWidth = TextWidth(gc, ellipsis);
    if (ellipsisWidth > maxWidth) {
        return wxString();
    }
    wxString result;
    for (wxString::const_iterator it = text.begin(); it != text.end(); ++it) {
        wxString candidate = result;
        candidate += *it;
        if (TextWidth(gc, candidate + ellipsis) > maxWidth) {
            break;
        }
        result = candidate;
    }
    return result + ellipsis;
}

/// 贪心折行：优先在最近的空格处断（拉丁文），否则按字符断（中文）。
/// 测量函数由调用方注入 —— Arrange 与 OnPaint 必须用**同一套**度量，否则
/// 计算高度与绘制行数会不一致（多一行被裁 / 少一行留白）。
std::vector<wxString> WrapText(const std::function<int(const wxString&)>& measure,
                               const wxString& text, int maxWidth)
{
    std::vector<wxString> lines;
    if (maxWidth <= 0 || text.empty()) {
        return lines;
    }
    wxString current;
    for (wxString::const_iterator it = text.begin(); it != text.end(); ++it) {
        const wxUniChar ch = *it;
        if (ch == static_cast<wxUniChar>('\n')) {
            lines.push_back(current);
            current.clear();
            continue;
        }
        wxString candidate = current;
        candidate += ch;
        if (!current.empty() && measure(candidate) > maxWidth) {
            int spacePos = -1;
            for (int index = static_cast<int>(current.length()) - 1; index >= 0; --index) {
                if (current[static_cast<std::size_t>(index)] == static_cast<wxUniChar>(' ')) {
                    spacePos = index;
                    break;
                }
            }
            if (spacePos > 0) {
                lines.push_back(current.Left(static_cast<std::size_t>(spacePos)));
                wxString rest = current.Mid(static_cast<std::size_t>(spacePos) + 1);
                rest += ch;
                current = rest;
            } else {
                lines.push_back(current);
                current = ch;
            }
        } else {
            current = candidate;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

/// 与 OnPaint 同一条 GDI+ 文本管线的测量器（wxClientDC + wxGraphicsContext）。
class Measurer
{
public:
    explicit Measurer(const wxWindow* window)
        : m_dc(const_cast<wxWindow*>(window)), m_window(window)
    {
        m_gc.reset(wxGraphicsContext::Create(m_dc));
    }

    int Width(const wxFont& font, const wxString& text) const
    {
        if (m_gc) {
            m_gc->SetFont(font, A().text1);
            return MeasureWidthPx(*m_gc, text);
        }
        if (m_window == nullptr) {
            return 0;
        }
        return const_cast<wxWindow*>(m_window)->GetTextExtent(text).x;
    }

private:
    wxClientDC m_dc;
    const wxWindow* m_window = nullptr;
    std::unique_ptr<wxGraphicsContext> m_gc;
};

// ---------------------------------------------------------------------------
// 步骤几何：一个地方算出 6 个步骤共用的内距/行高/总高，
// 步骤控件绘制与面板 Arrange 都调它，保证"高度算出来的 == 画出来的"。
// ---------------------------------------------------------------------------
struct StepMetrics
{
    int padX = 0;       ///< S::StepPadX（9）
    int padY = 0;       ///< S::StepPadY（8）
    int innerGap = 0;   ///< S::StepInnerGap（9，圆点与文字块间距）
    int dot = 0;        ///< I::StepDot（15）
    int titleLine = 0;  ///< 12 px 行框
    int statusLine = 0; ///< 10 px 行框
    int titleGap = 0;   ///< kTitleStatusGapDip（2）
    int radius = 0;     ///< M::ControlRadius（6）
    int height = 0;     ///< 2*padY + titleLine + titleGap + statusLine
};

StepMetrics ComputeStepMetrics(const wxWindow* window)
{
    StepMetrics metrics;
    metrics.padX = Dip(window, S::StepPadX);
    metrics.padY = Dip(window, S::StepPadY);
    metrics.innerGap = Dip(window, S::StepInnerGap);
    metrics.dot = Dip(window, I::StepDot);
    metrics.titleLine = tokens::LineHeightDip(window, T::LabelPx, T::LineHeightText);
    metrics.statusLine = tokens::LineHeightDip(window, T::MicroPx, T::LineHeightText);
    metrics.titleGap = Dip(window, kTitleStatusGapDip);
    metrics.radius = Dip(window, M::ControlRadius);
    metrics.height =
        2 * metrics.padY + metrics.titleLine + metrics.titleGap + metrics.statusLine;
    return metrics;
}

wxRect ClampRect(const wxRect& rect, int boundWidth, int boundHeight)
{
    const int x = std::max(0, std::min(rect.x, boundWidth));
    const int y = std::max(0, std::min(rect.y, boundHeight));
    return wxRect(x, y, std::max(0, std::min(rect.width, boundWidth - x)),
                  std::max(0, std::min(rect.height, boundHeight - y)));
}

// ---------------------------------------------------------------------------
// 状态圆点（H122–H129）
//
// Done    : --green 实心 + 白色勾（icons::Draw CheckMark，8 DIP）
// Active  : 2 DIP accent 环，不填充
// Recheck : 2 DIP warn 环，不填充
// Todo    : 2 DIP hairline 环
// Unknown : 2 DIP text3 环 + SHORT_DASH —— **原型没有 unknown 状态**（STEPS 恒有六态之一），
//           这是本次"诚实未知"的适配，不是重新设计：用虚线让"未知"与"待办"一眼可分，
//           同时不引入任何新色值（仍是 text3 环）。
// ---------------------------------------------------------------------------

void StrokeRing(wxGraphicsContext& gc, double centreX, double centreY, double radius, int width,
                const wxColour& colour, bool dashed)
{
    if (radius <= 0.0 || width <= 0) {
        return;
    }
    wxGraphicsPath path = gc.CreatePath();
    path.AddCircle(centreX, centreY, radius);
    wxGraphicsPenInfo info(colour);
    info.Width(width);
    if (dashed) {
        info.Style(wxPENSTYLE_SHORT_DASH);
    }
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(info));
    gc.StrokePath(path);
}

void DrawStepDot(wxGraphicsContext& gc, const wxWindow* window, const wxRect& box, Status status)
{
    if (box.width <= 0 || box.height <= 0) {
        return;
    }
    if (status == Status::Done) {
        // 实心 brand 圆 + 白色勾（H124/H125：.done .dot{background:green} + svg opacity 1）
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(gc.CreateBrush(wxBrush(A().brand)));
        gc.DrawEllipse(box.x, box.y, box.width, box.height);

        const int tick = std::max(0, std::min(Dip(window, I::StepTick),
                                             std::min(box.width, box.height)));
        if (tick > 0) {
            const wxRect tickBox(box.x + (box.width - tick) / 2, box.y + (box.height - tick) / 2,
                                 tick, tick);
            icons::Draw(gc, icons::IconId::CheckMark, tickBox, A().textOnAccent);
        }
        return;
    }

    const int ringWidth = std::max(1, Dip(window, I::StepDotRing));  // H122 2px 环
    const double centreX = box.x + box.width / 2.0;
    const double centreY = box.y + box.height / 2.0;
    const double radius = std::max(0.0, (std::min(box.width, box.height) - ringWidth) / 2.0);

    switch (status) {
        case Status::Active:
            StrokeRing(gc, centreX, centreY, radius, ringWidth, A().accent, false);
            break;
        case Status::Recheck:
            StrokeRing(gc, centreX, centreY, radius, ringWidth, A().warn, false);
            break;
        case Status::Todo:
            StrokeRing(gc, centreX, centreY, radius, ringWidth,
                       tokens::BorderColour(BorderRole::Hairline), false);
            break;
        case Status::Unknown:
            StrokeRing(gc, centreX, centreY, radius, ringWidth, A().text3, true);
            break;
        case Status::Done:
            break;  // 上面已 return
    }
}

// ---------------------------------------------------------------------------
// 可访问性基底（只保证"设置且可通过 API 读回"，未做读屏实测）
// ---------------------------------------------------------------------------
#if wxUSE_ACCESSIBILITY
struct AccessInfo
{
    wxString name;
    wxString help;
    bool focusable = false;
    bool focused = false;
    bool unavailable = false;
    bool selected = false;
};

class StepAccessible final : public wxAccessible
{
public:
    using Provider = std::function<AccessInfo()>;

    StepAccessible(wxWindow* window, Provider provider)
        : wxAccessible(window), m_provider(std::move(provider))
    {
    }

    wxAccStatus GetName(int childId, wxString* name) override
    {
        if (childId != wxACC_SELF || name == nullptr) {
            return wxACC_NOT_IMPLEMENTED;
        }
        *name = m_provider().name;
        return wxACC_OK;
    }

    wxAccStatus GetRole(int childId, wxAccRole* role) override
    {
        if (childId != wxACC_SELF || role == nullptr) {
            return wxACC_NOT_IMPLEMENTED;
        }
        *role = wxROLE_SYSTEM_PUSHBUTTON;
        return wxACC_OK;
    }

    wxAccStatus GetState(int childId, long* state) override
    {
        if (childId != wxACC_SELF || state == nullptr) {
            return wxACC_NOT_IMPLEMENTED;
        }
        const AccessInfo info = m_provider();
        *state = 0;
        if (info.focusable) {
            *state |= wxACC_STATE_SYSTEM_FOCUSABLE;
        }
        if (info.focused) {
            *state |= wxACC_STATE_SYSTEM_FOCUSED;
        }
        if (info.unavailable) {
            *state |= wxACC_STATE_SYSTEM_UNAVAILABLE;
        }
        if (info.selected) {
            *state |= wxACC_STATE_SYSTEM_SELECTED;
        }
        return wxACC_OK;
    }

    wxAccStatus GetHelpText(int childId, wxString* text) override
    {
        if (childId != wxACC_SELF || text == nullptr) {
            return wxACC_NOT_IMPLEMENTED;
        }
        *text = m_provider().help;
        return wxACC_OK;
    }

private:
    Provider m_provider;
};
#endif  // wxUSE_ACCESSIBILITY

// ---------------------------------------------------------------------------
// StepButton — 单个步骤的自绘原生控件（.step）
//
// 状态机（与 AgentNavigation::IconButton / AgentControls::AgentButton 同源）：
//   教学状态（status，来自投影）  ≠  导航选中（selected）  ≠  鼠标按住（pressed）
// 三者分开存：选中只加左侧导航条与 sunken 底，绝不改圆点/边框/状态文案。
// 激活：左键按下捕获 → 左键抬起且仍在内部 → 激活**一次**；捕获丢失复位；
//       Space/Enter 的 KEY_DOWN 激活一次（按住自动重复不再激活），KEY_UP 复位按压态。
// ---------------------------------------------------------------------------
class StepButton final : public wxControl
{
public:
    using Activator = std::function<void(StepId)>;

    StepButton(wxWindow* parent, const StepDef& def, Activator activate)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          m_activate(std::move(activate))
    {
        m_view.id = def.id;
        m_view.status = Status::Unknown;
        m_view.title = U8(def.title);  // 标题恒取注册表（设计固定文本）
        m_view.label = StatusLabel(m_view.status);

        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(0, 0));
        ApplyAccessibility();

        Bind(wxEVT_PAINT, &StepButton::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &StepButton::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &StepButton::OnLeftUp, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &StepButton::OnCaptureLost, this);
        Bind(wxEVT_ENTER_WINDOW, &StepButton::OnEnter, this);
        Bind(wxEVT_LEAVE_WINDOW, &StepButton::OnLeave, this);
        Bind(wxEVT_KEY_DOWN, &StepButton::OnKeyDown, this);
        Bind(wxEVT_KEY_UP, &StepButton::OnKeyUp, this);
        Bind(wxEVT_SET_FOCUS, &StepButton::OnFocus, this);
        Bind(wxEVT_KILL_FOCUS, &StepButton::OnKillFocus, this);
    }

    ~StepButton() override
    {
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    StepId Id() const { return m_view.id; }
    Status CurrentStatus() const { return m_view.status; }
    wxString Detail() const { return m_view.detail; }

    /// 投影 → 控件（标题不在这里改：它只来自注册表）。
    void SetStepState(Status status, const wxString& label, const wxString& detail)
    {
        if (m_view.status == status && m_view.label == label && m_view.detail == detail) {
            return;
        }
        m_view.status = status;
        m_view.label = label;
        m_view.detail = detail;
        ApplyAccessibility();
        Refresh(false);
    }

    /// 导航选中（不是教学状态）：只影响左侧导航条与"原本透明"的底色。
    void SetSelectedVisual(bool selected)
    {
        if (m_selected == selected) {
            return;
        }
        m_selected = selected;
        Refresh(false);
    }

    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return AcceptsFocus(); }

protected:
    wxSize DoGetBestSize() const override
    {
        const StepMetrics metrics = ComputeStepMetrics(this);
        Measurer measure(this);
        const wxFont titleFont = tokens::UiFontPx(this, T::LabelPx, tokens::Weight::Medium);
        const wxFont statusFont = tokens::UiFontPx(this, T::MicroPx, tokens::Weight::Semibold);
        const int textWidth =
            std::max(measure.Width(titleFont, m_view.title), measure.Width(statusFont, m_view.label));
        const int width = 2 * metrics.padX + metrics.dot + metrics.innerGap + textWidth;
        return wxSize(std::max(0, width), std::max(0, metrics.height));
    }

private:
    wxString AccessibleNameText() const
    {
        // title + " · " + label（例：「仿真验证 · 进行中」）
        return m_view.title + U8(" · ") + m_view.label;
    }

    wxString TooltipText() const
    {
        // title \n label \n detail
        return m_view.title + U8("\n") + m_view.label + U8("\n") + m_view.detail;
    }

    void ApplyAccessibility()
    {
        SetName(AccessibleNameText());
        SetToolTip(TooltipText());
#if wxUSE_ACCESSIBILITY
        // 只在首次挂载：provider 每次查询都现取状态，不需要重建 wxAccessible。
        if (GetAccessible() == nullptr) {
            SetAccessible(new StepAccessible(this, [this] {
                AccessInfo info;
                info.name = AccessibleNameText();
                info.help = m_view.detail;
                info.focusable = true;
                info.focused = HasFocus();
                info.unavailable = !IsEnabled();
                info.selected = m_selected;
                return info;
            }));
        }
#endif
    }

    bool IsActing() const { return m_mousePressed || m_keyPressed; }

    wxColour TitleColour() const
    {
        // H130/H132：标题在 done/active/recheck 用 text1，todo（与新增 unknown）用 text2。
        switch (m_view.status) {
            case Status::Done:
            case Status::Active:
            case Status::Recheck:
                return A().text1;
            case Status::Todo:
            case Status::Unknown:
                break;
        }
        return A().text2;
    }

    wxColour StatusColour() const
    {
        // H131/H132：.st 默认 text3，done 绿字、active 深蓝字、recheck 琥珀字。
        switch (m_view.status) {
            case Status::Done:
                return A().okText;
            case Status::Active:
                return A().accentText;
            case Status::Recheck:
                return A().warnText;
            case Status::Todo:
            case Status::Unknown:
                break;
        }
        return A().text3;
    }

    void ResetPress()
    {
        m_mousePressed = false;
        m_keyPressed = false;
        m_spaceDown = false;
        m_enterDown = false;
    }

    void Activate()
    {
        if (!IsEnabled()) {
            return;
        }
        // 回调（面板 → 导航处理器）可能销毁本控件：先拷贝再调用，调用后不再触碰成员。
        auto activate = m_activate;
        if (activate) {
            activate(m_view.id);
        }
    }

    void OnPaint(wxPaintEvent&)
    {
        const wxRect rect = GetClientRect();
        const wxColour surface = tokens::SurfaceColour(Component::Card);

        wxAutoBufferedPaintDC dc(this);
        // 圆角外的四角只能是卡片面：wx 子窗口没有透明背景（THE-021），
        // 用卡片面铺满再画状态面，永远不会出现黑角。
        dc.SetBackground(wxBrush(surface));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || rect.width <= 0 || rect.height <= 0) {
            return;
        }

        const StepMetrics metrics = ComputeStepMetrics(this);
        const double radius = ClampRadius(metrics.radius, rect);
        const Status status = m_view.status;
        const bool semantic = status == Status::Active || status == Status::Recheck;

        // ① 状态面 + 边框（H126/H128）：只有 active / recheck 有自底色与语义边线。
        //    按下态（pressed）与 hover 同面：原型没有 .step:active 样式，而 Active/Recheck
        //    的语义底不能因为按住而丢掉，因此这两档不额外加"按下"色（不引入新色值）。
        if (status == Status::Active) {
            FillRound(*gc, rect, radius, A().accentSoft);
            StrokeRound(*gc, rect, radius, tokens::BorderColour(BorderRole::Accent),
                        Dip(this, M::Hairline));
        } else if (status == Status::Recheck) {
            FillRound(*gc, rect, radius, A().warnSoft);
            StrokeRound(*gc, rect, radius, tokens::BorderColour(BorderRole::WarnMid),
                        Dip(this, M::Hairline));
        } else if (m_hover || IsActing()) {
            // H121 .step:hover{background:var(--sunken)}（done/todo/unknown 的常态是透明）
            FillRound(*gc, rect, radius, A().sunken);
        }

        // ② 导航选中：透明底时补 sunken；有语义底的（active/recheck）保持原底。
        //    注意顺序——选中只加这两笔，绝不改圆点、边框与文案颜色。
        if (m_selected && !semantic) {
            FillRound(*gc, rect, radius, A().sunken);
        }

        if (m_selected) {
            const int barWidth = Dip(this, kSelectedBarWidthDip);
            const int inset = Dip(this, kSelectedBarInsetDip);
            const wxRect bar(rect.x, rect.y + inset, barWidth,
                             std::max(0, rect.height - 2 * inset));
            FillRound(*gc, bar, barWidth / 2.0, A().accent);
        }

        // ③ 状态圆点
        const wxRect dotRect(rect.x + metrics.padX, rect.y + metrics.padY + Dip(this, kDotMarginTopDip),
                             metrics.dot, metrics.dot);
        DrawStepDot(*gc, this, dotRect, status);

        // ④ 文字块（圆点右侧 9 DIP；标题 12 px Medium，副状态 10 px Semibold，间隔 2 DIP）
        const int textX = rect.x + metrics.padX + metrics.dot + metrics.innerGap;
        const int textRight = rect.x + rect.width - metrics.padX;
        const int textWidth = std::max(0, textRight - textX);
        if (textWidth > 0) {
            const wxFont titleFont = tokens::UiFontPx(this, T::LabelPx, tokens::Weight::Medium);
            const wxFont statusFont = tokens::UiFontPx(this, T::MicroPx, tokens::Weight::Semibold);

            gc->SetFont(titleFont, TitleColour());
            const wxString title = EllipsizeToWidth(*gc, m_view.title, textWidth);
            const double titleGlyph = TextHeight(*gc);
            gc->DrawText(title, textX,
                         rect.y + metrics.padY + std::max(0.0, (metrics.titleLine - titleGlyph) / 2.0));

            gc->SetFont(statusFont, StatusColour());
            const wxString label = EllipsizeToWidth(*gc, m_view.label, textWidth);
            const double labelGlyph = TextHeight(*gc);
            gc->DrawText(label, textX,
                         rect.y + metrics.padY + metrics.titleLine + metrics.titleGap +
                             std::max(0.0, (metrics.statusLine - labelGlyph) / 2.0));
        }

        // ⑤ 键盘焦点环（仅键盘来源焦点：UiFocusRing::Tracker 在 SET_FOCUS 时闩存）
        if (HasFocus() && m_focusTracker.KeyboardFocused()) {
            DrawFocusRing(*gc, this, rect, radius, A().accent);
        }
    }

    void OnLeftDown(wxMouseEvent& event)
    {
        UiFocusTrace::NotePointer();
        if (!IsEnabled()) {
            event.Skip();
            return;
        }
        SetFocus();
        if (m_mousePressed) {
            event.Skip();
            return;
        }
        m_mousePressed = true;
        if (!HasCapture()) {
            CaptureMouse();
        }
        Refresh(false);
    }

    void OnLeftUp(wxMouseEvent& event)
    {
        UiFocusTrace::NotePointer();
        const bool wasPressed = m_mousePressed;
        m_mousePressed = false;
        const bool inside = GetClientRect().Contains(event.GetPosition());
        if (HasCapture()) {
            ReleaseMouse();
        }
        Refresh(false);
        if (wasPressed && inside && IsEnabled()) {
            Activate();  // 可能销毁本控件：此后不再触碰成员
        }
    }

    void OnCaptureLost(wxMouseCaptureLostEvent&)
    {
        // 捕获被系统/其它窗口拿走：按压态必须复位，否则留下"按住的假象"。
        ResetPress();
        Refresh(false);
    }

    void OnEnter(wxMouseEvent& event)
    {
        if (!m_hover) {
            m_hover = true;
            Refresh(false);
        }
        event.Skip();
    }

    void OnLeave(wxMouseEvent& event)
    {
        if (m_hover) {
            m_hover = false;
            Refresh(false);
        }
        event.Skip();
    }

    void OnKeyDown(wxKeyEvent& event)
    {
        UiFocusTrace::NoteKeyboard();
        const int code = event.GetKeyCode();
        const bool isSpace = code == WXK_SPACE;
        const bool isEnter = code == WXK_RETURN || code == WXK_NUMPAD_ENTER;
        if (!isSpace && !isEnter) {
            event.Skip();
            return;
        }
        if (!IsEnabled()) {
            return;
        }
        // 自动重复会连发 KEY_DOWN（中间没有 KEY_UP）：只有第一次激活。
        bool& down = isSpace ? m_spaceDown : m_enterDown;
        if (down) {
            return;
        }
        down = true;
        m_keyPressed = true;
        Refresh(false);
        Activate();  // 可能销毁本控件
    }

    void OnKeyUp(wxKeyEvent& event)
    {
        UiFocusTrace::NoteKeyboard();
        const int code = event.GetKeyCode();
        if (code == WXK_SPACE) {
            m_spaceDown = false;
        } else if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
            m_enterDown = false;
        } else {
            event.Skip();
            return;
        }
        if (!m_spaceDown && !m_enterDown) {
            m_keyPressed = false;
            Refresh(false);
        }
    }

    void OnFocus(wxFocusEvent& event)
    {
        m_focusTracker.OnFocusGained();
        Refresh(false);
        event.Skip();
    }

    void OnKillFocus(wxFocusEvent& event)
    {
        m_focusTracker.OnFocusLost();
        ResetPress();
        Refresh(false);
        event.Skip();
    }

    Activator m_activate;
    StepView m_view;
    bool m_selected = false;
    bool m_hover = false;
    bool m_mousePressed = false;
    bool m_keyPressed = false;
    bool m_spaceDown = false;
    bool m_enterDown = false;
    UiFocusRing::Tracker m_focusTracker;
};

// ---------------------------------------------------------------------------
// 投影辅助：只按 StepId 取值，避免"投影顺序"成为第二条隐式契约。
// ---------------------------------------------------------------------------
const StepView* FindView(const Projection& projection, StepId id)
{
    if (static_cast<int>(id) < 0) {
        return nullptr;
    }
    for (const StepView& view : projection.steps) {
        if (view.id == id) {
            return &view;
        }
    }
    return nullptr;
}

/// live 的诚实默认投影（六步 Unknown）。ProjectPath 的非 demo 分支与
/// 面板构造时的初始投影都用它，保证"没接模型"与"接了模型但没数据"完全一致。
Projection LivePath()
{
    Projection projection;
    projection.demo = false;
    projection.summary = U8(kLiveSummary);
    projection.reason = U8(kLiveReason);
    projection.steps.reserve(Registry().size());
    for (const StepDef& def : Registry()) {
        StepView view;
        view.id = def.id;
        view.status = Status::Unknown;
        view.title = U8(def.title);
        view.label = StatusLabel(view.status);
        view.detail = U8(kLiveDetail);
        projection.steps.push_back(view);
    }
    return projection;
}

}  // namespace

// ===========================================================================
// 1. 注册表与状态文案
// ===========================================================================

const std::vector<StepDef>& Registry()
{
    // 顺序 == StepId 顺序 == index.html H688–H695 的 STEPS 顺序。
    // key 是稳定英文键（日志/测试用），title 是设计固定中文标题。
    static const std::vector<StepDef> registry = {
        {StepId::Schematic, "schematic", "原理图设计"},
        {StepId::CodeSync, "code-sync", "图码同步"},
        {StepId::Simulation, "simulation", "仿真验证"},
        {StepId::Synthesis, "synthesis", "综合与资源"},
        {StepId::Debug, "debug", "教学调试"},
        {StepId::Summary, "summary", "练习小结"},
    };
    return registry;
}

const StepDef* Find(StepId id)
{
    // Count 与任何越界/负值都返回 nullptr（不夹紧到某一项，避免"点错也导航"）。
    const int index = static_cast<int>(id);
    if (index < 0) {
        return nullptr;
    }
    const std::vector<StepDef>& registry = Registry();
    if (static_cast<std::size_t>(index) >= registry.size()) {
        return nullptr;
    }
    return &registry[static_cast<std::size_t>(index)];
}

const char* StatusToken(Status status)
{
    switch (status) {
        case Status::Unknown:
            return "unknown";
        case Status::Todo:
            return "todo";
        case Status::Active:
            return "active";
        case Status::Done:
            return "done";
        case Status::Recheck:
            return "recheck";
    }
    return "unknown";  // 越界值一律按未知报（日志里不伪造状态）
}

wxString StatusLabel(Status status)
{
    switch (status) {
        case Status::Unknown:
            return U8("未知");
        case Status::Todo:
            return U8("待办");
        case Status::Active:
            return U8("进行中");
        case Status::Done:
            return U8("已完成");
        case Status::Recheck:
            return U8("需复测");
    }
    return U8("未知");
}

// ===========================================================================
// 2. 投影（六步状态的唯一入口）
// ===========================================================================

Projection ProjectPath(const vm::AgentViewModel& model)
{
    // 唯一隐式入口：AgentViewModel::DemoMode()。其余一切走 live 的诚实未知。
    //
    // 为什么不看别的字段：AgentViewModel 里**没有**能聚合出"六步教学进度"的可信来源。
    //   * Status() / RunStatus 描述的是一次运行的进行/等待/终态，不是"第几步已完成"；
    //   * Messages 是对话内容，Capabilities 是能力可用性（装没装工具、能不能调）；
    //   * cards / plan steps / checkers 是某一轮回答的产物与检查项，与设计固定的六步
    //     （原理图设计→…→练习小结）没有一一对应关系。
    // 从它们反推只能靠猜（"跑过一次仿真" ≠ "仿真验证已完成"），所以本函数只读
    // DemoMode()，其余状态一律不参与判定：真实路径聚合来源接入前，live 恒为六步 Unknown。
    if (!model.DemoMode()) {
        return LivePath();
    }
    return DemoPath();
}

Projection DemoPath()
{
    // 原型 H688–H695 / spec §12.1 的固定演示序列（唯一 demo 夹具，绝不用于 live）。
    static const Status kDemoStatus[] = {Status::Done,  Status::Done,   Status::Active,
                                         Status::Todo,  Status::Recheck, Status::Todo};
    constexpr std::size_t kDemoCount = sizeof(kDemoStatus) / sizeof(kDemoStatus[0]);

    Projection projection;
    projection.demo = true;
    projection.summary = U8(kDemoSummary);
    projection.reason = U8(kDemoReason);

    const std::vector<StepDef>& registry = Registry();
    projection.steps.reserve(registry.size());
    for (std::size_t index = 0; index < registry.size(); ++index) {
        const StepDef& def = registry[index];
        StepView view;
        view.id = def.id;
        view.status = index < kDemoCount ? kDemoStatus[index] : Status::Unknown;
        view.title = U8(def.title);
        view.label = StatusLabel(view.status);
        view.detail = U8(kDemoDetail);
        projection.steps.push_back(view);
    }
    return projection;
}

// ===========================================================================
// 3. 面板私有记录：头文件里的 struct Entry 在这里落地
//    Entry 只是"步骤 id + 子窗口指针"的轻量记录；子窗口归 wx 的父子所有权，
//    Entry 本身由面板 new/delete（见析构）。
// ===========================================================================

struct AgentLearningPath::Entry final
{
    StepId id = StepId::Schematic;
    StepButton* button = nullptr;
};

// ===========================================================================
// 4. AgentLearningPath
// ===========================================================================

AgentLearningPath::AgentLearningPath(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
    SetName("agent-learning-path");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(0, 0));

    // 底部导航提示（原型 .guide-note 的位置；toast 尚未实现时的显式替代）。
    m_note = U8(kNoteInitial);

    const std::vector<StepDef>& registry = Registry();
    m_entries.reserve(registry.size());
    for (const StepDef& def : registry) {
        auto* entry = new Entry();
        entry->id = def.id;
        entry->button = new StepButton(this, def, [this](StepId id) {
            const StepDef* step = Find(id);
            if (step == nullptr) {
                return;
            }
            // 用户点击：只做定位提示 + 选中（导航态）。不碰模型、请求、文件、
            // 配置或其它窗口 —— 这是"点击步骤只做定位，不重跑工具"的实现。
            m_note = U8(kNoteLocatePrefix) + U8(step->title) + U8(kNoteLocateSuffix);
            const int width = GetClientSize().x;
            if (width > 0) {
                Arrange(width);  // 提示变长 → 重新折行，面板高度随之更新（内部会 Refresh）
            } else {
                Refresh(false);
            }
            // 最后才走导航回调：处理器可能销毁本面板，之后不再触碰任何成员。
            Select(id, true);
        });
        m_entries.push_back(entry);
    }

    // 初始投影 = live 的诚实默认值（六步 Unknown）：不依赖任何模型，也不猜 demo。
    // 走 SetProjection 而不是直接赋值，让六个按钮拿到同一份状态/文案/tooltip。
    SetProjection(LivePath());

    Bind(wxEVT_PAINT, &AgentLearningPath::OnPaint, this);
    // 宿主只 SetSize 宽度时的自愈：内容高度恒由内容决定，尺寸事件后按新宽度重排。
    // Arrange 里的 SetSize 只在高度真的变化时才再发一次尺寸事件，深度有限。
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Arrange(event.GetSize().x);
        event.Skip();
    });

    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
    ApplyTheme();
}

AgentLearningPath::~AgentLearningPath()
{
    // 只解绑主题订阅；子窗口由 wx 的父子所有权回收（Entry 不拥有子窗口）。
    m_theme.Reset();
    // 先把记录表换成空的：析构期间若仍有尺寸/绘制事件进来，不会访问已释放的 Entry。
    std::vector<Entry*> entries;
    entries.swap(m_entries);
    for (Entry* entry : entries) {
        delete entry;
    }
}

void AgentLearningPath::ApplyTheme()
{
    // 字体/颜色每次都经 tokens 现取，主题或 DPI 变化后只需要重新测量 + 重绘。
    const wxColour surface = tokens::SurfaceColour(Component::Card);
    if (GetBackgroundColour() != surface) {
        SetBackgroundColour(surface);
    }
    const int width = GetClientSize().x;
    if (width > 0) {
        Arrange(width);
    } else {
        Refresh(false);
    }
}

bool AgentLearningPath::SetProjection(const Projection& projection)
{
    // 变化判定包含 statuses/labels/details/demo/summary/reason（Projection 的默认
    // operator== 逐字段比较，顺序不同也算变化——这里只是"是否需要重绘"的判据）。
    const bool changed = !(m_projection == projection);
    m_projection = projection;

    // 面板没有子窗口能挂整体说明：原因全文挂在面板自己的 tooltip 上（OnPaint 里再兜一次）。
    SetToolTip(m_projection.reason);

    for (Entry* entry : m_entries) {
        if (entry->button == nullptr) {
            continue;
        }
        const StepView* view = FindView(m_projection, entry->id);
        if (view != nullptr) {
            // 标题不取投影（恒为注册表里的设计固定文本），只有状态/文案来自投影。
            entry->button->SetStepState(view->status, view->label, view->detail);
        } else {
            // 投影缺项：显式未知，绝不沿用上一次的状态（也不冒充 todo）。
            entry->button->SetStepState(Status::Unknown, StatusLabel(Status::Unknown),
                                        U8(kLiveDetail));
        }
    }

    // 选中（导航态）与投影是两件事：m_selected 保持不变。
    Refresh(false);
    return changed;
}

void AgentLearningPath::SetNavigationHandler(std::function<void(StepId)> handler)
{
    m_handler = std::move(handler);
}

void AgentLearningPath::Select(StepId id, bool notify)
{
    // 越界/Count/负值一律夹紧到 Count（清除选中），不"就近选中"某一项。
    m_selected = Find(id) != nullptr ? id : StepId::Count;
    for (Entry* entry : m_entries) {
        if (entry->button != nullptr) {
            entry->button->SetSelectedVisual(entry->id == m_selected);
        }
    }
    Refresh(false);

    if (notify) {
        // 导航回调不得启动工具/请求；回调里可能销毁本面板：
        // 先拷贝再调用，调用后不再触碰任何成员。程序化 Select(…,false) 不走到这里。
        auto handler = m_handler;
        if (handler) {
            handler(m_selected);
        }
    }
}

wxWindow* AgentLearningPath::ButtonWindow(StepId id) const
{
    for (Entry* entry : m_entries) {
        if (entry->id == id) {
            return entry->button;
        }
    }
    return nullptr;
}

wxString AgentLearningPath::AccessibleName(StepId id) const
{
    const StepView* view = FindView(m_projection, id);
    if (view == nullptr) {
        return wxString();
    }
    // 与 StepButton::SetName() 同一口径：title + " · " + label
    return view->title + U8(" · ") + view->label;
}

wxString AgentLearningPath::DetailOf(StepId id) const
{
    const StepView* view = FindView(m_projection, id);
    return view != nullptr ? view->detail : wxString();
}

int AgentLearningPath::Arrange(int width)
{
    const int totalWidth = std::max(0, width);

    const int pad = Dip(this, S::StepsPad);        // H119 .steps{padding:8px}
    const int gap = Dip(this, S::StepGap);         // H119 gap:3px
    const int hairline = std::max(1, Dip(this, M::Hairline));
    const int hintPadTop = Dip(this, S::HintPadTop);  // H158 .hintline padding-top:7px
    const int summaryLine = std::max(
        Dip(this, I::DotSmall), tokens::LineHeightDip(this, T::TinyPx, T::LineHeightHint));
    const int noteLineHeight = tokens::LineHeightDip(this, T::TinyPx, T::LineHeightHint);

    const StepMetrics stepMetrics = ComputeStepMetrics(this);
    m_stepHeight = std::max(0, stepMetrics.height);

    const int contentWidth = std::max(0, totalWidth - 2 * pad);

    // 提示文案折行（与 OnPaint 同一字体、同一测量式，保证高度与绘制一致）。
    int noteLineCount = 0;
    if (contentWidth > 0 && !m_note.empty()) {
        Measurer measure(this);
        const wxFont noteFont = tokens::UiFontPx(this, T::TinyPx, tokens::Weight::Normal);
        const std::vector<wxString> lines = WrapText(
            [&measure, &noteFont](const wxString& text) { return measure.Width(noteFont, text); },
            m_note, contentWidth);
        noteLineCount = static_cast<int>(lines.size());
    }

    // ---- 纵向排布：状态行 → 六个步骤 → 虚线发丝线 + 提示块 ----
    std::vector<wxRect> stepRects;
    stepRects.reserve(m_entries.size());

    int y = pad;
    m_summaryRect = wxRect(pad, y, contentWidth, summaryLine);
    y += summaryLine + gap;

    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        stepRects.push_back(wxRect(pad, y, contentWidth, m_stepHeight));
        y += m_stepHeight;
        if (index + 1 < m_entries.size()) {
            y += gap;
        }
    }
    y += gap;  // 最后一步与底部提示块之间留一个步骤间距

    // m_noteRect 覆盖"虚线分隔线 + 7 DIP 上内距 + 折行后的提示文字"整块。
    m_noteRect = wxRect(pad, y, contentWidth, hairline + hintPadTop + noteLineCount * noteLineHeight);
    m_height = m_noteRect.y + m_noteRect.height + pad;

    // ---- 横向落位：每个步骤都是 (x=8, width-16)，且完全落在面板内 ----
    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        StepButton* button = m_entries[index]->button;
        if (button == nullptr) {
            continue;
        }
        const wxRect placed = ClampRect(stepRects[index], totalWidth, m_height);
        button->SetSize(placed);
    }

    if (totalWidth <= 0) {
        // 0 宽客户区：不留下任何非空矩形，也不产生负尺寸；高度仍返回非负值。
        m_summaryRect = wxRect();
        m_noteRect = wxRect();
    }

    SetSize(0, 0, totalWidth, m_height);
    Refresh(false);
    return m_height;
}

void AgentLearningPath::OnPaint(wxPaintEvent&)
{
    const wxColour surface = tokens::SurfaceColour(Component::Card);

    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(surface));
    dc.Clear();

    const wxRect rect = GetClientRect();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }

    // 面板没有子窗口能承载"整体原因"：tooltip 与投影保持同步（SetProjection 已设，
    // 这里兜住构造后直接改投影/换主题的路径；相同就不重复设置）。
    if (GetToolTipText() != m_projection.reason) {
        SetToolTip(m_projection.reason);
    }

    // ① 顶部状态行：6 DIP 圆点 + 10.5 px 文案
    //    demo → danger 点 + dangerText 字；live → text3 点 + text3 字。
    if (m_summaryRect.width > 0 && m_summaryRect.height > 0) {
        const int dot = Dip(this, I::DotSmall);
        const bool demo = m_projection.demo;
        const wxColour dotColour = demo ? A().danger : A().text3;
        const wxColour textColour = demo ? A().dangerText : A().text3;

        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateBrush(wxBrush(dotColour)));
        gc->DrawEllipse(m_summaryRect.x,
                        m_summaryRect.y + (m_summaryRect.height - dot) / 2.0, dot, dot);

        gc->SetFont(tokens::UiFontPx(this, T::TinyPx, tokens::Weight::Normal), textColour);
        const double textX = m_summaryRect.x + dot + Dip(this, S::TagGap);
        const double maxWidth =
            std::max(0.0, m_summaryRect.x + m_summaryRect.width - textX);
        const wxString summary = EllipsizeToWidth(*gc, m_projection.summary, maxWidth);
        const double glyph = TextHeight(*gc);
        gc->DrawText(summary, textX,
                     m_summaryRect.y + std::max(0.0, (m_summaryRect.height - glyph) / 2.0));
    }

    // ② 底部提示块：虚线发丝线 + 折行的 10.5 px text3 文字
    //    原型 .guide-note 的上边是实线；这里按本组件的需要画成虚线，用"虚线 = 提示/
    //    未知"的口径与六步状态区分（与 unknown 圆点同一视觉语言，不改步骤任何外观）。
    if (m_noteRect.width > 0 && m_noteRect.height > 0) {
        const int hairline = std::max(1, Dip(this, M::Hairline));
        const int hintPadTop = Dip(this, S::HintPadTop);

        wxGraphicsPenInfo lineInfo(tokens::BorderColour(BorderRole::Hairline));
        lineInfo.Width(hairline);
        lineInfo.Style(wxPENSTYLE_SHORT_DASH);
        gc->SetPen(gc->CreatePen(lineInfo));
        const double lineY = m_noteRect.y + hairline / 2.0;
        gc->StrokeLine(m_noteRect.x, lineY, m_noteRect.x + m_noteRect.width, lineY);

        const wxFont noteFont = tokens::UiFontPx(this, T::TinyPx, tokens::Weight::Normal);
        gc->SetFont(noteFont, A().text3);
        const int lineHeight = tokens::LineHeightDip(this, T::TinyPx, T::LineHeightHint);
        const std::vector<wxString> lines = WrapText(
            [gc = gc.get()](const wxString& text) { return MeasureWidthPx(*gc, text); }, m_note,
            m_noteRect.width);

        double y = m_noteRect.y + hairline + hintPadTop;
        for (const wxString& line : lines) {
            gc->DrawText(line, m_noteRect.x, y);
            y += lineHeight;
        }
    }
}

}  // namespace agent::gui::path
