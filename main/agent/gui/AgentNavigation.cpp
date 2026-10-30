//
// AgentNavigation.cpp — AGUI-T09 局部顶栏、功能 rail、aw-head 形态入口与教学辅助导航
//
// 契约：main/agent/gui/AgentNavigation.h（本文件只实现该头文件的声明）。
// 设计源：GUI/agent-gui/index.html H87–102（.titlebar/.lights/.brand/.mode-seg/
//         .bar-right/.demo-badge/.status）、H193–207（.aw-head/.aw-id/.aw-tools/.wt/
//         .aw-status）、H309–325（.rail/.rail-logo/.rb/.rail-sep/.rail-sp/.tb-sep/
//         .lv-seg/.lv-lbl）、H365–445（品牌/模式/等级/rail 七导航/四形态）；
//         spec.md §4.1/§4.2/§4.5/§5.1/§5.2/§5.4/§9；phase1/token-map.md（Token 溯源）。
//
// 分工与边界：
//   * 只做「模型 → 控件」投影与「用户意图 → 回调」。**不持有** AgentViewModel / 服务 /
//     协调器指针，不订阅模型，不联网，不读文件，不起定时器，不写配置。
//   * 形态真源是 AgentFormCoordinator：rail 与 aw-head 两组按钮的 pressed 都靠宿主用
//     SetFormPressed()/SetFormSelection() 回写；本文件不各存一份「当前形态」的业务真相。
//   * 颜色/字号/圆角/间距只取 agent::gui::tokens（唯一真源 UiTheme 的 Agent 命名空间）。
//     本文件**没有**任何色值字面量（无 hex、无三参色值构造），也没有第二套调色板。
//   * 绘制与输入分离：所有自绘控件都是 wxAutoBufferedPaintDC + wxGraphicsContext，
//     SetBackgroundStyle(wxBG_STYLE_PAINT)，0 尺寸客户区直接返回。
//   * 装饰性元素（顶栏三颗窗口灯、rail 品牌缩略、rail 分隔线）不建可聚焦子窗口、
//     不绑任何动作。
//
// 为什么有些布局值写成命名常量而不是 token：
//   index.html 里少量设计值（.aw-id 的 7px gap、.aw-status 的 1×10px 分隔线）在
//   UiTheme 的 Agent 命名空间里**尚无** token（token-map「待补门面」清单）。能对上
//   数值且语义最近的既有 token 一律复用（例：顶栏一行 30 DIP 的控件带 →
//   AgentMetrics::ToolButtonWidth = .wt 30×28）；剩下的两处以命名常量 + 行号注释
//   记录来源，不在布局代码里散布裸数字，并已在汇报里作为「待补 token」提出。
//
// ⚠️ 未验项（本轮没有真机 DPI / 读屏 / 真实鼠标键盘轨迹 / 深色档视觉对照）：
//   * 焦点环与 hover/pressed 配色的一致性只经代码推导，未截图逐像素对照；
//   * 顶栏换行点、rail 横条换行点只按 375/400/560/960/1180 做算术布局验证，
//     未在 125%/150% 缩放下逐个核对；
//   * SetName/SetToolTip + wxAccessible 只保证「设置且可读回」，**不是**读屏实测；
//   * IME、真实 Tab 遍历顺序、拖拽与真实鼠标轨迹未验证；
//   * demo 胶囊（AgentPill）自绘底色固定为卡片面，在顶栏玻璃条上四角会有 1 像素级
//     色差（AgentPill 属 T05，不在本任务可改范围）。
//

#include "agent/gui/AgentNavigation.h"

#include "agent/gui/AgentControls.h"
#include "agent/gui/AgentTokens.h"
#include "ui/UiFocusRing.h"
#include "ui/UiTheme.h"

#include <wx/access.h>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/pen.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace agent::gui::nav {

namespace {

// ---------------------------------------------------------------------------
// Token 门面短名（仍是单一来源；只是让布局代码可读）
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

wxFont FontOf(const wxWindow* window, double cssPx, tokens::Weight weight)
{
    return tokens::UiFontPx(window, cssPx, weight);
}

wxString U8(const char* text)
{
    return wxString::FromUTF8(text);
}

// index.html 有、UiTheme::AgentSpacing 里尚无 token 的两个设计值（见文件头说明）。
constexpr int kIdTagGapDip = 7;         ///< H197 .aw-id gap:7px
constexpr int kStatusSepDip = 10;       ///< H206 .aw-status .sep height:10px

// ---------------------------------------------------------------------------
// 圆角 / 环形图元（与 AgentControls 同一口径：路径内缩半个线宽，避免描边被裁）
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
/// 只画在控件内部：wx 子窗口无法把描边画到客户区之外。
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

    StrokeBand(gc, band, bandRadius, glow, A().accentSoft);

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

/// 单行省略（U+2026）。布局测量与绘制必须用同一个测量器，否则自然尺寸下也会被裁。
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

/// 与 OnPaint 同一条 GDI+ 文本管线（wxClientDC + wxGraphicsContext）的测量器。
class Measurer
{
public:
    explicit Measurer(const wxWindow* window)
        : m_dc(const_cast<wxWindow*>(window)), m_window(window)
    {
        m_gc.reset(wxGraphicsContext::Create(m_dc));
    }

    wxSize Text(const wxFont& font, const wxString& text) const
    {
        if (m_gc) {
            m_gc->SetFont(font, A().text1);
            double width = 0.0;
            double height = 0.0;
            m_gc->GetTextExtent(text, &width, &height);
            return wxSize(static_cast<int>(std::ceil(width)) + 1,
                          static_cast<int>(std::ceil(height)));
        }
        if (m_window == nullptr) {
            return wxSize(0, 0);
        }
        return const_cast<wxWindow*>(m_window)->GetTextExtent(text);
    }

private:
    wxClientDC m_dc;
    const wxWindow* m_window = nullptr;
    std::unique_ptr<wxGraphicsContext> m_gc;
};

// ---------------------------------------------------------------------------
// rail 背景的「子窗口版」：wx 子 HWND 没有透明背景（THE-021），因此每个 rail 子控件
// 先按**父条自身的渐变坐标**铺一遍底，再画自己的状态面。这样按钮 hover/选中之外
// 的区域与父条像素对齐，不会出现方块色差。
// ---------------------------------------------------------------------------

void FillRailBackdrop(wxGraphicsContext& gc, const wxWindow* child, const wxRect& rect)
{
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }
    const wxWindow* parent = child != nullptr ? child->GetParent() : nullptr;
    gc.SetPen(*wxTRANSPARENT_PEN);
    if (parent == nullptr) {
        gc.SetBrush(gc.CreateBrush(wxBrush(A().glassRailBottom)));
        gc.DrawRectangle(rect.x, rect.y, rect.width, rect.height);
        return;
    }
    const int parentHeight = std::max(1, parent->GetClientSize().y);
    const int offsetY = child->GetRect().y;  // 子窗口在父条里的纵向原点
    gc.SetBrush(gc.CreateLinearGradientBrush(
        0.0, static_cast<double>(-offsetY), 0.0, static_cast<double>(parentHeight - offsetY),
        A().glassRailTop, A().glassRailBottom));
    gc.DrawRectangle(rect.x, rect.y, rect.width, rect.height);
}

// ---------------------------------------------------------------------------
// 可访问性基底（只保证「设置且可通过 API 读回」；未在 Windows 读屏上实测）
// ---------------------------------------------------------------------------
#if wxUSE_ACCESSIBILITY
struct AccessInfo
{
    wxString name;
    wxString help;
    wxAccRole role = wxROLE_SYSTEM_PUSHBUTTON;
    bool focusable = false;
    bool focused = false;
    bool unavailable = false;
    bool checked = false;
};

class NavAccessible final : public wxAccessible
{
public:
    using Provider = std::function<AccessInfo()>;

    NavAccessible(wxWindow* window, Provider provider)
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
        *role = m_provider().role;
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
        if (info.checked) {
            *state |= wxACC_STATE_SYSTEM_CHECKED;
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
// IconButton — rail 的 .rb 与 aw-head 的 .wt 共用的自绘图标按钮
//
// 状态机与 AgentControls::AgentButton 一致：Disabled > Pressed > Selected(pressed 投影)
// > Hover > Normal。pressed 投影（SetPressedVisual）只表达「学习路径显示中 / 当前形态」，
// 与鼠标按住是两件事，因此分开存。
// ---------------------------------------------------------------------------
class IconButton : public wxControl
{
public:
    /// 底衬来源：rail 渐变（父条铺底）或对话卡片面（.aw-tools 所在容器）。
    enum class Backdrop
    {
        Rail,
        Card
    };

    IconButton(wxWindow* parent, Action action, icons::IconId icon, const wxString& label,
               const wxString& accessibleName, bool primary, int widthDip, int heightDip,
               int iconDip, int radiusDip, Backdrop backdrop)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          m_action(action), m_icon(icon), m_label(label), m_accessibleName(accessibleName),
          m_primary(primary), m_widthDip(widthDip), m_heightDip(heightDip), m_iconDip(iconDip),
          m_radiusDip(radiusDip), m_backdrop(backdrop)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetName(accessibleName.empty() ? label : accessibleName);
        SetMinSize(wxSize(0, 0));
        ApplyAccessibility();

        Bind(wxEVT_PAINT, &IconButton::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &IconButton::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &IconButton::OnLeftUp, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &IconButton::OnCaptureLost, this);
        Bind(wxEVT_ENTER_WINDOW, &IconButton::OnEnter, this);
        Bind(wxEVT_LEAVE_WINDOW, &IconButton::OnLeave, this);
        Bind(wxEVT_KEY_DOWN, &IconButton::OnKeyDown, this);
        Bind(wxEVT_KEY_UP, &IconButton::OnKeyUp, this);
        Bind(wxEVT_SET_FOCUS, &IconButton::OnFocus, this);
        Bind(wxEVT_KILL_FOCUS, &IconButton::OnKillFocus, this);
    }

    ~IconButton() override
    {
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void SetActionHandler(std::function<void(Action)> handler) { m_handler = std::move(handler); }
    Action GetAction() const { return m_action; }

    /// 程序化 pressed（不触发回调，只改视觉）。
    void SetPressedVisual(bool pressed)
    {
        if (m_selected == pressed) {
            return;
        }
        m_selected = pressed;
        ApplyAccessibility();
        Refresh(false);
    }
    bool PressedVisual() const { return m_selected; }

    void SetEnabledState(bool enabled, const wxString& reason)
    {
        m_disabledReason = enabled ? wxString() : reason;
        if (!enabled) {
            ResetPress();
            if (HasCapture()) {
                ReleaseMouse();
            }
        }
        if (IsEnabled() != enabled) {
            wxControl::Enable(enabled);
        }
        ApplyAccessibility();
        Refresh(false);
    }

    wxString DisabledReason() const { return m_disabledReason; }

    /// 图标边长（DIP）：竖排 18，窄档 34px 按钮用 16（H357 缩小的视觉补偿）。
    void SetIconSizeDip(int dip)
    {
        if (m_iconDip == dip) {
            return;
        }
        m_iconDip = dip;
        Refresh(false);
    }

    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return AcceptsFocus(); }

protected:
    wxSize DoGetBestSize() const override
    {
        return wxSize(Dip(this, m_widthDip), Dip(this, m_heightDip));
    }

private:
    void ApplyAccessibility()
    {
        SetName(m_accessibleName.empty() ? m_label : m_accessibleName);
        // 禁用时 tooltip 换成原因（原型 .level[disabled] 保留 title 说明）。
        SetToolTip(IsEnabled() ? m_label : (m_disabledReason.empty() ? m_label : m_disabledReason));
#if wxUSE_ACCESSIBILITY
        if (GetAccessible() == nullptr) {
            SetAccessible(new NavAccessible(this, [this] {
                AccessInfo info;
                info.name = m_accessibleName.empty() ? m_label : m_accessibleName;
                info.help = IsEnabled() ? m_label : m_disabledReason;
                info.role = wxROLE_SYSTEM_PUSHBUTTON;
                info.focusable = true;
                info.focused = HasFocus();
                info.unavailable = !IsEnabled();
                info.checked = m_selected;
                return info;
            }));
        }
#endif
    }

    wxColour BackdropColour() const
    {
        return m_backdrop == Backdrop::Rail ? A().glassRailBottom
                                            : tokens::SurfaceColour(Component::Card);
    }

    wxColour IconTint() const
    {
        if (!IsEnabled()) {
            return A().text3;
        }
        if (m_primary) {
            return A().textOnAccent;
        }
        if (m_selected) {
            return A().accentText;
        }
        if (m_hover || m_mousePressed || m_keyPressed) {
            return A().text1;
        }
        return A().text2;
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
        // 处理器可能销毁本控件（切换形态会重建宿主）：调用后不再触碰成员。
        auto handler = m_handler;
        if (handler) {
            handler(m_action);
        }
    }

    void OnPaint(wxPaintEvent&)
    {
        const wxRect rect = GetClientRect();
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(BackdropColour()));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || rect.width <= 0 || rect.height <= 0) {
            return;
        }

        // ① 先铺父层底衬（rail 渐变按父条坐标对齐；卡片面直接同色铺满）
        if (m_backdrop == Backdrop::Rail) {
            FillRailBackdrop(*gc, this, rect);
        } else {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(gc->CreateBrush(wxBrush(BackdropColour())));
            gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);
        }

        const double radius = ClampRadius(Dip(this, m_radiusDip), rect);
        const bool enabled = IsEnabled();
        const bool acting = m_mousePressed || m_keyPressed;

        // ② 状态面
        if (m_primary && enabled) {
            wxGraphicsPath path = gc->CreatePath();
            path.AddRoundedRectangle(rect.x, rect.y, rect.width, rect.height, radius);
            gc->SetPen(*wxTRANSPARENT_PEN);
            if (acting) {
                gc->SetBrush(gc->CreateBrush(wxBrush(A().ctaBottom)));
            } else {
                // H320 hover 用 brightness(1.08)：wx 无 filter，把渐变终点抬到亮端近似。
                const wxColour bottom = m_hover ? A().ctaTop : A().ctaBottom;
                gc->SetBrush(gc->CreateLinearGradientBrush(
                    rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, A().ctaTop, bottom));
            }
            gc->FillPath(path);
        } else if (enabled && m_selected) {
            // H318 aria-pressed=true：accent-soft 底 + accent .40 边
            FillRound(*gc, rect, radius, A().accentSoft);
            StrokeRound(*gc, rect, radius, tokens::BorderColour(BorderRole::Accent),
                        Dip(this, M::Hairline));
        } else if (enabled && (acting || m_hover)) {
            FillRound(*gc, rect, radius, A().sunken);
            if (m_backdrop == Backdrop::Card) {
                // H203 .wt:hover 自带 hairline 边（rail 的 .rb:hover 没有）
                StrokeRound(*gc, rect, radius, tokens::BorderColour(BorderRole::Hairline),
                            Dip(this, M::Hairline));
            }
        }

        // ③ 图标：居中方形；宽高不等时按短边居中，不拉伸（icons::Draw 的契约）
        const int iconSize =
            std::max(0, std::min(Dip(this, m_iconDip), std::min(rect.width, rect.height)));
        if (iconSize > 0) {
            const int iconX = rect.x + (rect.width - iconSize) / 2;
            const int iconY = rect.y + (rect.height - iconSize) / 2;
            icons::Draw(*gc, m_icon, wxRect(iconX, iconY, iconSize, iconSize), IconTint());
        }

        // ④ 键盘焦点环（仅键盘来源焦点）
        if (HasFocus() && m_focusTracker.KeyboardFocused()) {
            const wxColour ring = m_primary ? A().textOnAccent : A().accent;
            DrawFocusRing(*gc, this, rect, radius, ring);
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
        // 捕获被系统/其它窗口拿走：按压态必须复位，否则留下「按住的假象」。
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

    std::function<void(Action)> m_handler;
    Action m_action = Action::Count;
    icons::IconId m_icon = icons::IconId::Unknown;
    wxString m_label;
    wxString m_accessibleName;
    wxString m_disabledReason;
    bool m_primary = false;
    bool m_selected = false;
    bool m_hover = false;
    bool m_mousePressed = false;
    bool m_keyPressed = false;
    bool m_spaceDown = false;
    bool m_enterDown = false;
    int m_widthDip = 0;
    int m_heightDip = 0;
    int m_iconDip = 0;
    int m_radiusDip = 0;
    Backdrop m_backdrop = Backdrop::Rail;
    UiFocusRing::Tracker m_focusTracker;
};

// ---------------------------------------------------------------------------
// RailLogo — rail 品牌缩略（H397/aria-hidden）：纯装饰，不可聚焦、无动作。
// 单独做成子窗口是为了能被 rail 的排布精确定位（顶栏灯则保持纯绘制）。
// ---------------------------------------------------------------------------
class RailLogo final : public wxPanel
{
public:
    explicit RailLogo(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetName(U8("品牌标识（装饰）"));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(0, 0));
        Bind(wxEVT_PAINT, &RailLogo::OnPaint, this);
    }

    ~RailLogo() override = default;

    void SetLogoSizeDip(int dip)
    {
        m_sizeDip = dip;
        InvalidateBestSize();
    }

    bool AcceptsFocus() const override { return false; }
    bool AcceptsFocusFromKeyboard() const override { return false; }

protected:
    wxSize DoGetBestSize() const override
    {
        const int size = Dip(this, m_sizeDip);
        return wxSize(size, size);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        const wxRect rect = GetClientRect();
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(A().glassRailBottom));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || rect.width <= 0 || rect.height <= 0) {
            return;
        }
        FillRailBackdrop(*gc, this, rect);
        // BrandMark 是固定 artwork：tint 传入不参与上色（icons::Draw 契约）。
        icons::Draw(*gc, icons::IconId::BrandMark, rect, A().text1);
    }

    int m_sizeDip = M::RailLogoSize;
};

// ---------------------------------------------------------------------------
// 布局：把若干个「流量项」按可用宽度贪心换行，返回每行的已用宽度与行高。
// 顶栏与 rail 横条共用同一套口径（gap 由调用方给）。
// ---------------------------------------------------------------------------
struct FlowEntry
{
    int index = 0;   ///< 调用方数组下标（0 = 无）
    int width = 0;
    int height = 0;
};

struct FlowRow
{
    int usedWidth = 0;
    int height = 0;
};

/// 贪心换行：只在同一行内累加，超宽就换行（与 H89 flex-wrap / H338 横条同序）。
/// 单项宽于整行时按整行宽夹紧，保证永不越界。
void FlowPack(const std::vector<FlowEntry>& items, int contentWidth, int gap,
              std::vector<FlowRow>* rows, std::vector<int>* rowOfItem, std::vector<int>* xOfItem)
{
    rows->clear();
    rowOfItem->assign(items.size(), 0);
    xOfItem->assign(items.size(), 0);
    rows->push_back(FlowRow{});

    int row = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const int itemWidth = std::max(0, std::min(items[i].width, contentWidth));
        const int advance = (rows->at(static_cast<size_t>(row)).usedWidth > 0 ? gap : 0) + itemWidth;
        if (rows->at(static_cast<size_t>(row)).usedWidth > 0 &&
            rows->at(static_cast<size_t>(row)).usedWidth + advance > contentWidth) {
            rows->push_back(FlowRow{});
            ++row;
        }
        FlowRow& current = rows->at(static_cast<size_t>(row));
        const int x = current.usedWidth + (current.usedWidth > 0 ? gap : 0);
        (*rowOfItem)[i] = row;
        (*xOfItem)[i] = x;
        current.usedWidth = x + itemWidth;
        current.height = std::max(current.height, items[i].height);
    }
}

/// 把子窗口放到 (x,y,w,h)，并夹紧到给定的客户区尺寸内（任何宽度都不越界、不变负）。
void PlaceInside(wxWindow* window, int x, int y, int width, int height, int boundWidth,
                 int boundHeight)
{
    if (window == nullptr) {
        return;
    }
    x = std::max(0, std::min(x, boundWidth));
    y = std::max(0, std::min(y, boundHeight));
    width = std::max(0, std::min(width, boundWidth - x));
    height = std::max(0, std::min(height, boundHeight - y));
    window->SetSize(x, y, width, height);
}

wxRect ClampedRect(int x, int y, int width, int height, int boundWidth, int boundHeight)
{
    x = std::max(0, std::min(x, boundWidth));
    y = std::max(0, std::min(y, boundHeight));
    width = std::max(0, std::min(width, boundWidth - x));
    height = std::max(0, std::min(height, boundHeight - y));
    return wxRect(x, y, width, height);
}

}  // namespace

// ===========================================================================
// 1. 动作注册表
//
// 顺序 == index.html 视觉顺序（H399–H436）；order 连续 0..11，测试/日志用它定位。
// 文案与 aria-label 逐条对齐原型（title / aria-label 分列，便于 tooltip 与读屏分开取）。
// ===========================================================================

const std::vector<Item>& Catalog()
{
    static const std::vector<Item> items = {
        {Action::NewAsk, Group::Primary, icons::IconId::NewAsk, "newAsk", "新建提问", "新建提问", 0},
        {Action::ToggleGuide, Group::Toggle, icons::IconId::RailGuide, "rGuide", "学习路径 · 六步",
         "学习路径 · 六步", 1},
        {Action::JumpIssue, Group::Jump, icons::IconId::RailIssue, "rIssue", "当前问题", "查看当前问题",
         2},
        {Action::JumpEvidence, Group::Jump, icons::IconId::RailEvidence, "rEvid", "证据链",
         "查看证据链", 3},
        {Action::JumpPlan, Group::Jump, icons::IconId::RailPlan, "rPlan", "验证计划", "查看验证计划", 4},
        {Action::JumpCourse, Group::Jump, icons::IconId::RailCourse, "rCourse", "课程引用",
         "查看课程引用", 5},
        {Action::JumpRecord, Group::Jump, icons::IconId::RailRecord, "rRec", "学习记录", "查看学习记录",
         6},
        {Action::JumpChecker, Group::Jump, icons::IconId::RailChecker, "rChk", "检查器与状态",
         "查看检查器与状态", 7},
        {Action::FormCenter, Group::Form, icons::IconId::FormCenter, "qCenter", "主对话", "主对话", 8},
        {Action::FormDock, Group::Form, icons::IconId::FormDock, "qDock", "右侧停靠", "右侧停靠", 9},
        {Action::FormFloat, Group::Form, icons::IconId::FormFloat, "qFloat", "悬浮窗", "悬浮窗", 10},
        {Action::FormPet, Group::Form, icons::IconId::FormPet, "qPet", "桌宠待命", "桌宠待命", 11},
    };
    return items;
}

const Item* Find(Action action)
{
    const std::vector<Item>& items = Catalog();
    const auto index = static_cast<std::size_t>(action);
    if (index >= items.size()) {
        return nullptr;
    }
    return &items[index];
}

const Item* FindById(const char* id)
{
    if (id == nullptr) {
        return nullptr;
    }
    for (const Item& item : Catalog()) {
        if (item.id != nullptr && wxString::FromUTF8(item.id) == wxString::FromUTF8(id)) {
            return &item;
        }
    }
    return nullptr;
}

bool IsFormAction(Action action)
{
    return FormOf(action) != AgentForm::None;
}

AgentForm FormOf(Action action)
{
    switch (action) {
        case Action::FormCenter:
            return AgentForm::Center;
        case Action::FormDock:
            return AgentForm::Dock;
        case Action::FormFloat:
            return AgentForm::Float;
        case Action::FormPet:
            return AgentForm::Pet;
        case Action::NewAsk:
        case Action::ToggleGuide:
        case Action::JumpIssue:
        case Action::JumpEvidence:
        case Action::JumpPlan:
        case Action::JumpCourse:
        case Action::JumpRecord:
        case Action::JumpChecker:
        case Action::Count:
            break;
    }
    return AgentForm::None;
}

bool ActionForForm(AgentForm form, Action* out)
{
    if (form == AgentForm::None) {
        return false;
    }
    const std::vector<Item>& items = Catalog();
    for (const Item& item : items) {
        if (item.group == Group::Form && FormOf(item.action) == form) {
            if (out != nullptr) {
                *out = item.action;
            }
            return true;
        }
    }
    return false;
}

// ===========================================================================
// 2. 教学辅助分区（七个导航目标；T09 只提供入口 + 诚实占位）
// ===========================================================================

namespace {

struct SectionCopy
{
    Section section;
    const char* id;
    const char* title;
    const char* summary;
    const char* body;
    const char* reason;
};

const std::vector<SectionCopy>& SectionTable()
{
    static const std::vector<SectionCopy> table = {
        {Section::Guide, "guide", "学习路径", "六步",
         "学习路径尚未接入真实教学记录，当前不显示进度，"
         "不显示演示步骤、完成态或推断结论。",
         "学习路径尚未接入。"},
        {Section::Issue, "issue", "当前问题", "尚未接通",
         "当前问题分区尚未接入可信教学结果。这里不显示演示 ID、revision 或推断结论。",
         "当前问题内容尚未接入。"},
        {Section::Evidence, "evidence", "证据链", "尚未接通",
         "证据链分区尚未接入可信证据。这里不显示演示证据、观察/假设标签或推断结论。",
         "证据链内容尚未接入。"},
        {Section::Plan, "plan", "验证计划", "只读",
         "验证计划分区只显示后端 plan-card 的只读摘要；"
         "没有计划时如实显示“尚未生成”，不显示演示步骤、批准状态或执行入口。",
         "验证计划内容尚未接入（只读投影）。"},
        {Section::Course, "course", "课程引用", "只读",
         "课程引用分区只显示后端随卡片返回的只读引用元数据；"
         "没有引用时如实显示“尚未生成”，不抓取课程正文、不显示进度或跳转授权。",
         "课程引用内容尚未接入（只读投影）。"},
        {Section::Record, "record", "学习记录", "未接通",
         "学习记录分区仍无可信 wire 合同与只读路由；"
         "不显示错题、统计、画像或历史，也不提供导出、删除或历史写入。",
         "学习记录内容尚未接入（合同未定义）。"},
        {Section::Checker, "checker", "检查器与状态", "未接通",
         "检查器与状态分区仍无可信 wire 字段或聚合路由；"
         "不把 rule_match/诊断当作检查器通过，也不显示静态全绿、分数或进度。",
         "检查器与状态内容尚未接入（合同未定义）。"},
    };
    return table;
}

const SectionCopy* FindSectionCopy(Section section)
{
    const std::vector<SectionCopy>& table = SectionTable();
    const auto index = static_cast<std::size_t>(section);
    if (index >= table.size()) {
        return nullptr;
    }
    return &table[index];
}

}  // namespace

const std::vector<Section>& SectionCatalog()
{
    static const std::vector<Section> sections = {
        Section::Guide, Section::Issue,   Section::Evidence, Section::Plan,
        Section::Course, Section::Record, Section::Checker,
    };
    return sections;
}

const char* SectionId(Section section)
{
    const SectionCopy* copy = FindSectionCopy(section);
    return copy != nullptr ? copy->id : "";
}

const char* SectionTitle(Section section)
{
    const SectionCopy* copy = FindSectionCopy(section);
    return copy != nullptr ? copy->title : "";
}

Action SectionAction(Section section)
{
    switch (section) {
        case Section::Guide:
            return Action::ToggleGuide;
        case Section::Issue:
            return Action::JumpIssue;
        case Section::Evidence:
            return Action::JumpEvidence;
        case Section::Plan:
            return Action::JumpPlan;
        case Section::Course:
            return Action::JumpCourse;
        case Section::Record:
            return Action::JumpRecord;
        case Section::Checker:
            return Action::JumpChecker;
        case Section::Count:
            break;
    }
    return Action::Count;
}

bool SectionForAction(Action action, Section* out)
{
    const std::vector<Section>& sections = SectionCatalog();
    for (Section section : sections) {
        if (SectionAction(section) == action) {
            if (out != nullptr) {
                *out = section;
            }
            return true;
        }
    }
    return false;
}

SectionProjection ProjectSection(const vm::AgentViewModel& model, Section section)
{
    SectionProjection projection;
    const SectionCopy* copy = FindSectionCopy(section);
    if (copy == nullptr) {
        // 越界：显式未知，不冒充任何分区。
        projection.summary = U8("尚未接通");
        projection.body = U8("未知分区，无法显示内容。");
        projection.reason = U8("未知分区。");
        return projection;
    }

    // T09：**没有任何分区**可用。Guide 面板存在，但六步内容由 T10 填；
    // 六个教学辅助分区由 T15–T19 填。这里宁可显示原因，也不编造教学内容。
    projection.available = false;
    projection.summary = U8(copy->summary);
    projection.body = U8(copy->body);
    projection.reason = U8(copy->reason);

    if (section == Section::Plan) {
        // 验证计划是**只读**投影：plan-card 由 AgentWireProjection 严格校验后写入
        // PlanView；这里只显示 bounded 文本，不存在批准/执行/取消入口。
        const vm::PlanSectionView plan =
            vm::ProjectPlanSection(model.Plan(), model.BoundRevision(), model.DemoMode());
        projection.available = plan.available;
        projection.summary = wxString::FromUTF8(plan.summary.c_str());
        projection.body = wxString::FromUTF8(plan.body.c_str());
        projection.reason = wxString::FromUTF8(plan.reason.c_str());
        return projection;
    }

    if (section == Section::Course) {
        // 课程引用是**只读**投影：不抓取课程正文、不显示进度，url 仅作纯文本。
        const vm::CourseSectionView course = vm::ProjectCourseSection(
            model.Course(), model.Capabilities().Get(vm::CapabilityKey::Course),
            model.BoundRevision(), model.DemoMode());
        projection.available = course.available;
        projection.summary = wxString::FromUTF8(course.summary.c_str());
        projection.body = wxString::FromUTF8(course.body.c_str());
        projection.reason = wxString::FromUTF8(course.reason.c_str());
        return projection;
    }
    if (section == Section::Record) {
        // 仍无可信 record 合同：只显示诚实边界 + 能力原因，不显示任何记录数据。
        const vm::UnavailableSectionView record =
            vm::ProjectRecordSection(model.Capabilities().Get(vm::CapabilityKey::Record));
        projection.available = record.available;
        projection.summary = wxString::FromUTF8(record.summary.c_str());
        projection.body = wxString::FromUTF8(record.body.c_str());
        projection.reason = wxString::FromUTF8(record.reason.c_str());
        return projection;
    }
    if (section == Section::Checker) {
        // 仍无可信 checker wire 字段：不把 rule_match/诊断当通过，也不造假全绿。
        const vm::UnavailableSectionView checker =
            vm::ProjectCheckerSection(model.Capabilities().Get(vm::CapabilityKey::Checkers));
        projection.available = checker.available;
        projection.summary = wxString::FromUTF8(checker.summary.c_str());
        projection.body = wxString::FromUTF8(checker.body.c_str());
        projection.reason = wxString::FromUTF8(checker.reason.c_str());
        return projection;
    }
    return projection;
}

// ===========================================================================
// 3. 顶栏 / 状态条投影
// ===========================================================================

namespace {

wxString StatusLabel(vm::RunStatus status)
{
    switch (status) {
        case vm::RunStatus::Unknown:
            return U8("状态未知");
        case vm::RunStatus::Running:
            return U8("运行中");
        case vm::RunStatus::WaitingStudent:
            return U8("等待学生");
        case vm::RunStatus::WaitingEvidence:
            return U8("等待证据");
        case vm::RunStatus::WaitingApproval:
            return U8("等待批准");
        case vm::RunStatus::RecoveryRequired:
            return U8("恢复待确认");
        case vm::RunStatus::Completed:
            return U8("已完成");
        case vm::RunStatus::Succeeded:
            return U8("成功");
        case vm::RunStatus::Degraded:
            return U8("已降级");
        case vm::RunStatus::Cancelled:
            return U8("已取消");
        case vm::RunStatus::Failed:
            return U8("失败");
        case vm::RunStatus::BudgetExceeded:
            return U8("超出预算");
    }
    return U8("状态未知");
}

}  // namespace

StatusProjection ProjectStatus(const vm::AgentViewModel& model)
{
    const vm::StatusView& status = model.Status();
    StatusProjection projection;

    // 状态：后端原文优先（不认识也要照原样显示），否则用中文映射。
    projection.statusText =
        status.rawStatus.empty() ? StatusLabel(status.status) : wxString::FromUTF8(status.rawStatus);

    if (status.modelUsed == "true") {
        projection.ruleMode = U8("模型模式");
        projection.modelUsed = U8("model_used=true");
    } else if (status.modelUsed == "false") {
        projection.ruleMode = U8("规则模式");
        projection.modelUsed = U8("model_used=false");
    } else {
        projection.ruleMode = U8("模式未知");
        projection.modelUsed = U8("model_used=未知");
    }

    // policy 恒为显式未知：contracts/edu-agent/v1 的 run DTO **没有** policy 字段
    // （只有 ui-receipt 合同有），因此这里没有可信来源。原型 H388/H643 的
    // 「policy edu-default-v1」是固定演示串，不得写成常量。等 T22/T24 提供可信
    // 来源后再改成模型投影。
    projection.policy = U8("policy 未知");

    if (status.sessionId.empty()) {
        projection.session = U8("session 未绑定");
    } else {
        const wxString id = wxString::FromUTF8(status.sessionId);
        wxString shortId = id;
        if (id.length() > 12) {
            shortId = id.Left(12);
            shortId += wxUniChar(0x2026);  // 「…」：只截前缀，不伪造完整 ID
        }
        projection.session = U8("session #") + shortId;
    }

    projection.demo = model.DemoMode();
    projection.busy = status.busy;
    return projection;
}

wxString ComposeStatusLine(const StatusProjection& status)
{
    return status.statusText + U8(" · ") + status.ruleMode + U8(" · ") + status.modelUsed +
           U8(" │ ") + status.policy + U8(" │ ") + status.session;
}

// ===========================================================================
// 4. 提示级别的非猜测映射（DEC-06 未决：绝不 U2 + 1）
// ===========================================================================

int LevelNumber(const vm::Level& level)
{
    if (level.kind != vm::Level::Kind::V1String) {
        return 0;  // U2 整数语义未确认，不做任何换算
    }
    const std::string text = level.ToText();
    if (text == "L1") {
        return 1;
    }
    if (text == "L2") {
        return 2;
    }
    if (text == "L3") {
        return 3;
    }
    // "L4"、其它字符串、空串一律未知（0）：L4 需策略允许与两次确认，A 阶段关闭。
    return 0;
}

vm::Level LevelForNumber(int number)
{
    switch (number) {
        case 1:
            return vm::Level::V1("L1");
        case 2:
            return vm::Level::V1("L2");
        case 3:
            return vm::Level::V1("L3");
        default:
            return vm::Level::Unbound();
    }
}

// ===========================================================================
// 5. AgentTopBar
// ===========================================================================

namespace {

wxString BrandPrimaryText()
{
    return U8("SigFlow · 教学版");
}

wxString BrandSecondaryText()
{
    return U8("教育 Agent");
}

wxString DemoFullText()
{
    return U8("演示数据 · 非真实后端");
}

wxString DemoCompactText()
{
    return U8("演示");
}

wxString LevelCaptionText()
{
    return U8("提示");
}

}  // namespace

AgentTopBar::AgentTopBar(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetName("agent-topbar");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(0, 0));

    m_modeSegment = new controls::AgentSegmented(this, wxID_ANY);
    m_modeSegment->AddSegment(0, U8("学习"), U8("学习模式"));
    m_modeSegment->AddSegment(1, U8("工作区"), U8("工作区模式"));
    m_modeSegment->SetToolTip(U8("学习 / 工作区模式"));
    m_modeSegment->Bind(controls::EVT_AGENT_SEGMENT, [this](wxCommandEvent& event) {
        const int selection = event.GetSelection();
        if (selection != 0 && selection != 1) {
            return;
        }
        const vm::AppMode mode = selection == 0 ? vm::AppMode::Learning : vm::AppMode::Working;
        // 分段控件自身已改了可见选中项：这里同步投影，避免 Mode() 与画面不一致。
        // 模型仍是唯一真源，随后 SetMode() 会再投影一次（程序化 SetSelection 不发事件）。
        m_mode = mode;
        // 处理器可能销毁本控件：先拷贝再调用。
        auto handler = m_modeHandler;
        if (handler) {
            handler(mode);
        }
    });

    static const char* kLevels[4] = {"L1", "L2", "L3", "L4"};
    static const char* kLevelNames[4] = {"提示级别 L1", "提示级别 L2", "提示级别 L3",
                                         "提示级别 L4（关闭）"};
    static const char* kLevelTips[4] = {
        "L1 方向性提问 · 只给方向，不给答案",
        "L2 现象/机制提示 · 由你主动请求",
        "L3 具体线索 · 需有效证据映射，仅指向不代改",
        "参考解要点：需策略允许与两次确认，A 阶段关闭",
    };
    for (int index = 0; index < 4; ++index) {
        auto* button = new controls::AgentButton(this, wxID_ANY, U8(kLevels[index]),
                                                 controls::Variant::Secondary);
        button->SetAccessibleName(U8(kLevelNames[index]));
        button->SetToolTip(U8(kLevelTips[index]));
        if (index == 3) {
            // L4 恒不可用：不是「暂时没内容」，而是策略未决（DEC-05/DEC-06）。
            button->SetDisabledReason(U8(kLevelTips[3]));
        }
        button->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            const int level = index + 1;
            if (level < 1 || level > 3) {
                return;
            }
            auto handler = m_levelHandler;
            if (handler) {
                handler(level);
            }
        });
        m_levels[index] = button;
    }

    m_demoPill = new controls::AgentPill(this, wxID_ANY, DemoFullText(), controls::Tone::Danger);
    m_demoPill->SetToolTip(U8("演示数据：来自模型 demo 标记，不是真实后端结果"));
    m_demoPill->Hide();

    // 默认投影：全部显式「未知/未绑定」，不留空串（空串会被画成空的胶囊）。
    m_status.statusText = U8("状态未知");
    m_status.ruleMode = U8("模式未知");
    m_status.modelUsed = U8("model_used=未知");
    m_status.policy = U8("policy 未知");
    m_status.session = U8("session 未绑定");
    SetToolTip(ComposeStatusLine(m_status));

    Bind(wxEVT_PAINT, &AgentTopBar::OnPaint, this);

    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
    ApplyTheme();
}

AgentTopBar::~AgentTopBar()
{
    // 只解绑主题订阅；子窗口由 wx 的父子所有权回收，不在这里 Deref。
    m_theme.Reset();
}

void AgentTopBar::SetIntentHandlers(std::function<void(vm::AppMode)> modeHandler,
                                    std::function<void(int)> levelHandler)
{
    m_modeHandler = std::move(modeHandler);
    m_levelHandler = std::move(levelHandler);
}

void AgentTopBar::SetMode(vm::AppMode mode)
{
    m_mode = mode;
    if (m_modeSegment != nullptr) {
        // 程序化选中：AgentSegmented::SetSelection 不发业务事件。
        m_modeSegment->SetSelection(mode == vm::AppMode::Learning ? 0 : 1);
    }
    Refresh(false);
}

void AgentTopBar::SetLevel(int level)
{
    // 1..3 精确选中一项；其它值（0/L4/U2 未知/越界）只清除选中，绝不点亮 L4。
    const bool known = level >= 1 && level <= 3;
    for (int index = 0; index < 4; ++index) {
        if (m_levels[index] != nullptr) {
            m_levels[index]->SetSelected(known && index + 1 == level);
        }
    }
    m_level = known ? level : 0;  // 未知显式记为 0（与 LevelNumber 的口径一致）
    Refresh(false);
}

void AgentTopBar::SetDemo(bool demo)
{
    m_demo = demo;
    if (m_demoPill != nullptr) {
        m_demoPill->SetText(m_narrow ? DemoCompactText() : DemoFullText());
        m_demoPill->Show(demo);
    }
    // 胶囊文案/宽度随断点变化，交给 Arrange 重排（宿主不重排时也不重叠）。
    if (GetClientSize().x > 0) {
        Arrange(GetClientSize().x);
    }
    Refresh(false);
}

void AgentTopBar::SetStatus(const StatusProjection& status)
{
    m_status = status;
    // 胶囊是绘制的，没有子窗口可以挂 tooltip：全文挂在顶栏上。
    SetToolTip(ComposeStatusLine(m_status));
    Refresh(false);
}

wxWindow* AgentTopBar::ModeWindow() const
{
    return m_modeSegment;
}

wxWindow* AgentTopBar::LevelWindow(int level)
{
    if (level < 1 || level > 4) {
        return nullptr;
    }
    return m_levels[level - 1];
}

wxWindow* AgentTopBar::DemoWindow() const
{
    return m_demoPill;
}

wxString AgentTopBar::BrandText() const
{
    return BrandPrimaryText() + " " + BrandSecondaryText();
}

wxString AgentTopBar::DemoText() const
{
    return m_narrow ? DemoCompactText() : DemoFullText();
}

wxString AgentTopBar::StatusText() const
{
    return ComposeStatusLine(m_status);
}

wxRect AgentTopBar::LightsRectPx() const
{
    return m_lightsRect;
}

wxRect AgentTopBar::StatusRectPx() const
{
    return m_statusRect;
}

wxRect AgentTopBar::BrandRectPx() const
{
    return m_brandRect;
}

int AgentTopBar::MinHeightPx() const
{
    // 2×上下内距 + 一行控件带（30 DIP，取自 .wt 的 30×28）与 H89 min-height:46 取大。
    const int byContent = 2 * Dip(this, S::BrandGap) + Dip(this, M::ToolButtonWidth);
    return std::max(Dip(this, M::TitleBarHeight), byContent);
}

void AgentTopBar::MeasureBrand(int* primaryWidth, int* secondaryWidth) const
{
    Measurer measure(this);
    const wxFont primaryFont = FontOf(this, T::BrandPx, tokens::Weight::Semibold);
    const wxFont secondaryFont = FontOf(this, T::LabelPx, tokens::Weight::Normal);
    if (primaryWidth != nullptr) {
        *primaryWidth = measure.Text(primaryFont, BrandPrimaryText()).x;
    }
    if (secondaryWidth != nullptr) {
        *secondaryWidth = measure.Text(secondaryFont, BrandSecondaryText()).x;
    }
}

void AgentTopBar::ApplyTheme()
{
    if (m_levels[0] != nullptr) {
        const wxFont levelFont = FontOf(this, T::SmallPx, tokens::Weight::Medium);
        for (auto* level : m_levels) {
            if (level != nullptr) {
                level->SetFont(levelFont);
            }
        }
    }
    // 字体变化会改变级别按钮与胶囊的最佳尺寸：按当前宽度重排一次。
    Arrange(GetClientSize().x);
    Refresh(false);
}

int AgentTopBar::Arrange(int width)
{
    const int totalWidth = std::max(0, width);
    if (totalWidth <= 0) {
        // 0 宽客户区：只保留最小高度，所有子窗口收成 0（不产生负尺寸）。
        m_height = MinHeightPx();
        if (m_modeSegment != nullptr) {
            m_modeSegment->SetSize(0, 0, 0, 0);
        }
        for (auto* level : m_levels) {
            if (level != nullptr) {
                level->SetSize(0, 0, 0, 0);
            }
        }
        if (m_demoPill != nullptr) {
            m_demoPill->SetSize(0, 0, 0, 0);
        }
        m_lightsRect = wxRect();
        m_brandRect = wxRect();
        m_statusRect = wxRect();
        m_dividerRect = wxRect();
        SetSize(0, 0, 0, m_height);
        return m_height;
    }

    const int padX = Dip(this, S::TopBarPadX);
    const int padY = Dip(this, S::BrandGap);  // H89 竖向居中带（8 DIP，无专用 token）
    const int gap = Dip(this, S::TopBarGap);
    const int rowGap = Dip(this, S::RowWrapGap);
    const int rightGap = Dip(this, S::TopBarRightGap);
    const int rowMin = Dip(this, M::ToolButtonWidth);  // 一行控件带 30 DIP（.wt 30×28）
    const int contentWidth = std::max(0, totalWidth - 2 * padX);
    const int contentRight = padX + contentWidth;

    m_narrow = totalWidth <= Dip(this, M::NarrowBreakpoint);

    const wxFont brandFont = FontOf(this, T::BrandPx, tokens::Weight::Semibold);
    const wxFont brandSubFont = FontOf(this, T::LabelPx, tokens::Weight::Normal);
    const wxFont captionFont = FontOf(this, T::SmallPx, tokens::Weight::Semibold);
    const wxFont levelFont = FontOf(this, T::SmallPx, tokens::Weight::Medium);
    const wxFont statusFont = FontOf(this, T::SmallBodyPx, tokens::Weight::Normal);

    Measurer measure(this);

    // ---- 各流量项的自然尺寸 ----
    const int lightSize = Dip(this, I::WindowLight);
    const int lightsWidth = 3 * lightSize + 2 * Dip(this, S::LightsGap);

    const wxSize brandPrimary = measure.Text(brandFont, BrandPrimaryText());
    const wxSize brandSecondary = measure.Text(brandSubFont, BrandSecondaryText());
    const int brandGap = Dip(this, S::BrandGap);
    const int brandWidth =
        brandPrimary.x + (m_narrow ? 0 : brandGap + brandSecondary.x);
    const int brandHeight = brandPrimary.y;

    const wxSize modeBest = m_modeSegment != nullptr ? m_modeSegment->GetBestSize() : wxSize(0, 0);

    const int dividerWidth = std::max(1, Dip(this, M::Hairline));
    const int dividerHeight = Dip(this, M::TopBarDividerHeight);

    const wxSize levelText = measure.Text(levelFont, U8("L1"));
    const int levelPadX = Dip(this, S::LevelPadX);
    const int levelPadY = Dip(this, S::LevelPadY);
    const int levelGap = Dip(this, S::LevelGap);
    int levelButtonWidth = levelText.x + 2 * levelPadX;
    const int levelButtonHeight =
        std::max(levelText.y + 2 * levelPadY, Dip(this, M::ToolButtonHeight));
    const int captionWidth = measure.Text(captionFont, LevelCaptionText()).x;

    int levelGroupWidth = captionWidth + levelGap + 4 * levelButtonWidth + 3 * levelGap;
    if (contentWidth > 0 && levelGroupWidth > contentWidth) {
        // 极窄：先把四个按钮压到最小可点宽度，再整体夹紧到整行（绝不越界）。
        const int room = std::max(
            0, contentWidth - captionWidth - levelGap - 3 * levelGap);
        levelButtonWidth = std::max(1, room / 4);
        levelGroupWidth =
            std::min(contentWidth, captionWidth + levelGap + 4 * levelButtonWidth + 3 * levelGap);
    }

    if (m_demoPill != nullptr) {
        m_demoPill->SetText(m_narrow ? DemoCompactText() : DemoFullText());
        m_demoPill->Show(m_demo);
    }
    const wxSize demoBest =
        (m_demoPill != nullptr && m_demo) ? m_demoPill->GetBestSize() : wxSize(0, 0);

    const wxString statusText = ComposeStatusLine(m_status);
    const int statusDot = Dip(this, I::DotMedium);
    const int statusPadX = Dip(this, S::StatusPadX);
    const int statusPadY = Dip(this, S::StatusPadY);
    const int dotGap = Dip(this, S::PillGap);
    const wxSize statusTextExtent = measure.Text(statusFont, statusText);
    const int statusHeight = 2 * statusPadY + std::max(statusTextExtent.y, statusDot);
    const int statusNatural =
        2 * statusPadX + statusDot + dotGap + statusTextExtent.x;
    const int statusMinText = measure.Text(statusFont, StatusLabel(vm::RunStatus::Unknown)).x;
    const int statusMin = std::min(std::max(0, contentWidth),
                                   2 * statusPadX + statusDot + dotGap + statusMinText);

    // ---- 固定项贪心换行（灯 → 品牌 → 模式 → 分隔线 → 提示级别组）----
    std::vector<FlowEntry> fixed;
    fixed.push_back(FlowEntry{0, lightsWidth, lightSize});
    fixed.push_back(FlowEntry{1, brandWidth, brandHeight});
    fixed.push_back(FlowEntry{2, modeBest.x, modeBest.y});
    fixed.push_back(FlowEntry{3, dividerWidth, dividerHeight});
    fixed.push_back(FlowEntry{4, levelGroupWidth, levelButtonHeight});

    std::vector<FlowRow> rows;
    std::vector<int> rowOfItem;
    std::vector<int> xOfItem;
    FlowPack(fixed, contentWidth, gap, &rows, &rowOfItem, &xOfItem);

    // ---- 演示标记 + 状态胶囊 ----
    const int demoWidth = std::min(std::max(0, contentWidth), demoBest.x);
    const int demoHeight = demoBest.y;
    const int lastRow = static_cast<int>(rows.size()) - 1;
    const int lastUsed = rows[static_cast<size_t>(lastRow)].usedWidth;
    const int separatorGap = lastUsed > 0 ? gap : 0;
    const int roomForGroup = contentWidth - lastUsed - separatorGap;
    const int groupWidthMin = demoWidth + (m_demo ? rightGap : 0) + statusMin;

    int demoRow = -1;
    int demoX = 0;
    int statusRow = -1;
    int statusX = 0;
    int statusWidth = 0;

    if (roomForGroup >= groupWidthMin && contentWidth > 0) {
        // 同一行：整组靠右；状态胶囊最后才被压缩，绝不压住左侧控件。
        int width = std::min(statusNatural, roomForGroup - demoWidth - (m_demo ? rightGap : 0));
        int groupTotal = demoWidth + (m_demo ? rightGap : 0) + width;
        int groupX = contentRight - groupTotal;
        const int minX = padX + lastUsed + separatorGap;
        if (groupX < minX) {
            groupX = minX;
            groupTotal = contentRight - groupX;
            width = std::max(0, groupTotal - demoWidth - (m_demo ? rightGap : 0));
        }
        demoRow = lastRow;
        demoX = groupX;
        statusRow = lastRow;
        statusX = groupX + demoWidth + (m_demo ? rightGap : 0);
        statusWidth = width;
        rows[static_cast<size_t>(lastRow)].usedWidth =
            std::max(lastUsed, statusX + statusWidth - padX);
        rows[static_cast<size_t>(lastRow)].height =
            std::max(rows[static_cast<size_t>(lastRow)].height,
                     std::max(demoHeight, statusHeight));
    } else {
        // 换新行：整组放得下就贴右（右组内 gap 10），放不下就左起（仍然互不重叠）。
        rows.push_back(FlowRow{});
        const int row = static_cast<int>(rows.size()) - 1;
        const int groupGap = m_demo ? rightGap : 0;
        statusWidth =
            std::min(statusNatural, std::max(0, contentWidth - demoWidth - groupGap));
        const int groupTotal = demoWidth + groupGap + statusWidth;
        const int groupX = groupTotal <= contentWidth ? contentWidth - groupTotal : 0;
        demoRow = row;
        demoX = padX + groupX;
        statusRow = row;
        statusX = demoX + demoWidth + groupGap;
        rows[static_cast<size_t>(row)].usedWidth = std::max(0, groupX + groupTotal);
        rows[static_cast<size_t>(row)].height = std::max(demoHeight, statusHeight);
    }

    // ---- 行高/纵向位置 ----
    std::vector<int> rowY(rows.size(), 0);
    int y = padY;
    for (size_t index = 0; index < rows.size(); ++index) {
        rows[index].height = std::max(rows[index].height, rowMin);
        rowY[index] = y;
        y += rows[index].height + rowGap;
    }
    const int stackedHeight = rows.empty() ? padY : (y - rowGap + padY);
    m_height = std::max(Dip(this, M::TitleBarHeight), stackedHeight);

    SetSize(0, 0, totalWidth, m_height);

    // ---- 落位（子窗口 + 绘制矩形）----
    const int boundWidth = totalWidth;
    const int boundHeight = m_height;

    const auto itemY = [&](int row, int itemHeight) {
        const int rowTop = rowY[static_cast<size_t>(std::max(0, row))];
        const int rowHeight = rows[static_cast<size_t>(std::max(0, row))].height;
        return rowTop + std::max(0, (rowHeight - itemHeight) / 2);
    };

    for (size_t index = 0; index < fixed.size(); ++index) {
        const int row = rowOfItem[index];
        const int itemWidth = std::min(fixed[index].width, contentWidth);
        const int x = padX + xOfItem[index];
        const int itemHeight = fixed[index].height;
        const int itemTop = itemY(row, itemHeight);
        switch (fixed[index].index) {
            case 0:  // 装饰灯
                m_lightsRect = ClampedRect(x, itemTop, itemWidth, lightSize, boundWidth,
                                           boundHeight);
                break;
            case 1:  // 品牌
                m_brandRect =
                    ClampedRect(x, itemTop, itemWidth, itemHeight, boundWidth, boundHeight);
                break;
            case 2:  // 模式分段
                PlaceInside(m_modeSegment, x, itemTop, itemWidth, itemHeight, boundWidth,
                            boundHeight);
                break;
            case 3:  // 竖分隔线
                m_dividerRect =
                    ClampedRect(x, itemTop, dividerWidth, dividerHeight, boundWidth, boundHeight);
                break;
            case 4: {  // 提示级别组：caption + 四个按钮
                const int buttonsX = x + captionWidth + levelGap;
                const int remaining = std::max(0, (x + itemWidth) - buttonsX);
                const int buttonWidth =
                    remaining >= 4 ? std::max(1, (remaining - 3 * levelGap) / 4) : 0;
                for (int level = 0; level < 4; ++level) {
                    if (m_levels[level] == nullptr) {
                        continue;
                    }
                    PlaceInside(m_levels[level], buttonsX + level * (buttonWidth + levelGap),
                                itemTop, buttonWidth, levelButtonHeight, boundWidth, boundHeight);
                }
                break;
            }
            default:
                break;
        }
    }

    if (m_demoPill != nullptr) {
        if (m_demo && demoRow >= 0) {
            PlaceInside(m_demoPill, demoX, itemY(demoRow, demoHeight), demoWidth, demoHeight,
                        boundWidth, boundHeight);
        } else {
            m_demoPill->SetSize(0, 0, 0, 0);
        }
    }

    if (statusRow >= 0) {
        m_statusRect = ClampedRect(statusX, itemY(statusRow, statusHeight), statusWidth,
                                   statusHeight, boundWidth, boundHeight);
    } else {
        m_statusRect = wxRect();
    }

    Refresh(false);
    return m_height;
}

void AgentTopBar::OnPaint(wxPaintEvent&)
{
    const wxColour strip =
        UiTheme::CompositeOver(A().cardBg, UiTheme::AgentOpacity::RailBottom, A().pageBg);

    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(strip));
    dc.Clear();

    const wxRect rect = GetClientRect();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }

    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateBrush(wxBrush(strip)));
    gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);

    // H89 border-bottom：1 DIP 发丝线（预合成，避免直接叠 alpha）。
    const int hair = std::max(1, Dip(this, M::Hairline));
    const wxColour hairColour = UiTheme::CompositeOver(A().hairline, 0.9, strip);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(hairColour).Width(hair)));
    gc->StrokeLine(0, std::max(0, rect.height - hair), rect.width,
                   std::max(0, rect.height - hair));

    // ① 装饰窗口灯（H90-92：12px，gap 8，red/amber/green）。纯绘制，无子窗口/无事件。
    if (m_lightsRect.width > 0 && m_lightsRect.height > 0) {
        const int size = Dip(this, I::WindowLight);
        const int step = size + Dip(this, S::LightsGap);
        const wxColour lights[3] = {A().danger, A().warn, A().brand};
        gc->SetPen(*wxTRANSPARENT_PEN);
        for (int index = 0; index < 3; ++index) {
            gc->SetBrush(gc->CreateBrush(wxBrush(lights[index])));
            gc->DrawEllipse(m_lightsRect.x + index * step, m_lightsRect.y, size, size);
        }
    }

    // ② 品牌：主标 13.5px/600 与副标 12px text3，基线近似为底对齐（wx 无 ascent API）。
    if (m_brandRect.width > 0 && m_brandRect.height > 0) {
        const wxFont primaryFont = FontOf(this, T::BrandPx, tokens::Weight::Semibold);
        const wxFont secondaryFont = FontOf(this, T::LabelPx, tokens::Weight::Normal);

        gc->SetFont(primaryFont, A().text1);
        const wxString primary = BrandPrimaryText();
        const double primaryWidth = TextWidth(*gc, primary);
        const double primaryHeight = TextHeight(*gc);
        const double primaryTop = m_brandRect.y;

        gc->SetFont(secondaryFont, A().text3);
        const wxString secondary = BrandSecondaryText();
        const double secondaryHeight = TextHeight(*gc);
        const double secondaryTop = primaryTop + std::max(0.0, primaryHeight - secondaryHeight);

        gc->SetFont(primaryFont, A().text1);
        gc->DrawText(primary, m_brandRect.x, primaryTop);
        if (!m_narrow) {
            gc->SetFont(secondaryFont, A().text3);
            gc->DrawText(secondary, m_brandRect.x + primaryWidth + Dip(this, S::BrandGap),
                         secondaryTop);
        }
    }

    // ③ 1×20 竖分隔线（H323 .tb-sep）
    if (m_dividerRect.width > 0 && m_dividerRect.height > 0) {
        gc->SetPen(gc->CreatePen(
            wxGraphicsPenInfo(tokens::BorderColour(BorderRole::Hairline)).Width(m_dividerRect.width)));
        gc->StrokeLine(m_dividerRect.x + m_dividerRect.width / 2.0, m_dividerRect.y,
                       m_dividerRect.x + m_dividerRect.width / 2.0,
                       m_dividerRect.y + m_dividerRect.height);
    }

    // ④ 级别标题「提示」：位置由 L1 按钮反推（同一行内始终紧跟其左，永不重叠）。
    if (m_levels[0] != nullptr && m_levels[0]->GetRect().width > 0) {
        const wxRect first = m_levels[0]->GetRect();
        gc->SetFont(FontOf(this, T::SmallPx, tokens::Weight::Semibold), A().text3);
        const wxString caption = LevelCaptionText();
        const double captionWidth = TextWidth(*gc, caption);
        const double captionHeight = TextHeight(*gc);
        const double captionX =
            std::max(static_cast<double>(rect.x), first.x - Dip(this, S::LevelGap) - captionWidth);
        const double captionY = first.y + std::max(0.0, (first.height - captionHeight) / 2.0);
        gc->DrawText(caption, captionX, captionY);
    }

    // ⑤ 状态胶囊（H101 .status）：白底 + 发丝边 + 7px 绿点 + 11.5px text2，超宽省略。
    if (m_statusRect.width > 0 && m_statusRect.height > 0) {
        const double radius = ClampRadius(std::min(m_statusRect.width, m_statusRect.height) / 2.0,
                                          m_statusRect);
        FillRound(*gc, m_statusRect, radius, A().cardBg);
        StrokeRound(*gc, m_statusRect, radius, tokens::BorderColour(BorderRole::Hairline),
                    Dip(this, M::Hairline));

        const int dot = Dip(this, I::DotMedium);
        const double dotX = m_statusRect.x + Dip(this, S::StatusPadX);
        const double dotY = m_statusRect.y + (m_statusRect.height - dot) / 2.0;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateBrush(wxBrush(A().brand)));
        gc->DrawEllipse(dotX, dotY, dot, dot);

        gc->SetFont(FontOf(this, T::SmallBodyPx, tokens::Weight::Normal), A().text2);
        const double textX = dotX + dot + Dip(this, S::PillGap);
        const double maxWidth =
            std::max(0.0, m_statusRect.GetRight() - Dip(this, S::StatusPadX) - textX);
        const wxString shown = EllipsizeToWidth(*gc, ComposeStatusLine(m_status), maxWidth);
        const double textHeight = TextHeight(*gc);
        gc->DrawText(shown, textX,
                     m_statusRect.y + std::max(0.0, (m_statusRect.height - textHeight) / 2.0));
    }
}

// ===========================================================================
// 6. AgentRail
// ===========================================================================

// 头文件里的私有前置声明 struct Entry 在这里落地：它复用同一个图标按钮实现，
// 只是把控件分成 rail 与 aw-head 两组（各自的 vector<Entry*> 由宿主持有）。
struct AgentRail::Entry final : public IconButton
{
    Entry(wxWindow* parent, const Item& item, bool primary, int widthDip, int heightDip,
          int iconDip)
        : IconButton(parent, item.action, item.icon, U8(item.label), U8(item.accessibleName),
                     primary, widthDip, heightDip, iconDip, M::ControlRadius, Backdrop::Rail)
    {
    }
};

namespace {

/// 竖排：logo + 12 按钮 + 1 条分隔 → 13 个间隙。
int VerticalNaturalHeightOf(const wxWindow* window)
{
    const int padY = Dip(window, S::RailPadY);
    const int gap = Dip(window, S::RailGap);
    const int logo = Dip(window, M::RailLogoSize);
    const int button = Dip(window, M::RailButton);
    const int separator = std::max(1, Dip(window, M::Hairline)) + 2 * Dip(window, S::RailSepMargin);
    constexpr int kItems = 14;  // logo + 12 按钮 + 1 分隔
    return 2 * padY + logo + 12 * button + separator + gap * (kItems - 1);
}

/// rail 按钮在横排里的边长：≤560 用 34（H357 .rb），否则 38。
int RailButtonDipFor(bool compact, int width, int narrowWidth, int buttonDip, int narrowDip)
{
    if (!compact) {
        return buttonDip;
    }
    return width > 0 && width <= narrowWidth ? narrowDip : buttonDip;
}

/// rail 的整条自绘。冻结头文件里 AgentRail 没有 OnPaint 成员，因此绘制体放在
/// 这里（ctor 里用 lambda 绑定 wxEVT_PAINT），所需状态由调用方显式传入。
void PaintRail(wxWindow& rail, bool compact, const wxWindow* before, const wxWindow* after)
{
    const wxRect rect = rail.GetClientRect();
    wxAutoBufferedPaintDC dc(&rail);
    dc.SetBackground(wxBrush(A().glassRailBottom));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }

    // H311 玻璃渐变（预合成实色；wx 侧不做真实模糊，SAFE-008）。
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateLinearGradientBrush(0.0, 0.0, 0.0,
                                               static_cast<double>(rect.height),
                                               A().glassRailTop, A().glassRailBottom));
    gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);

    // 边线：竖排右侧、横排底部的 1 DIP 发丝线。
    const int hair = std::max(1, Dip(&rail, M::Hairline));
    gc->SetPen(gc->CreatePen(
        wxGraphicsPenInfo(tokens::BorderColour(BorderRole::Hairline)).Width(hair)));
    if (compact) {
        gc->StrokeLine(0, std::max(0, rect.height - hair), rect.width,
                       std::max(0, rect.height - hair));
    } else {
        gc->StrokeLine(std::max(0, rect.width - hair), 0, std::max(0, rect.width - hair),
                       rect.height);
    }

    // H321 .rail-sep：newAsk 与 rGuide 之间的 24×1（横排为 1×24）。
    // 位置由相邻两钮的真实矩形反推，因此永远与按钮同排、不会与它们重叠。
    if (before != nullptr && after != nullptr) {
        const wxRect beforeRect = before->GetRect();
        const wxRect afterRect = after->GetRect();
        gc->SetPen(gc->CreatePen(
            wxGraphicsPenInfo(tokens::BorderColour(BorderRole::Hairline)).Width(hair)));
        if (compact) {
            const int separatorHeight = Dip(&rail, M::RailSeparatorWidth);
            const int x = (beforeRect.GetRight() + afterRect.x) / 2;
            const int top = beforeRect.y + std::max(0, (beforeRect.height - separatorHeight) / 2);
            gc->StrokeLine(x, top, x, top + separatorHeight);
        } else {
            const int separatorWidth = Dip(&rail, M::RailSeparatorWidth);
            const int y = (beforeRect.GetBottom() + afterRect.y) / 2;
            const int left = std::max(0, (rect.width - separatorWidth) / 2);
            gc->StrokeLine(left, y, left + separatorWidth, y);
        }
    }
}

}  // namespace

AgentRail::AgentRail(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetName("agent-rail");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(0, 0));

    auto* logo = new RailLogo(this);
    logo->SetLogoSizeDip(M::RailLogoSize);
    m_logo = logo;

    for (const Item& item : Catalog()) {
        const bool primary = item.group == Group::Primary;
        // 竖排尺寸先给足；横排时 Arrange 会重新按断点设置位置与大小。
        auto* entry = new Entry(this, item, primary, M::RailButton, M::RailButton, I::RailButton);
        entry->SetActionHandler([this](Action action) {
            // 处理器可能销毁 rail：先拷贝再调用。
            auto handler = m_handler;
            if (handler) {
                handler(action);
            }
        });
        m_entries.push_back(entry);
    }

    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        PaintRail(*this, m_compact, ButtonWindow(Action::NewAsk),
                  ButtonWindow(Action::ToggleGuide));
    });

    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
    ApplyTheme();
}

AgentRail::~AgentRail()
{
    m_theme.Reset();
    m_handler = nullptr;
    m_entries.clear();
}

void AgentRail::SetActionHandler(std::function<void(Action)> handler)
{
    m_handler = std::move(handler);
}

void AgentRail::ApplyTheme()
{
    // 按钮没有可见文字（图标 + tooltip），主题变化只需重排 + 重绘。
    Arrange(m_compact ? GetClientSize().x : PreferredWidthPx());
    Refresh(false);
}

int AgentRail::ButtonSizePx() const
{
    return RailButtonDipFor(m_compact, GetClientSize().x, Dip(this, M::NarrowBreakpoint),
                            Dip(this, M::RailButton), Dip(this, M::RailButtonNarrow));
}

int AgentRail::PreferredWidthPx() const
{
    return m_compact ? 0 : Dip(this, M::RailWidth);
}

bool AgentRail::PressedState(Action action) const
{
    const Item* item = Find(action);
    if (item == nullptr) {
        return false;
    }
    if (item->group == Group::Toggle) {
        return m_guidePressed;
    }
    if (item->group == Group::Form) {
        const AgentForm form = FormOf(action);
        return form != AgentForm::None && m_form == form;
    }
    return false;
}

void AgentRail::SetPressed(Action action, bool pressed)
{
    const Item* item = Find(action);
    if (item == nullptr) {
        return;
    }
    if (item->group == Group::Toggle) {
        m_guidePressed = pressed;
        for (Entry* entry : m_entries) {
            if (entry != nullptr && entry->GetAction() == action) {
                entry->SetPressedVisual(pressed);  // 只改视觉，不回调
            }
        }
    } else if (item->group == Group::Form) {
        SetFormPressed(pressed ? FormOf(action) : AgentForm::None);
    }
    // 其它分组（primary / jump）没有 pressed 语义：忽略。
}

bool AgentRail::IsPressed(Action action) const
{
    return PressedState(action);
}

void AgentRail::SetFormPressed(AgentForm form)
{
    m_form = form;
    for (Entry* entry : m_entries) {
        if (entry == nullptr) {
            continue;
        }
        const Item* item = Find(entry->GetAction());
        if (item == nullptr || item->group != Group::Form) {
            continue;
        }
        const AgentForm entryForm = FormOf(item->action);
        entry->SetPressedVisual(form != AgentForm::None && entryForm == form);
    }
}

void AgentRail::SetCompact(bool compact)
{
    if (m_compact == compact) {
        return;
    }
    m_compact = compact;
    if (m_logo != nullptr) {
        auto* logo = dynamic_cast<RailLogo*>(m_logo);
        if (logo != nullptr) {
            logo->SetLogoSizeDip(compact ? M::RailLogoSizeNarrow : M::RailLogoSize);
        }
    }
    // 切到横排时当前客户区宽度还是竖排的 58 DIP（不可信）：不拿它排版，
    // 由宿主随后的 Arrange(width) 决定换行；没有可信宽度就先保持原位。
    const int width = GetClientSize().x;
    if (!compact || width > Dip(this, M::RailWidth)) {
        Arrange(compact ? width : PreferredWidthPx());
    }
    Refresh(false);
}

wxWindow* AgentRail::ButtonWindow(Action action) const
{
    for (Entry* entry : m_entries) {
        if (entry != nullptr && entry->GetAction() == action) {
            return entry;
        }
    }
    return nullptr;
}

wxWindow* AgentRail::LogoWindow() const
{
    return m_logo;
}

bool AgentRail::IsButtonEnabled(Action action) const
{
    for (const Entry* entry : m_entries) {
        if (entry != nullptr && entry->GetAction() == action) {
            return entry->IsEnabled();
        }
    }
    return false;
}

wxString AgentRail::DisabledReason(Action action) const
{
    for (const Entry* entry : m_entries) {
        if (entry != nullptr && entry->GetAction() == action) {
            return entry->DisabledReason();
        }
    }
    return wxString();
}

void AgentRail::SetButtonEnabled(Action action, bool enabled, const wxString& reason)
{
    for (Entry* entry : m_entries) {
        if (entry != nullptr && entry->GetAction() == action) {
            entry->SetEnabledState(enabled, reason);
            return;
        }
    }
}

int AgentRail::Arrange(int width)
{
    const int totalWidth = m_compact ? std::max(0, width) : PreferredWidthPx();

    // ---- 竖排（H311）：固定 58 DIP 宽，高度 = 自然高度，多余空间全给弹性空白 ----
    if (!m_compact) {
        const int natural = VerticalNaturalHeightOf(this);
        // 宿主给的高度更小时不裁剪（页面滚动）；更大时把富余塞进 spacer。
        const int height = std::max(natural, GetClientSize().y);
        SetSize(0, 0, totalWidth, height);
        m_height = height;

        const int padY = Dip(this, S::RailPadY);
        const int gap = Dip(this, S::RailGap);
        const int button = Dip(this, M::RailButton);
        const int logoSize = Dip(this, M::RailLogoSize);
        const int separatorHeight =
            std::max(1, Dip(this, M::Hairline)) + 2 * Dip(this, S::RailSepMargin);
        const int spacer = std::max(0, height - natural);

        int y = padY;
        const auto centerX = [&](int itemWidth) {
            return std::max(0, (totalWidth - itemWidth) / 2);
        };

        PlaceInside(m_logo, centerX(logoSize), y, logoSize, logoSize, totalWidth, height);
        y += logoSize + gap;

        const auto placeEntry = [&](size_t index, int x, int top, int w, int h) {
            if (index < m_entries.size()) {
                m_entries[index]->SetIconSizeDip(I::RailButton);  // 竖排恒 18 DIP
                PlaceInside(m_entries[index], x, top, w, h, totalWidth, height);
            }
        };

        placeEntry(0, centerX(button), y, button, button);  // newAsk
        y += button + gap;

        // 分隔线：占据 separatorHeight，绘制位置由相邻两钮反推（见 OnPaint）。
        y += separatorHeight + gap;

        for (size_t index = 1; index <= 7; ++index) {  // 七个导航
            placeEntry(index, centerX(button), y, button, button);
            y += button + gap;
        }

        y += spacer;  // 弹性空白：把四个形态按钮推到底部

        for (size_t index = 8; index <= 11; ++index) {  // 四形态
            placeEntry(index, centerX(button), y, button, button);
            y += button + gap;
        }

        Refresh(false);
        return m_height;
    }

    // ---- 横排（H338/H356）：单行流式换行，no spacer，分隔线转 1×24 竖线 ----
    const int padX = Dip(this, S::RailPadY);   // H338 padding:7px 10px → 横排的左右内距
    const int padY = Dip(this, S::RailPadX);   // 同上：上下的 6~7 DIP 用 rail 的另一侧 token
    const int gap = Dip(this, S::ToolsGap);    // H356 gap:4px
    const int button = RailButtonDipFor(true, totalWidth, Dip(this, M::NarrowBreakpoint),
                                        Dip(this, M::RailButton), Dip(this, M::RailButtonNarrow));
    const int logoSize = Dip(this, M::RailLogoSizeNarrow);
    const int separatorWidth = std::max(1, Dip(this, M::Hairline));
    const int separatorHeight = Dip(this, M::RailSeparatorWidth);
    const int contentWidth = std::max(0, totalWidth - 2 * padX);

    // 流量项编码：index 0 = logo；-1 = 分隔线；1..12 = Catalog 下标 (index-1)。
    // 视觉顺序同 index.html：logo → newAsk → 分隔 → 七导航 → 四形态。
    std::vector<FlowEntry> items;
    items.push_back(FlowEntry{0, logoSize, logoSize});
    items.push_back(FlowEntry{1, button, button});  // newAsk
    items.push_back(FlowEntry{-1, separatorWidth, separatorHeight});
    for (int index = 2; index <= 12; ++index) {  // rGuide…qPet（Catalog 下标 1..11）
        items.push_back(FlowEntry{index, button, button});
    }

    std::vector<FlowRow> rows;
    std::vector<int> rowOfItem;
    std::vector<int> xOfItem;
    FlowPack(items, contentWidth, gap, &rows, &rowOfItem, &xOfItem);

    std::vector<int> rowY(rows.size(), 0);
    int y = padY;
    for (size_t index = 0; index < rows.size(); ++index) {
        rows[index].height = std::max(rows[index].height, button);
        rowY[index] = y;
        y += rows[index].height + gap;
    }
    m_height = rows.empty() ? padY * 2 : (y - gap + padY);

    SetSize(0, 0, totalWidth, m_height);

    const auto itemTop = [&](int row, int itemHeight) {
        const int rowTop = rowY[static_cast<size_t>(std::max(0, row))];
        const int rowHeight = rows[static_cast<size_t>(std::max(0, row))].height;
        return rowTop + std::max(0, (rowHeight - itemHeight) / 2);
    };

    for (size_t index = 0; index < items.size(); ++index) {
        const int row = rowOfItem[index];
        const int x = padX + xOfItem[index];
        const int itemHeight = items[index].height;
        const int itemTopY = itemTop(row, itemHeight);
        const int kind = items[index].index;
        if (kind == 0) {  // logo
            PlaceInside(m_logo, x, itemTopY, logoSize, logoSize, totalWidth, m_height);
        } else if (kind < 0) {  // 1×24 竖分隔线（绘制，见 OnPaint）
            continue;
        } else if (static_cast<size_t>(kind - 1) < m_entries.size()) {
            // 34 px 按钮配 16 DIP 图标（H357 的视觉补偿），38 px 配 18 DIP。
            const int iconDip = button <= Dip(this, M::RailButtonNarrow) ? I::Composer
                                                                        : I::RailButton;
            m_entries[static_cast<size_t>(kind - 1)]->SetIconSizeDip(iconDip);
            PlaceInside(m_entries[static_cast<size_t>(kind - 1)], x, itemTopY, button, button,
                        totalWidth, m_height);
        }
    }

    Refresh(false);
    return m_height;
}

// ===========================================================================
// 7. AgentFormSwitch（.aw-tools，30×28 图标按钮 ×4）
// ===========================================================================

struct AgentFormSwitch::Entry final : public IconButton
{
    Entry(wxWindow* parent, const Item& item)
        : IconButton(parent, item.action, item.icon, U8(item.label), U8(item.accessibleName),
                     /*primary*/ false, M::ToolButtonWidth, M::ToolButtonHeight, I::HeadTool,
                     M::ControlRadius, Backdrop::Card)
    {
    }
};

AgentFormSwitch::AgentFormSwitch(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetName("agent-form-switch");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(0, 0));

    for (const Item& item : Catalog()) {
        if (item.group != Group::Form) {
            continue;
        }
        auto* entry = new Entry(this, item);
        entry->SetActionHandler([this](Action action) {
            auto handler = m_handler;
            if (handler) {
                handler(action);
            }
        });
        m_entries.push_back(entry);
    }

    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        // 容器底色必须与对话卡片面一致，否则圆角卡片上会出现第二种底色。
        wxAutoBufferedPaintDC dc(this);
        const wxColour card = tokens::SurfaceColour(Component::Card);
        dc.SetBackground(wxBrush(card));
        dc.Clear();
        const wxRect rect = GetClientRect();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || rect.width <= 0 || rect.height <= 0) {
            return;
        }
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateBrush(wxBrush(card)));
        gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);
    });

    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
    ApplyTheme();
}

AgentFormSwitch::~AgentFormSwitch()
{
    m_theme.Reset();
    m_handler = nullptr;
    m_entries.clear();
}

void AgentFormSwitch::SetActionHandler(std::function<void(Action)> handler)
{
    m_handler = std::move(handler);
}

void AgentFormSwitch::ApplyTheme()
{
    if (GetParent() != nullptr) {
        Arrange(GetPosition().x, GetPosition().y);
    }
    Refresh(false);
}

void AgentFormSwitch::SetFormSelection(AgentForm form)
{
    m_form = form;
    for (Entry* entry : m_entries) {
        if (entry == nullptr) {
            continue;
        }
        entry->SetPressedVisual(form != AgentForm::None && FormOf(entry->GetAction()) == form);
    }
}

int AgentFormSwitch::PreferredWidthPx() const
{
    // H200 gap:4px + H201 .wt 30×28
    return 4 * Dip(this, M::ToolButtonWidth) + 3 * Dip(this, S::ToolsGap);
}

int AgentFormSwitch::PreferredHeightPx() const
{
    return Dip(this, M::ToolButtonHeight);
}

int AgentFormSwitch::Arrange(int x, int y)
{
    const int width = PreferredWidthPx();
    const int height = PreferredHeightPx();
    SetSize(x, y, width, height);

    const int gap = Dip(this, S::ToolsGap);
    const int buttonWidth = Dip(this, M::ToolButtonWidth);
    int buttonX = 0;
    for (Entry* entry : m_entries) {
        if (entry == nullptr) {
            continue;
        }
        PlaceInside(entry, buttonX, 0, buttonWidth, height, width, height);
        buttonX += buttonWidth + gap;
    }
    Refresh(false);
    return width;
}

wxWindow* AgentFormSwitch::ButtonWindow(Action action) const
{
    for (Entry* entry : m_entries) {
        if (entry != nullptr && entry->GetAction() == action) {
            return entry;
        }
    }
    return nullptr;
}

// ===========================================================================
// 8. AgentConversationHeader（.aw-head）
// ===========================================================================

namespace {

wxString HeaderTitleText()
{
    return U8("芯语");
}

wxString UnknownModeTagText()
{
    return U8("回答模式未知");
}

}  // namespace

AgentConversationHeader::AgentConversationHeader(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetName("agent-conversation-header");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(0, 0));

    m_forms = new AgentFormSwitch(this);
    Bind(wxEVT_PAINT, &AgentConversationHeader::OnPaint, this);

    // 冻结的头部**没有** UiTheme::Subscription 成员，也没有动作回调成员：
    //   * 主题变化时本控件全部颜色在 OnPaint 里现取 token，字体也现算，因此无需订阅；
    //   * 动作回调直接转交 m_forms（形态唯一真源仍是协调器，本控件不缓存 handler）。
    ApplyTheme();
}

AgentConversationHeader::~AgentConversationHeader() = default;

void AgentConversationHeader::SetActionHandler(std::function<void(Action)> handler)
{
    if (m_forms != nullptr) {
        m_forms->SetActionHandler(std::move(handler));
    }
}

void AgentConversationHeader::SetFormSelection(AgentForm form)
{
    if (m_forms != nullptr) {
        m_forms->SetFormSelection(form);
    }
}

AgentForm AgentConversationHeader::CurrentForm() const
{
    return m_forms != nullptr ? m_forms->CurrentForm() : AgentForm::None;
}

void AgentConversationHeader::SetModeTag(const wxString& tag)
{
    m_tag = tag;
    // 尚无客户区宽度（构造后未排布）时先按「有标签」记录；真实排布时由 Arrange
    // 按可用宽度决定是否画得下（放不下就整体不画，绝不与四钮块重叠）。
    m_tagShown = !m_tag.empty();
    if (GetClientSize().x > 0) {
        Arrange(GetPosition().x, GetPosition().y, GetClientSize().x);
    }
    Refresh(false);
}

wxString AgentConversationHeader::ModeTag() const
{
    // T09 没有可信回答模式来源（T12/T22 才投影）：空标签显式「未知」，
    // 绝不写死原型 H167 的演示串「深答 · 思考链」。
    return m_tag.empty() ? UnknownModeTagText() : m_tag;
}

wxString AgentConversationHeader::TitleText() const
{
    return HeaderTitleText();
}

void AgentConversationHeader::ApplyTheme()
{
    // 无条件重排一次：构造期客户区宽度可能是 0，至少也要给出正确的高度。
    Arrange(GetPosition().x, GetPosition().y, GetClientSize().x);
    Refresh(false);
}

int AgentConversationHeader::Arrange(int x, int y, int width)
{
    const int totalWidth = std::max(0, width);
    const int padX = Dip(this, S::ComposePadX);       // H193 padding:11px 14px
    const int padY = Dip(this, S::PaneHeadPadY);
    const int formsWidth = m_forms != nullptr ? m_forms->PreferredWidthPx() : 0;
    const int formsHeight = m_forms != nullptr ? m_forms->PreferredHeightPx() : 0;

    Measurer measure(this);
    const wxFont titleFont = FontOf(this, T::BrandPx, tokens::Weight::Semibold);
    const wxFont tagFont = FontOf(this, T::TinyPx, tokens::Weight::Semibold);
    const wxSize titleExtent = measure.Text(titleFont, HeaderTitleText());
    const wxSize tagExtent = measure.Text(tagFont, ModeTag());

    const int tagPadX = Dip(this, S::TagPadX);
    const int tagPadY = Dip(this, S::TagPadY);
    const int tagWidth = tagExtent.x + 2 * tagPadX;
    const int tagHeight = tagExtent.y + 2 * tagPadY;

    // 头部高度：min 44（H193/H349）与「四钮 + 2×11 内距」「标题行 + 2×11」取大。
    m_height = std::max(Dip(this, M::HeaderHeight),
                        std::max(formsHeight + 2 * padY, titleExtent.y + 2 * padY));
    SetSize(x, y, totalWidth, m_height);

    // 四个形态按钮钉右：右侧留 14（= H193 .aw-head padding:11px 14px，与 Arrange 段
    // 的「14 right padding」一致；类注释里的「4 DIP from right edge」实为 .aw-tools
    // 的按钮间 gap，不改变外缘内距）。宽度不足时贴住左侧，不越界。
    const int formsX = std::max(0, std::min(totalWidth - padX - formsWidth,
                                           totalWidth - formsWidth));
    const int formsY = std::max(0, (m_height - formsHeight) / 2);
    if (m_forms != nullptr) {
        m_forms->Arrange(formsX, formsY);
    }

    // 标题：可用宽度到四钮块左侧为止，永远不与其重叠。
    const int titleLimit = std::max(0, formsX - padX - Dip(this, S::HeadGap));
    const int titleWidth = std::min(titleExtent.x, titleLimit);
    const int titleHeight = std::min(titleExtent.y, m_height);
    m_titleRect = ClampedRect(padX, std::max(0, (m_height - titleHeight) / 2), titleWidth,
                              titleHeight, totalWidth, m_height);

    // 模式标签：标题之后、四钮块之前；放不下就整体不画（不重叠、不压缩标题）。
    m_tagShown = false;
    m_tagRect = wxRect();
    if (!m_tag.empty()) {
        const int tagX = m_titleRect.x + m_titleRect.width + Dip(this, kIdTagGapDip);
        const int tagLimit = formsX - Dip(this, S::HeadGap);
        if (tagWidth > 0 && tagX + tagWidth <= tagLimit && tagHeight <= m_height) {
            m_tagShown = true;
            m_tagRect = ClampedRect(tagX, std::max(0, (m_height - tagHeight) / 2), tagWidth,
                                    tagHeight, totalWidth, m_height);
        }
    }

    Refresh(false);
    return m_height;
}

void AgentConversationHeader::OnPaint(wxPaintEvent&)
{
    const wxRect rect = GetClientRect();
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(tokens::SurfaceColour(Component::Card)));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateBrush(wxBrush(tokens::SurfaceColour(Component::Card))));
    gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);

    // 标题「芯语」13.5px/600 text1
    if (m_titleRect.width > 0 && m_titleRect.height > 0) {
        gc->SetFont(FontOf(this, T::BrandPx, tokens::Weight::Semibold), A().text1);
        const wxString title = HeaderTitleText();
        const double titleHeight = TextHeight(*gc);
        const wxString shown = EllipsizeToWidth(*gc, title, m_titleRect.width);
        gc->DrawText(shown, m_titleRect.x,
                     m_titleRect.y + std::max(0.0, (m_titleRect.height - titleHeight) / 2.0));
    }

    // 回答模式标签（H199 .tag）：accentText on accentSoft，pill 圆角，padding 2/8。
    if (m_tagShown && m_tagRect.width > 0 && m_tagRect.height > 0) {
        const double radius = ClampRadius(
            std::min(m_tagRect.width, m_tagRect.height) / 2.0, m_tagRect);
        FillRound(*gc, m_tagRect, radius, A().accentSoft);
        gc->SetFont(FontOf(this, T::TinyPx, tokens::Weight::Semibold), A().accentText);
        const wxString shown =
            EllipsizeToWidth(*gc, ModeTag(), std::max(0.0, m_tagRect.width - 2.0 * Dip(this, S::TagPadX)));
        const double textHeight = TextHeight(*gc);
        const double textWidth = TextWidth(*gc, shown);
        gc->DrawText(shown,
                     m_tagRect.x + std::max(0.0, (m_tagRect.width - textWidth) / 2.0),
                     m_tagRect.y + std::max(0.0, (m_tagRect.height - textHeight) / 2.0));
    }

    // H193 border-bottom：1 DIP 发丝线
    const int hair = std::max(1, Dip(this, M::Hairline));
    gc->SetPen(gc->CreatePen(
        wxGraphicsPenInfo(tokens::BorderColour(BorderRole::Hairline)).Width(hair)));
    gc->StrokeLine(0, std::max(0, rect.height - hair), rect.width,
                   std::max(0, rect.height - hair));
}

// ===========================================================================
// 9. AgentStatusStrip（.aw-status）
// ===========================================================================

AgentStatusStrip::AgentStatusStrip(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetName("agent-status-strip");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(0, 0));

    Bind(wxEVT_PAINT, &AgentStatusStrip::OnPaint, this);
    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
    Arrange(0);
}

AgentStatusStrip::~AgentStatusStrip()
{
    m_theme.Reset();
}

void AgentStatusStrip::SetSegments(const std::vector<wxString>& segments)
{
    m_segments = segments;
    // 全文保留（含省略号之外的部分），tooltip 与 Text() 都不裁剪。
    SetToolTip(Text());
    Refresh(false);
}

wxString AgentStatusStrip::Text() const
{
    wxString text;
    for (size_t index = 0; index < m_segments.size(); ++index) {
        if (index > 0) {
            text += U8(" │ ");
        }
        text += m_segments[index];
    }
    return text;
}

int AgentStatusStrip::Arrange(int width)
{
    const int totalWidth = std::max(0, width);
    const int padY = Dip(this, S::StatusStripPadY);  // H205 padding:6px 14px
    const int lineHeight = tokens::LineHeightDip(this, T::TinyPx, T::LineHeightHint);
    // 2×6 + 10.5px/1.55 约 28 DIP，天然高于 spec §4.5 的 24 DIP 下限，无需再加常量。
    m_height = std::max(0, 2 * padY + lineHeight);
    SetSize(0, 0, totalWidth, m_height);
    Refresh(false);
    return m_height;
}

void AgentStatusStrip::ApplyTheme()
{
    Arrange(GetClientSize().x);
    Refresh(false);
}

void AgentStatusStrip::OnPaint(wxPaintEvent&)
{
    const wxRect rect = GetClientRect();
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(A().sunkenSoft));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateBrush(wxBrush(A().sunkenSoft)));
    gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);

    const int hair = std::max(1, Dip(this, M::Hairline));
    gc->SetPen(gc->CreatePen(
        wxGraphicsPenInfo(tokens::BorderColour(BorderRole::Hairline)).Width(hair)));
    gc->StrokeLine(0, std::max(0, rect.height - hair), rect.width,
                   std::max(0, rect.height - hair));

    if (m_segments.empty()) {
        return;
    }

    const int padX = Dip(this, S::StatusStripPadX);
    const int dot = Dip(this, I::DotSmall);
    const double dotGap = Dip(this, S::PillGap);
    const int separatorWidth = 2 * Dip(this, S::PillGap) + std::max(1, Dip(this, M::Hairline));
    const int separatorHeight = std::min(Dip(this, kStatusSepDip),
                                         std::max(0, rect.height - 2 * hair));

    const double dotX = static_cast<double>(rect.x + padX);
    const double dotY = rect.y + (rect.height - dot) / 2.0;
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateBrush(wxBrush(A().brand)));
    gc->DrawEllipse(dotX, dotY, dot, dot);

    gc->SetFont(FontOf(this, T::TinyPx, tokens::Weight::Normal), A().text3);
    const double right = static_cast<double>(rect.GetRight() - padX);
    double x = dotX + dot + dotGap;
    const double textHeight = TextHeight(*gc);
    const double textY = rect.y + (rect.height - textHeight) / 2.0;

    for (size_t index = 0; index < m_segments.size(); ++index) {
        const double available = std::max(0.0, right - x);
        if (available <= 0.0) {
            break;
        }
        const wxString& segment = m_segments[index];
        const double need = TextWidth(*gc, segment);
        const bool last = index + 1 >= m_segments.size();
        const double reservedForSeparator = last ? 0.0 : separatorWidth;
        const wxString shown =
            need + reservedForSeparator <= available
                ? segment
                : EllipsizeToWidth(*gc, segment,
                                   std::max(0.0, available - reservedForSeparator));
        if (shown.empty() && !segment.empty()) {
            break;
        }
        gc->DrawText(shown, x, textY);
        x += TextWidth(*gc, shown);
        const bool truncated = x < right + 0.5 && TextWidth(*gc, shown) < need;
        if (last || truncated) {
            // 已经省略：后面的分段放不下，不再硬塞（Text() 仍返回全文）。
            break;
        }
        // H206 .sep：1×10 发丝竖线，居中于两个分段之间
        const double lineX = x + separatorWidth / 2.0;
        if (lineX <= right) {
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(tokens::BorderColour(BorderRole::Hairline))
                                         .Width(std::max(1, Dip(this, M::Hairline)))));
            gc->StrokeLine(lineX, rect.y + (rect.height - separatorHeight) / 2.0, lineX,
                           rect.y + (rect.height + separatorHeight) / 2.0);
            gc->SetPen(*wxTRANSPARENT_PEN);
        }
        x += separatorWidth;
    }
}

}  // namespace agent::gui::nav
