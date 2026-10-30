//
// AgentControls.cpp — AGUI-T05 可复用原生控件与圆角内容宿主（实现）
//
// 本文件不得出现 wxColour(r,g,b) / 0xRRGGBB / 井号十六进制 之类的色值字面量；
// 颜色一律来自 agent::gui::tokens（唯一真源 UiTheme 的 Agent 扩展）。
// 绘制一律 wxAutoBufferedPaintDC + wxGraphicsContext；事件处理只改状态 + Refresh。
//
#include "agent/gui/AgentControls.h"

#include <wx/access.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/pen.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace agent::gui::controls {

wxDEFINE_EVENT(EVT_AGENT_SEGMENT, wxCommandEvent);
wxDEFINE_EVENT(EVT_AGENT_ACCORDION, wxCommandEvent);

namespace {

// Token 门面的短名（仍是单一来源，不引入任何本地常量）
using tokens::BorderRole;
using tokens::Component;
using tokens::ComponentTokens;

namespace M = UiTheme::AgentMetrics;
namespace S = UiTheme::AgentSpacing;

const UiTheme::AgentColours& A()
{
    return tokens::Colours();
}

const ComponentTokens& Tok(Component component)
{
    return tokens::Tokens(component);
}

int Dip(const wxWindow* window, int dip)
{
    return tokens::CssPxToDip(window, dip);
}

wxFont FontOf(const wxWindow* window, const ComponentTokens& t, tokens::Weight weight)
{
    return tokens::UiFontPx(window, t.fontSizePx, weight);
}

// ---------------------------------------------------------------------------
// 圆角/环形图元
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

/// 1 DIP 发丝边线：路径内缩半个线宽，避免描边被客户区裁掉一半。
void StrokeRound(wxGraphicsContext& gc, const wxRect& rect, double radius,
                 const wxColour& colour, int widthPx)
{
    const int width = std::max(1, widthPx);
    if (rect.width <= width || rect.height <= width) {
        return;
    }
    const double half = width / 2.0;
    wxGraphicsPath path = gc.CreatePath();
    path.AddRoundedRectangle(rect.x + half, rect.y + half, rect.width - width,
                             rect.height - width,
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
    path.AddRoundedRectangle(rect.x + half, rect.y + half, rect.width - width,
                             rect.height - width,
                             std::max(0.0, ClampRadius(radius, rect) - half));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(width)));
    gc.StrokePath(path);
}

/// Agent 焦点环（原型 H85 outline 2px accent + offset 2px；H257 0 0 0 3px accent-soft）。
/// 只画在控件内部：wx 子窗口无法把描边画到自身客户区之外，故 2 DIP offset 表现为内缩。
/// ringColour 由调用方给出（Primary 渐变底上 accent 环不可见，改用 textOnAccent）。
void DrawFocusRing(wxGraphicsContext& gc, const wxWindow* window, const wxRect& area,
                   const ComponentTokens& tokens, double radius, const wxColour& ringColour)
{
    const int ring = std::max(1, Dip(window, tokens.focusRingDip));
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

double TextHeight(wxGraphicsContext& gc)
{
    double w = 0.0;
    double h = 0.0;
    gc.GetTextExtent(wxString::FromUTF8("Ag"), &w, &h);
    return h;
}

double TextWidth(wxGraphicsContext& gc, const wxString& text)
{
    double w = 0.0;
    double h = 0.0;
    gc.GetTextExtent(text, &w, &h);
    return w;
}

// 与 OnPaint 使用同一测量器，避免 GDI 与 GDI+ 的字宽差让自然尺寸下的中文也被省略。
wxSize MeasuredText(const wxWindow* window, const wxString& text)
{
    wxClientDC dc(const_cast<wxWindow*>(window));
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) {
        return const_cast<wxWindow*>(window)->GetTextExtent(text);
    }
    gc->SetFont(window->GetFont(), A().text1);
    double width = 0.0, height = 0.0;
    gc->GetTextExtent(text, &width, &height);
    return wxSize(static_cast<int>(std::ceil(width)) + 1, static_cast<int>(std::ceil(height)));
}

/// 单行省略（U+2026）。超出可用宽度时逐字符回收，保证不越过右边界。
/// 说明：原型 .kv .v 用 overflow-wrap:anywhere 换行；本控件受「内容窗格高度由宿主
/// 决定，换行会把文字挤出卡片」的限制，改为单行省略（已作为偏差记录）。
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

/// 内容窗口的「自然高度」：优先 best size，其次 min size / 当前尺寸，最后按一行兜底。
int ContentBestHeight(wxWindow* content)
{
    if (content == nullptr) {
        return 0;
    }
    int height = content->GetBestSize().y;
    if (height < 1) {
        height = content->GetMinSize().y;
    }
    if (height < 1) {
        height = content->GetSize().y;
    }
    if (height < 1) {
        height = content->GetCharHeight() + content->FromDIP(8);
    }
    return std::max(0, height);
}

}  // namespace

// ===========================================================================
// 可访问性基底（仅设置名称/角色/状态/帮助文本；未在 Windows 读屏上实测）
// ===========================================================================
#if wxUSE_ACCESSIBILITY
namespace {

struct AccessInfo
{
    wxString name;
    wxString help;
    wxString description;
    wxString value;
    wxAccRole role = wxROLE_SYSTEM_PUSHBUTTON;
    bool focusable = false;
    bool focused = false;
    bool unavailable = false;
    bool checked = false;
    int expandedState = 0;  ///< 0 不表态；1 展开；2 收起
};

/// 所有控件共用的 wxAccessible 实现：数据由控件在查询时现取（provider 回调），
/// 因此状态变化不需要重新挂载。
class AgentAccessible final : public wxAccessible
{
public:
    using Provider = std::function<AccessInfo()>;

    explicit AgentAccessible(wxWindow* window, Provider provider)
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
        if (info.expandedState == 1) {
            *state |= wxACC_STATE_SYSTEM_EXPANDED;
        } else if (info.expandedState == 2) {
            *state |= wxACC_STATE_SYSTEM_COLLAPSED;
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

    wxAccStatus GetDescription(int childId, wxString* text) override
    {
        if (childId != wxACC_SELF || text == nullptr) {
            return wxACC_NOT_IMPLEMENTED;
        }
        *text = m_provider().description;
        return wxACC_OK;
    }

    wxAccStatus GetValue(int childId, wxString* value) override
    {
        if (childId != wxACC_SELF || value == nullptr) {
            return wxACC_NOT_IMPLEMENTED;
        }
        *value = m_provider().value;
        return wxACC_OK;
    }

private:
    Provider m_provider;
};

}  // namespace
#endif  // wxUSE_ACCESSIBILITY

// ===========================================================================
// AgentButton
// ===========================================================================

AgentButton::AgentButton(wxWindow* parent, wxWindowID id, const wxString& label,
                         Variant variant)
    // 刻意不用 wxWANTS_CHARS：它会把 Tab / Enter 变成字符事件，破坏整窗 Tab 遍历。
    // 方向/空格/回车都能以 wxEVT_KEY_DOWN 正常到达，见 OnKeyDown。
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      m_variant(variant)
{
    m_accessibleName = label;
    SetLabel(label);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFont(FontOf(this, Tok(Component::Button), Tok(Component::Button).weight));
    ApplyAccessibility();

    Bind(wxEVT_PAINT, &AgentButton::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &AgentButton::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &AgentButton::OnLeftUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &AgentButton::OnCaptureLost, this);
    Bind(wxEVT_ENTER_WINDOW, &AgentButton::OnEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &AgentButton::OnLeave, this);
    Bind(wxEVT_KEY_DOWN, &AgentButton::OnKeyDown, this);
    Bind(wxEVT_KEY_UP, &AgentButton::OnKeyUp, this);
    Bind(wxEVT_SET_FOCUS, &AgentButton::OnFocus, this);
    Bind(wxEVT_KILL_FOCUS, &AgentButton::OnKillFocus, this);

    m_theme = tokens::SubscribeTheme([this] {
        ApplyAccessibility();
        UpdateVisualState();
    });
}

AgentButton::~AgentButton()
{
    // 析构时不得留下鼠标捕获（否则整窗收不到后续鼠标消息）
    if (HasCapture()) {
        ReleaseMouse();
    }
}

void AgentButton::SetVariant(Variant variant)
{
    if (m_variant == variant) {
        return;
    }
    m_variant = variant;
    InvalidateBestSize();
    Refresh(false);
}

void AgentButton::SetLabel(const wxString& label)
{
    m_label = label;
    wxControl::SetLabel(label);
    if (m_accessibleName.empty() || m_accessibleName == GetName()) {
        m_accessibleName = label;
    }
    ApplyAccessibility();
    InvalidateBestSize();
    Refresh(false);
}

void AgentButton::SetSelected(bool selected)
{
    if (m_selected == selected) {
        return;
    }
    m_selected = selected;
    Refresh(false);
}

void AgentButton::SetIcon(icons::IconId icon, int sizeDip)
{
    m_icon = icon;
    m_iconDip = std::max(0, sizeDip);
    m_hasIcon = sizeDip > 0;
    InvalidateBestSize();
    Refresh(false);
}

void AgentButton::SetDisabledReason(const wxString& reason)
{
    m_disabledReason = reason;
    if (!reason.empty()) {
        // 非空原因 = 该按钮此刻不可用（DEC-05 的能力门禁都带原因）。
        Enable(false);
    }
    ApplyAccessibility();
    Refresh(false);
}

void AgentButton::SetAccessibleName(const wxString& name)
{
    m_accessibleName = name;
    ApplyAccessibility();
}

bool AgentButton::Enable(bool enable)
{
    if (!enable) {
        ResetPressState();
        if (HasCapture()) {
            ReleaseMouse();
        }
    }
    const bool changed = wxControl::Enable(enable);
    if (changed) {
        ApplyAccessibility();
        Refresh(false);
    }
    return changed;
}

AgentButton::State AgentButton::GetState() const
{
    if (!IsEnabled()) {
        return State::Disabled;
    }
    if (m_mousePressed || m_keyPressed) {
        return State::Pressed;
    }
    if (m_selected) {
        return State::Selected;
    }
    if (m_hover) {
        return State::Hover;
    }
    return State::Normal;
}

bool AgentButton::IsFocusVisible() const
{
    return HasFocus() && m_focusTracker.KeyboardFocused();
}

wxSize AgentButton::DoGetBestSize() const
{
    const ComponentTokens& t = Tok(Component::Button);
    const int padX = Dip(this, t.paddingXDip);
    const int padY = Dip(this, t.paddingYDip);
    const int minHeight = Dip(this, t.minHeightDip);
    const int minWidth = Dip(this, M::RailButton);
    if (m_hasIcon) {
        const int iconSize = Dip(this, m_iconDip);
        return wxSize(std::max(minWidth, iconSize + 2 * padX),
                      std::max(minHeight, iconSize + 2 * padY));
    }
    const wxSize measured = MeasuredText(this, m_label);
    const int textW = measured.x;
    const int textH = measured.y;

    const int width = std::max(minWidth, textW + 2 * padX);
    const int height = std::max(minHeight, textH + 2 * padY);
    return wxSize(width, height);
}

wxColour AgentButton::CornerSurface() const
{
    // 圆角外角只能填「父层近似色」：wx 子窗口没有透明背景（THE-021），
    // 这里统一取 Agent 卡片玻璃面，避免出现黑角或方角色块。
    return tokens::SurfaceColour(Component::Card);
}

wxColour AgentButton::StateSurface() const
{
    const bool primary = m_variant == Variant::Primary;
    if (!IsEnabled()) {
        if (primary) {
            return A().sunkenSoft;
        }
        return m_variant == Variant::Ghost ? A().cardGlass
                                           : tokens::SurfaceColour(Component::Button);
    }
    if (m_mousePressed || m_keyPressed) {
        return primary ? A().ctaBottom : A().sunken;
    }
    if (m_selected) {
        return A().accentSoft;
    }
    if (m_hover) {
        if (primary) {
            return A().ctaTop;
        }
        return m_variant == Variant::Ghost ? A().cardGlass : A().accentSoft;
    }
    if (primary) {
        return A().ctaBottom;
    }
    return m_variant == Variant::Ghost ? A().cardGlass
                                       : tokens::SurfaceColour(Component::Button);
}

wxColour AgentButton::StateText() const
{
    if (!IsEnabled()) {
        return A().text3;
    }
    if (m_variant == Variant::Primary) {
        return A().textOnAccent;
    }
    if (m_selected || m_hover || m_mousePressed || m_keyPressed) {
        return A().accentText;
    }
    return A().text2;
}

bool AgentButton::StateHasBorder() const
{
    if (m_variant == Variant::Primary) {
        return false;  // H319 .rb.primary border-color:transparent
    }
    if (!IsEnabled()) {
        return true;
    }
    if (m_selected) {
        return true;  // VIS-007：选中边框有设计依据（H252 accent .40）
    }
    if (m_variant == Variant::Ghost) {
        return m_hover;  // H182 .act.ghost 常态无边框
    }
    return true;
}

void AgentButton::ApplyAccessibility()
{
    const wxString name = m_accessibleName.empty() ? m_label : m_accessibleName;
    SetName(name);
    SetToolTip(m_disabledReason);
#if wxUSE_ACCESSIBILITY
    // 只在首次挂载：provider 每次查询都现取状态，不需要重建 wxAccessible
    if (GetAccessible() == nullptr) {
        SetAccessible(new AgentAccessible(this, [this] {
            AccessInfo info;
            info.name = AccessibleName().empty() ? m_label : AccessibleName();
            info.help = m_disabledReason;
            info.description = m_disabledReason.empty() ? wxString() : m_disabledReason;
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

void AgentButton::UpdateVisualState()
{
    Refresh(false);
}

void AgentButton::ResetPressState()
{
    m_mousePressed = false;
    m_keyPressed = false;
    m_spaceDown = false;
    m_enterDown = false;
}

void AgentButton::Activate()
{
    if (!IsEnabled()) {
        return;
    }
    ++m_activationCount;
    wxCommandEvent event(wxEVT_BUTTON, GetId());
    event.SetEventObject(this);
    // 业务处理器可能销毁本控件，之后不得再触碰成员。
    ProcessWindowEvent(event);
}

void AgentButton::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(CornerSurface()));
    dc.Clear();

    const wxRect rect = GetClientRect();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }

    const ComponentTokens& t = Tok(Component::Button);
    const double radius = ClampRadius(Dip(this, t.radiusDip), rect);

    // ① 主体
    if (m_variant == Variant::Primary && IsEnabled() && GetState() != State::Disabled) {
        wxGraphicsPath path = gc->CreatePath();
        path.AddRoundedRectangle(rect.x, rect.y, rect.width, rect.height, radius);
        gc->SetPen(*wxTRANSPARENT_PEN);
        if (GetState() == State::Pressed) {
            gc->SetBrush(gc->CreateBrush(wxBrush(A().ctaBottom)));
        } else {
            // H260 .send:hover{filter:brightness(1.06)}：wx 无 filter，用把渐变
            // 终点抬到亮端来近似（同一组 Token，不引入新色值）。
            const wxColour bottom =
                GetState() == State::Hover ? A().ctaTop : A().ctaBottom;
            gc->SetBrush(gc->CreateLinearGradientBrush(
                rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, A().ctaTop, bottom));
        }
        gc->FillPath(path);
    } else {
        FillRound(*gc, rect, radius, StateSurface());
    }

    // ② 1 DIP 边线（禁用/选中/hover 各有设计依据；Ghost 常态无边框）
    if (StateHasBorder()) {
        wxColour border = tokens::BorderColour(Component::Button);
        if (IsEnabled() && m_selected) {
            border = tokens::BorderColour(BorderRole::Accent);
        } else if (IsEnabled() && m_hover && !m_mousePressed && !m_keyPressed) {
            border = A().accent;  // H180 .act:hover border-color:var(--accent)
        }
        StrokeRound(*gc, rect, radius, border, Dip(this, t.borderDip));
    }

    // ③ 文本（垂直居中；超宽省略）——图标按钮只画图标（.send 白箭头 H259）
    if (m_hasIcon) {
        const int iconSize = Dip(this, m_iconDip);
        const wxRect box(rect.x + (rect.width - iconSize) / 2,
                         rect.y + (rect.height - iconSize) / 2, iconSize, iconSize);
        icons::Draw(*gc, m_icon, box, StateText());
    } else {
        gc->SetFont(FontOf(this, t, t.weight), StateText());
        const double padX = Dip(this, t.paddingXDip);
        const wxString text =
            EllipsizeToWidth(*gc, m_label, std::max(0.0, rect.width - 2.0 * padX));
        const double textW = TextWidth(*gc, text);
        const double textH = TextHeight(*gc);
        gc->DrawText(text, rect.x + std::max(0.0, (rect.width - textW) / 2.0),
                     rect.y + std::max(0.0, (rect.height - textH) / 2.0));
    }

    // ④ 键盘焦点环（原型 2 DIP accent + 3 DIP accent-soft 光晕）
    if (IsFocusVisible()) {
        const wxColour ring =
            m_variant == Variant::Primary ? A().textOnAccent : A().accent;
        DrawFocusRing(*gc, this, rect, t, radius, ring);
    }
}

void AgentButton::OnLeftDown(wxMouseEvent& event)
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
    UpdateVisualState();
}

void AgentButton::OnLeftUp(wxMouseEvent& event)
{
    UiFocusTrace::NotePointer();
    const bool wasPressed = m_mousePressed;
    m_mousePressed = false;
    const bool inside = GetClientRect().Contains(event.GetPosition());
    if (HasCapture()) {
        ReleaseMouse();
    }
    UpdateVisualState();
    if (wasPressed && inside && IsEnabled()) {
        Activate();  // 可能销毁本控件
    }
}

void AgentButton::OnCaptureLost(wxMouseCaptureLostEvent&)
{
    // 捕获被系统/其它窗口拿走：pressed 必须复位，否则会留下「按住的假象」。
    ResetPressState();
    UpdateVisualState();
}

void AgentButton::OnEnter(wxMouseEvent& event)
{
    if (!m_hover) {
        m_hover = true;
        UpdateVisualState();
    }
    event.Skip();
}

void AgentButton::OnLeave(wxMouseEvent& event)
{
    if (m_hover) {
        m_hover = false;
        UpdateVisualState();
    }
    event.Skip();
}

void AgentButton::OnKeyDown(wxKeyEvent& event)
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
    // 按住自动重复会连发 KEY_DOWN（没有中间的 KEY_UP）：只有第一次激活。
    bool& down = isSpace ? m_spaceDown : m_enterDown;
    if (down) {
        return;
    }
    down = true;
    m_keyPressed = true;
    UpdateVisualState();
    Activate();  // 可能销毁本控件
}

void AgentButton::OnKeyUp(wxKeyEvent& event)
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
        UpdateVisualState();
    }
}

void AgentButton::OnFocus(wxFocusEvent& event)
{
    m_focusTracker.OnFocusGained();
    UpdateVisualState();
    event.Skip();
}

void AgentButton::OnKillFocus(wxFocusEvent& event)
{
    m_focusTracker.OnFocusLost();
    ResetPressState();
    UpdateVisualState();
    event.Skip();
}

// ===========================================================================
// SegmentCell（AgentSegmented 的分段单元；roving tabindex，选中项才进 Tab 链）
// ===========================================================================

class SegmentCell final : public wxControl
{
public:
    SegmentCell(AgentSegmented* owner, int index, wxWindowID id, const wxString& label)
        : wxControl(owner, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_owner(owner),
          m_index(index), m_label(label)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        const ComponentTokens& t = Tok(Component::Button);
        SetFont(FontOf(this, t, t.weight));
        SetName(label);
        ApplyAccessibility();

        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { Paint(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) { OnLeftDown(event); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) { OnLeftUp(event); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_pressed = false;
            Refresh(false);
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_hover = true;
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hover = false;
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { OnKeyDown(event); });
        Bind(wxEVT_KEY_UP, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_SPACE) {
                m_spaceDown = false;
                m_pressed = false;
                Refresh(false);
            }
            event.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) {
            m_focusTracker.OnFocusGained();
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
            m_focusTracker.OnFocusLost();
            m_spaceDown = false;
            m_pressed = false;
            Refresh(false);
            event.Skip();
        });
    }

    void SetSelected(bool selected)
    {
        if (m_selected != selected) {
            m_selected = selected;
            ApplyAccessibility();
            Refresh(false);
        }
    }
    bool IsSelected() const { return m_selected; }

    ~SegmentCell() override
    {
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void SetTabStop(bool tabStop) { m_tabStop = tabStop; }

    void SetSegmentEnabled(bool enabled, const wxString& reason)
    {
        m_disabledReason = reason;
        if (IsEnabled() != enabled) {
            wxControl::Enable(enabled);
        }
        ApplyAccessibility();
        Refresh(false);
    }

    void SetAccessibleName(const wxString& name)
    {
        m_accessibleName = name;
        ApplyAccessibility();
    }

    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override
    {
        return AcceptsFocus() && m_tabStop;
    }

    wxSize DoGetBestSize() const override
    {
        const wxSize measured = MeasuredText(this, m_label);
        const int textW = measured.x;
        const int textH = measured.y;
        const int padX = Dip(this, S::SegBtnPadX);
        const int padY = Dip(this, S::SegBtnPadY);
        return wxSize(std::max(Dip(this, M::ToolButtonWidth), textW + 2 * padX),
                      std::max(Dip(this, M::ToolButtonHeight), textH + 2 * padY));
    }

private:
    wxColour CornerSurface() const { return A().sunken; }  // 容器底（H96）

    wxColour Surface() const
    {
        if (!IsEnabled()) {
            return m_selected ? A().accentSoft : A().sunken;
        }
        if (m_selected) {
            return tokens::SurfaceColour(Component::Button);  // H98 选中 = 面板白
        }
        if (m_hover || m_pressed) {
            return A().cardGlass;
        }
        return A().sunken;
    }

    wxColour Text() const
    {
        if (!IsEnabled()) {
            return A().text3;
        }
        if (m_selected) {
            return A().text1;
        }
        return (m_hover || m_pressed) ? A().text1 : A().text2;
    }

    void ApplyAccessibility()
    {
        SetName(m_accessibleName.empty() ? m_label : m_accessibleName);
        SetToolTip(m_disabledReason);
#if wxUSE_ACCESSIBILITY
        if (GetAccessible() == nullptr) {
            SetAccessible(new AgentAccessible(this, [this] {
                AccessInfo info;
                info.name = m_accessibleName.empty() ? m_label : m_accessibleName;
                info.help = m_disabledReason;
                info.description = m_disabledReason;
                info.role = wxROLE_SYSTEM_RADIOBUTTON;
                info.focusable = true;
                info.focused = HasFocus();
                info.unavailable = !IsEnabled();
                info.checked = m_selected;
                return info;
            }));
        }
#endif
    }

    void Paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(CornerSurface()));
        dc.Clear();

        const wxRect rect = GetClientRect();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || rect.width <= 0 || rect.height <= 0) {
            return;
        }
        const ComponentTokens& t = Tok(Component::Button);
        // H97 .mode-seg button border-radius:var(--r-pill)
        const double radius = ClampRadius(std::min(rect.width, rect.height) / 2.0, rect);
        if (m_selected || m_hover || m_pressed) {
            FillRound(*gc, rect, radius, Surface());
        }
        if (m_selected && IsEnabled()) {
            StrokeRound(*gc, rect, radius, tokens::BorderColour(BorderRole::Accent),
                        Dip(this, t.borderDip));
        }
        gc->SetFont(FontOf(this, t, t.weight), Text());
        const wxString shown =
            EllipsizeToWidth(*gc, m_label, std::max(0.0, rect.width - 2.0 * Dip(this, S::SegBtnPadX)));
        const double textW = TextWidth(*gc, shown);
        const double textH = TextHeight(*gc);
        gc->DrawText(shown, rect.x + std::max(0.0, (rect.width - textW) / 2.0),
                     rect.y + std::max(0.0, (rect.height - textH) / 2.0));
        if (HasFocus() && m_focusTracker.KeyboardFocused()) {
            DrawFocusRing(*gc, this, rect, t, radius, A().accent);
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
        m_pressed = true;
        if (!HasCapture()) {
            CaptureMouse();
        }
        Refresh(false);
    }

    void OnLeftUp(wxMouseEvent& event)
    {
        UiFocusTrace::NotePointer();
        const bool wasPressed = m_pressed;
        m_pressed = false;
        const bool inside = GetClientRect().Contains(event.GetPosition());
        if (HasCapture()) {
            ReleaseMouse();
        }
        Refresh(false);
        if (wasPressed && inside && IsEnabled() && m_owner != nullptr) {
            m_owner->SelectByUser(m_index);
        }
    }

    void OnKeyDown(wxKeyEvent& event)
    {
        UiFocusTrace::NoteKeyboard();
        const int code = event.GetKeyCode();
        switch (code) {
            case WXK_LEFT:
            case WXK_UP:
                if (m_owner != nullptr) {
                    m_owner->StepSelection(m_index, -1);
                }
                return;
            case WXK_RIGHT:
            case WXK_DOWN:
                if (m_owner != nullptr) {
                    m_owner->StepSelection(m_index, +1);
                }
                return;
            case WXK_SPACE:
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER: {
                if (!IsEnabled()) {
                    return;
                }
                const bool isSpace = code == WXK_SPACE;
                // 自动重复不重复激活
                bool& down = isSpace ? m_spaceDown : m_enterDown;
                if (down) {
                    return;
                }
                down = true;
                m_pressed = true;
                Refresh(false);
                if (m_owner != nullptr) {
                    m_owner->SelectByUser(m_index);
                }
                return;
            }
            default:
                event.Skip();
                return;
        }
    }

    AgentSegmented* m_owner = nullptr;
    int m_index = -1;
    wxString m_label;
    wxString m_accessibleName;
    wxString m_disabledReason;
    bool m_selected = false;
    bool m_tabStop = false;
    bool m_hover = false;
    bool m_pressed = false;
    bool m_spaceDown = false;
    bool m_enterDown = false;
    UiFocusRing::Tracker m_focusTracker;
};

// ===========================================================================
// AgentSegmented
// ===========================================================================

AgentSegmented::AgentSegmented(wxWindow* parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetName(wxString::FromUTF8("AgentSegmented"));  // 分组名；每个分段的名称见 SegmentCell
    Bind(wxEVT_PAINT, &AgentSegmented::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Arrange();
        event.Skip();
    });
    m_theme = tokens::SubscribeTheme([this] {
        ApplyTheme();
    });
}

AgentSegmented::~AgentSegmented() = default;

int AgentSegmented::PadPx() const
{
    return Dip(this, S::SegPad);
}

int AgentSegmented::GapPx() const
{
    return Dip(this, S::SegGap);
}

int AgentSegmented::IndexOf(int segmentId) const
{
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].id == segmentId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int AgentSegmented::FirstEnabled() const
{
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].enabled) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool AgentSegmented::IsTabStopCell(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_entries.size()) ||
        !m_entries[index].enabled) {
        return false;
    }
    const int selected = IndexOf(m_selection);
    if (selected >= 0 && m_entries[static_cast<size_t>(selected)].enabled) {
        // 选中项在 Tab 链里（roving tabindex）
        return selected == index;
    }
    // 无选中或选中项被禁用：把 Tab 停靠点交给第一个可用分段，避免整组不可达
    return FirstEnabled() == index;
}

int AgentSegmented::AddSegment(int segmentId, const wxString& label,
                               const wxString& accessibleName)
{
    const int existing = IndexOf(segmentId);
    if (existing >= 0) {
        return existing;  // id 唯一；重复添加不产生第二个分段
    }
    Entry entry;
    entry.id = segmentId;
    entry.label = label;
    entry.name = accessibleName.empty() ? label : accessibleName;
    m_entries.push_back(entry);

    auto* cell = new SegmentCell(this, static_cast<int>(m_entries.size()) - 1, wxID_ANY, label);
    cell->SetAccessibleName(entry.name);
    m_cells.push_back(cell);

    RefreshCells();
    Arrange();
    InvalidateBestSize();
    if (m_selection < 0) {
        m_selection = segmentId;  // 首个分段默认为选中，不发事件
        RefreshCells();
    }
    Refresh(false);
    return static_cast<int>(m_entries.size()) - 1;
}

void AgentSegmented::SetSelection(int segmentId)
{
    const int index = IndexOf(segmentId);
    if (index < 0) {
        return;
    }
    m_selection = segmentId;
    RefreshCells();
    Refresh(false);
}

bool AgentSegmented::IsSegmentEnabled(int segmentId) const
{
    const int index = IndexOf(segmentId);
    return index >= 0 && m_entries[index].enabled;
}

void AgentSegmented::SetSegmentEnabled(int segmentId, bool enabled,
                                       const wxString& reason)
{
    const int index = IndexOf(segmentId);
    if (index < 0) {
        return;
    }
    m_entries[index].enabled = enabled;
    m_entries[index].reason = enabled ? wxString() : reason;
    ApplyEntry(index);
    RefreshCells();
    Refresh(false);
}

wxWindow* AgentSegmented::SegmentWindow(int segmentId) const
{
    const int index = IndexOf(segmentId);
    if (index < 0 || index >= static_cast<int>(m_cells.size())) {
        return nullptr;
    }
    return m_cells[index];
}

void AgentSegmented::SelectByUser(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entries.size())) {
        return;
    }
    if (!m_entries[index].enabled) {
        return;  // disabled 分段不改变选择、不发事件
    }
    if (m_selection == m_entries[index].id) {
        return;  // 选择未变化：不发事件
    }
    m_selection = m_entries[index].id;
    RefreshCells();
    Refresh(false);

    wxCommandEvent event(EVT_AGENT_SEGMENT, GetId());
    event.SetEventObject(this);
    event.SetInt(m_selection);
    event.SetInt(m_selection);
    ProcessWindowEvent(event);
}

void AgentSegmented::StepSelection(int fromIndex, int delta)
{
    if (delta == 0 || fromIndex < 0 || fromIndex >= static_cast<int>(m_entries.size())) {
        return;
    }
    // 不环绕：走到端点后停住，避免「按一下又跳回另一端」的误触发。
    for (int index = fromIndex + delta; index >= 0 && index < static_cast<int>(m_entries.size());
         index += delta) {
        if (!m_entries[index].enabled) {
            continue;
        }
        const bool changed = m_selection != m_entries[index].id;
        m_selection = m_entries[index].id;
        RefreshCells();
        Refresh(false);
        if (index < static_cast<int>(m_cells.size()) && m_cells[index] != nullptr) {
            m_cells[index]->SetFocus();
        }
        if (changed) {
            wxCommandEvent event(EVT_AGENT_SEGMENT, GetId());
            event.SetEventObject(this);
            event.SetInt(m_selection);
            event.SetInt(m_selection);
            ProcessWindowEvent(event);
        }
        return;
    }
}

void AgentSegmented::ApplyEntry(int index)
{
    if (index < 0 || index >= static_cast<int>(m_cells.size()) || m_cells[index] == nullptr) {
        return;
    }
    SegmentCell* cell = m_cells[index];
    cell->SetSegmentEnabled(m_entries[index].enabled, m_entries[index].reason);
    cell->SetAccessibleName(m_entries[index].name);
}

void AgentSegmented::RefreshCells()
{
    for (size_t i = 0; i < m_cells.size(); ++i) {
        SegmentCell* cell = m_cells[i];
        if (cell == nullptr) {
            continue;
        }
        cell->SetSelected(m_entries[i].id == m_selection);
        cell->SetTabStop(IsTabStopCell(static_cast<int>(i)));
        cell->SetSegmentEnabled(m_entries[i].enabled, m_entries[i].reason);
    }
}

void AgentSegmented::ApplyTheme()
{
    for (size_t i = 0; i < m_cells.size(); ++i) {
        if (m_cells[i] != nullptr) {
            m_cells[i]->Refresh(false);
        }
    }
    Refresh(false);
}

void AgentSegmented::Arrange()
{
    const wxSize client = GetClientSize();
    const int width = std::max(0, client.x);
    const int height = std::max(0, client.y);
    const int pad = PadPx();
    const int gap = GapPx();
    const int cellHeight = std::max(0, height - 2 * pad);

    std::vector<int> widths(m_cells.size(), 0);
    int natural = 0;
    for (size_t i = 0; i < m_cells.size(); ++i) {
        widths[i] = m_cells[i] != nullptr ? m_cells[i]->GetBestSize().x : 0;
        natural += widths[i];
    }
    if (!m_cells.empty()) {
        natural += gap * (static_cast<int>(m_cells.size()) - 1);
    }
    const int available = std::max(0, width - 2 * pad);
    if (natural > available && natural > 0) {
        // 极窄容器：按比例压缩，最小 1 像素，仍保持非负尺寸
        for (size_t i = 0; i < widths.size(); ++i) {
            const int scaled = static_cast<int>(
                std::floor(static_cast<double>(widths[i]) * available / natural));
            widths[i] = std::max(1, scaled);
        }
    }

    int x = pad;
    for (size_t i = 0; i < m_cells.size(); ++i) {
        if (m_cells[i] == nullptr) {
            continue;
        }
        m_cells[i]->SetSize(x, pad, std::max(0, widths[i]), cellHeight);
        x += widths[i] + gap;
    }
}

void AgentSegmented::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(A().pageBg));
    dc.Clear();

    const wxRect rect = GetClientRect();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    const ComponentTokens& button = Tok(Component::Button);
    // H96 .mode-seg：sunken 底 + hairline + pill 圆角 + padding:3px
    const double radius = ClampRadius(std::min(rect.width, rect.height) / 2.0, rect);
    FillRound(*gc, rect, radius, A().sunken);
    StrokeRound(*gc, rect, radius, tokens::BorderColour(BorderRole::Hairline),
                Dip(this, button.borderDip));
}

wxSize AgentSegmented::DoGetBestSize() const
{
    int width = 2 * PadPx();
    int height = 0;
    for (size_t i = 0; i < m_cells.size(); ++i) {
        if (m_cells[i] == nullptr) {
            continue;
        }
        const wxSize best = m_cells[i]->GetBestSize();
        width += best.x;
        height = std::max(height, best.y);
    }
    if (m_cells.size() > 1) {
        width += GapPx() * (static_cast<int>(m_cells.size()) - 1);
    }
    height += 2 * PadPx();
    return wxSize(width, height);
}

// ===========================================================================
// AgentPill
// ===========================================================================

AgentPill::AgentPill(wxWindow* parent, wxWindowID id, const wxString& text, Tone tone)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_text(text),
      m_tone(tone)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    const ComponentTokens& t = Tok(Component::Pill);
    SetFont(FontOf(this, t, t.weight));
    SetName(text);
    Bind(wxEVT_PAINT, &AgentPill::OnPaint, this);
    m_theme = tokens::SubscribeTheme([this] {
        ApplyTheme();
    });
}

AgentPill::~AgentPill() = default;

void AgentPill::SetTone(Tone tone)
{
    if (m_tone == tone) {
        return;
    }
    m_tone = tone;
    Refresh(false);
}

void AgentPill::SetText(const wxString& text)
{
    if (m_text == text) {
        return;
    }
    m_text = text;
    SetName(text);
    InvalidateBestSize();
    Refresh(false);
}

void AgentPill::ApplyTheme()
{
    Refresh(false);
}

wxSize AgentPill::DoGetBestSize() const
{
    const ComponentTokens& t = Tok(Component::Pill);
    const wxSize measured = MeasuredText(this, m_text);
    const int textW = measured.x;
    const int textH = measured.y;
    const int dot = Dip(this, t.iconDip);
    const int width = 2 * Dip(this, t.paddingXDip) + dot + Dip(this, t.gapDip) + textW;
    const int height = 2 * Dip(this, t.paddingYDip) + std::max(textH, dot);
    return wxSize(width, height);
}

void AgentPill::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    // 胶囊常放在卡片/对话玻璃上；角部按卡片玻璃面填充（子窗口无透明背景）
    dc.SetBackground(wxBrush(tokens::SurfaceColour(Component::Card)));
    dc.Clear();

    const wxRect rect = GetClientRect();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }

    const ComponentTokens& t = Tok(Component::Pill);
    // H159-163 逐档语义（颜色/边线都取自 AgentTokens + BorderRole）
    wxColour surface = tokens::SurfaceColour(Component::Pill);
    wxColour text = tokens::TextColour(Component::Pill);
    wxColour border = tokens::BorderColour(Component::Pill);
    switch (m_tone) {
        case Tone::Neutral:
            break;
        case Tone::Accent:  // H161 .pill.blue
            surface = A().accentSoft;
            text = A().accentText;
            border = tokens::BorderColour(BorderRole::AccentMid);
            break;
        case Tone::Success:  // H162 .pill.green
            surface = A().okSoft;
            text = A().okText;
            border = tokens::BorderColour(BorderRole::Ok);
            break;
        case Tone::Warn:  // H163 .pill.amber
            surface = A().warnSoft;
            text = A().warnText;
            border = tokens::BorderColour(BorderRole::Warn);
            break;
        case Tone::Danger:  // H160 .pill.red
            surface = A().dangerSoft;
            text = A().dangerText;
            border = tokens::BorderColour(BorderRole::Danger);
            break;
    }

    const double radius = ClampRadius(std::min(rect.width, rect.height) / 2.0, rect);
    FillRound(*gc, rect, radius, surface);
    StrokeRound(*gc, rect, radius, border, Dip(this, t.borderDip));

    // 语义圆点（H164 .pill .d 6px 用 currentColor）：只是几何圆，不是图标资源
    const int dot = Dip(this, t.iconDip);
    const double dotX = rect.x + Dip(this, t.paddingXDip) + dot / 2.0;
    const double dotY = rect.y + rect.height / 2.0;
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateBrush(wxBrush(text)));
    gc->DrawEllipse(dotX - dot / 2.0, dotY - dot / 2.0, dot, dot);

    gc->SetFont(FontOf(this, t, t.weight), text);
    const double textX = rect.x + Dip(this, t.paddingXDip) + dot + Dip(this, t.gapDip);
    const double maxWidth = std::max(0.0, rect.GetRight() - textX - Dip(this, t.paddingXDip));
    const wxString shown = EllipsizeToWidth(*gc, m_text, maxWidth);
    const double textH = TextHeight(*gc);
    gc->DrawText(shown, textX, rect.y + std::max(0.0, (rect.height - textH) / 2.0));
}

// ===========================================================================
// AgentKvRow
// ===========================================================================

AgentKvRow::AgentKvRow(wxWindow* parent, const wxString& key, const wxString& value)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_key(key),
      m_value(value)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    const ComponentTokens& t = Tok(Component::Card);
    SetFont(FontOf(this, t, tokens::Weight::Normal));
    SetName(key);
    Bind(wxEVT_PAINT, &AgentKvRow::OnPaint, this);
    m_theme = tokens::SubscribeTheme([this] {
        ApplyTheme();
    });
}

AgentKvRow::~AgentKvRow() = default;

void AgentKvRow::SetKey(const wxString& key)
{
    if (m_key == key) {
        return;
    }
    m_key = key;
    SetName(key);
    Refresh(false);
}

void AgentKvRow::SetValue(const wxString& value)
{
    if (m_value == value) {
        return;
    }
    m_value = value;
    Refresh(false);
}

void AgentKvRow::ApplyTheme()
{
    Refresh(false);
}

int AgentKvRow::ValueWidthPx() const
{
    const ComponentTokens& t = Tok(Component::Card);
    const wxRect rect = GetClientRect();
    return std::max(0, rect.width - Dip(this, M::KvKeyWidth) - Dip(this, t.gapDip));
}

wxSize AgentKvRow::DoGetBestSize() const
{
    const ComponentTokens& t = Tok(Component::Card);
    int valueW = 0;
    int valueH = 0;
    const_cast<AgentKvRow*>(this)->GetTextExtent(m_value, &valueW, &valueH);
    const int height = std::max(valueH, tokens::LineHeightDip(this, t));
    const int width = Dip(this, M::KvKeyWidth) + Dip(this, t.gapDip) + valueW;
    return wxSize(width, height);
}

void AgentKvRow::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(tokens::SurfaceColour(Component::Card)));
    dc.Clear();

    const wxRect rect = GetClientRect();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    const ComponentTokens& t = Tok(Component::Card);
    const wxFont font = FontOf(this, t, tokens::Weight::Normal);
    gc->SetFont(font, A().text3);
    const double keyW = Dip(this, M::KvKeyWidth);
    const double gap = Dip(this, t.gapDip);
    const double textH = TextHeight(*gc);
    const double y = rect.y + std::max(0.0, (rect.height - textH) / 2.0);

    const wxString key = EllipsizeToWidth(*gc, m_key, std::max(0.0, keyW - gap / 2.0));
    gc->DrawText(key, rect.x, y);

    gc->SetFont(font, A().text1);
    const double valueX = rect.x + keyW + gap;
    const double valueW = std::max(0.0, rect.GetRight() - valueX);
    gc->DrawText(EllipsizeToWidth(*gc, m_value, valueW), valueX, y);
}

// ===========================================================================
// AgentCardHost
// ===========================================================================

AgentCardHost::AgentCardHost(wxWindow* parent, wxWindowID id, const wxString& title)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_title(title)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFont(FontOf(this, Tok(Component::Card), Tok(Component::Card).weight));
    SetName(title.empty() ? wxString::FromUTF8("AgentCardHost") : title);
    Bind(wxEVT_PAINT, &AgentCardHost::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Arrange();
        event.Skip();
    });
    m_theme = tokens::SubscribeTheme([this] {
        ApplyTheme();
    });
}

AgentCardHost::~AgentCardHost() = default;

int AgentCardHost::RadiusPx() const
{
    return Dip(this, Tok(Component::Card).radiusDip);
}

int AgentCardHost::InsetPx() const
{
    const ComponentTokens& t = Tok(Component::Card);
    const double radius = static_cast<double>(Dip(this, t.radiusDip));
    // 圆角切点：内容必须落在弧线内侧，r*(1-1/√2) 是「弧形后退」的最大距离。
    const int byCorner = static_cast<int>(std::ceil(radius * (1.0 - std::sqrt(0.5))));
    const int byMetric = Dip(this, M::CardInset);
    // 再加 1 DIP 描边宽度，避免内容压到 hairline 上（LAY-009「无旧方框包边」）。
    return std::max(byMetric, byCorner) + Dip(this, t.borderDip);
}

int AgentCardHost::HeaderHeightPx() const
{
    if (m_title.empty()) {
        return 0;
    }
    const ComponentTokens& t = Tok(Component::Card);
    return 2 * Dip(this, S::CardHeadPadY) + tokens::LineHeightDip(this, t) +
           Dip(this, t.borderDip);
}

void AgentCardHost::SetTitle(const wxString& title)
{
    if (m_title == title) {
        return;
    }
    m_title = title;
    InvalidateBestSize();
    Arrange();
    Refresh(false);
}

wxRect AgentCardHost::ContentRectPx() const
{
    const wxSize size = GetClientSize();
    const int width = std::max(0, size.x);
    const int height = std::max(0, size.y);
    const int inset = InsetPx();
    const int header = HeaderHeightPx();
    const int left = std::min(inset, width);
    const int top = std::min(header > 0 ? header : inset, height);
    const int contentWidth = std::max(0, width - 2 * left);
    const int contentHeight = std::max(0, height - top - std::min(inset, height));
    return wxRect(left, top, contentWidth, contentHeight);
}

void AgentCardHost::SetContent(wxWindow* content)
{
    if (content == m_content) {
        if (m_content != nullptr) {
            Arrange();
            m_content->Show();
        }
        return;
    }
    if (m_content != nullptr) {
        // 旧内容不销毁、不脱离父窗口（Reparent(nullptr) 会造出游离顶层窗口），
        // 只隐藏并留给宿主析构时统一回收。
        m_content->Hide();
        m_content = nullptr;
    }
    m_content = content;
    if (m_content != nullptr) {
        if (m_content->GetParent() != this) {
            m_content->Reparent(this);
        }
        m_content->SetBackgroundColour(tokens::SurfaceColour(Component::Button));
        m_content->Show();
    }
    InvalidateBestSize();
    Arrange();
    Refresh(false);
}

void AgentCardHost::Arrange()
{
    if (m_content == nullptr) {
        return;
    }
    const wxRect rect = ContentRectPx();
    m_content->SetSize(rect);
    if (rect.width > 0 && rect.height > 0) {
        m_content->Show();
    }
}

void AgentCardHost::ApplyTheme()
{
    if (m_content != nullptr) {
        m_content->SetBackgroundColour(tokens::SurfaceColour(Component::Button));
        m_content->Refresh(false);
    }
    Refresh(false);
}

wxSize AgentCardHost::DoGetBestSize() const
{
    const int inset = InsetPx();
    int contentWidth = 0;
    int contentHeight = 0;
    if (m_content != nullptr) {
        const wxSize best = m_content->GetBestSize();
        contentWidth = std::max(0, best.x);
        contentHeight = std::max(0, best.y > 0 ? best.y : ContentBestHeight(m_content));
    }
    const int minWidth = Dip(this, M::KvKeyWidth) * 3;
    const int width = std::max(minWidth, contentWidth) + 2 * inset;
    const int height = HeaderHeightPx() + contentHeight + 2 * inset;
    return wxSize(width, height);
}

void AgentCardHost::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(A().pageBg));
    dc.Clear();

    const wxRect rect = GetClientRect();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || rect.width <= 0 || rect.height <= 0) {
        return;
    }

    const ComponentTokens& t = Tok(Component::Card);
    const double radius = ClampRadius(RadiusPx(), rect);

    // ① 玻璃面（H242 rgba(255,255,255,.66) 的预合成实色；SAFE-008 不做真实模糊）
    wxGraphicsPath path = gc->CreatePath();
    path.AddRoundedRectangle(rect.x, rect.y, rect.width, rect.height, radius);
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(gc->CreateBrush(wxBrush(tokens::SurfaceColour(Component::Card))));
    gc->FillPath(path);

    // ② 标题条（H243 .card-h：sunkenSoftStrong 底 + 底部 hairline），裁进圆角内
    //    wx 3.2 的 wxGraphicsContext 只能用矩形裁剪（没有 path clip），因此先按
    //    「标题高度」矩形裁剪，再填充整张圆角卡片路径 —— 上圆角依旧被尊重。
    const int header = HeaderHeightPx();
    if (header > 0) {
        gc->Clip(rect.x, rect.y, rect.width, header);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateBrush(wxBrush(A().sunkenSoftStrong)));
        gc->FillPath(path);
        gc->SetPen(gc->CreatePen(wxPen(tokens::BorderColour(BorderRole::Hairline),
                                       Dip(this, t.borderDip))));
        gc->StrokeLine(rect.x, rect.y + header - Dip(this, t.borderDip),
                       rect.x + rect.width, rect.y + header - Dip(this, t.borderDip));
        gc->ResetClip();
    }

    // ③ 1 DIP 高光边（H242 border rgba(255,255,255,.85)）
    StrokeRound(*gc, rect, radius, tokens::BorderColour(Component::Card),
                Dip(this, t.borderDip));

    // ④ 标题文字（只画一次，不再画「内层方框」）
    if (header > 0) {
        gc->SetFont(FontOf(this, t, t.weight), tokens::TextColour(Component::Card));
        const double textX = rect.x + Dip(this, S::CardHeadPadX);
        const double maxWidth = std::max(0.0, rect.width - 2.0 * Dip(this, S::CardHeadPadX));
        const wxString shown = EllipsizeToWidth(*gc, m_title, maxWidth);
        const double textH = TextHeight(*gc);
        gc->DrawText(shown, textX,
                     rect.y + std::max(0.0, (header - textH) / 2.0));
    }
}

// ===========================================================================
// SectionHeader（AgentAccordion 的标题行）
// ===========================================================================

class SectionHeader final : public wxControl
{
public:
    SectionHeader(AgentAccordion* owner, int index, wxWindowID id, const wxString& title)
        : wxControl(owner, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_owner(owner),
          m_index(index), m_title(title)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        const ComponentTokens& t = Tok(Component::Card);
        SetFont(FontOf(this, t, tokens::Weight::Medium));
        SetName(title);
        ApplyAccessibility();

        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { Paint(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            UiFocusTrace::NotePointer();
            SetFocus();
            m_pressed = true;
            if (!HasCapture()) {
                CaptureMouse();
            }
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            UiFocusTrace::NotePointer();
            const bool wasPressed = m_pressed;
            m_pressed = false;
            const bool inside = GetClientRect().Contains(event.GetPosition());
            if (HasCapture()) {
                ReleaseMouse();
            }
            Refresh(false);
            if (wasPressed && inside && m_owner != nullptr) {
                m_owner->ToggleByUser(m_index);
            }
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_pressed = false;
            Refresh(false);
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_hover = true;
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hover = false;
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { OnKeyDown(event); });
        Bind(wxEVT_KEY_UP, [this](wxKeyEvent& event) {
            const int code = event.GetKeyCode();
            if (code == WXK_SPACE) {
                m_spaceDown = false;
            } else if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
                m_enterDown = false;
            }
            m_pressed = false;
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) {
            m_focusTracker.OnFocusGained();
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
            m_focusTracker.OnFocusLost();
            m_spaceDown = false;
            m_enterDown = false;
            m_pressed = false;
            Refresh(false);
            event.Skip();
        });
    }

    void SetTitle(const wxString& title)
    {
        m_title = title;
        SetName(title);
        ApplyAccessibility();
        Refresh(false);
    }

    ~SectionHeader() override
    {
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void SetExpandedState(bool expanded)
    {
        if (m_expanded != expanded) {
            m_expanded = expanded;
            ApplyAccessibility();
            Refresh(false);
        }
    }

    bool IsExpandedState() const { return m_expanded; }
    bool IsLastSection() const { return m_last; }
    void SetLastSection(bool last) { m_last = last; }

    /// T06 图标槽位（12 DIP 的 caret）：本轮只预留，不绘制矢量图标
    wxRect CaretRectPx() const
    {
        const int size = Dip(this, Tok(Component::Card).iconDip);
        const wxRect rect = GetClientRect();
        return wxRect(rect.x + Dip(this, S::AccHeadPadX),
                      rect.y + std::max(0, (rect.height - size) / 2), size, size);
    }

    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return AcceptsFocus(); }

    wxSize DoGetBestSize() const override
    {
        const wxSize measured = MeasuredText(this, m_title);
        const int textW = measured.x;
        const int textH = measured.y;
        const int caret = Dip(this, Tok(Component::Card).iconDip);
        const int width = 2 * Dip(this, S::AccHeadPadX) + caret + Dip(this, S::HeadGap) + textW;
        const int height = std::max(2 * Dip(this, S::AccHeadPadY) + textH,
                                    Dip(this, M::ButtonHeight));
        return wxSize(width, height);
    }

private:
    void ApplyAccessibility()
    {
        SetName(m_title);
#if wxUSE_ACCESSIBILITY
        if (GetAccessible() == nullptr) {
            SetAccessible(new AgentAccessible(this, [this] {
                AccessInfo info;
                info.name = m_title;
                info.help = m_expanded ? wxString::FromUTF8("已展开，回车收起")
                                       : wxString::FromUTF8("已收起，回车展开");
                info.description = info.help;
                info.role = wxROLE_SYSTEM_PUSHBUTTON;
                info.focusable = true;
                info.focused = HasFocus();
                info.expandedState = m_expanded ? 1 : 2;
                return info;
            }));
        }
#endif
    }

    void Paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(tokens::SurfaceColour(Component::Card)));
        dc.Clear();

        const wxRect rect = GetClientRect();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || rect.width <= 0 || rect.height <= 0) {
            return;
        }
        const ComponentTokens& t = Tok(Component::Card);

        // 状态底：展开用 sunkenSoftStrong（与 .card-h 同族），hover 用 sunken（H146）
        if (m_expanded) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(gc->CreateBrush(wxBrush(A().sunkenSoftStrong)));
            gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);
        } else if (m_hover || m_pressed) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(gc->CreateBrush(wxBrush(A().sunken)));
            gc->DrawRectangle(rect.x, rect.y, rect.width, rect.height);
        }

        // 分隔线（H143 .acc border-bottom；最后一节不画）
        if (!m_last) {
            gc->SetPen(
                gc->CreatePen(wxPen(tokens::BorderColour(BorderRole::Hairline),
                                    Dip(this, t.borderDip))));
            gc->StrokeLine(rect.x, rect.GetBottom() - Dip(this, t.borderDip) / 2.0,
                           rect.x + rect.width,
                           rect.GetBottom() - Dip(this, t.borderDip) / 2.0);
        }

        // 文本：caret 槽位（CaretRectPx）本轮留空，文字从槽位右侧开始
        const wxRect caret = CaretRectPx();
        const double textX = caret.GetRight() + Dip(this, S::HeadGap);
        const double maxWidth = std::max(0.0, rect.GetRight() - textX - Dip(this, S::AccHeadPadX));
        gc->SetFont(FontOf(this, t, tokens::Weight::Medium), A().text1);
        const wxString shown = EllipsizeToWidth(*gc, m_title, maxWidth);
        const double textH = TextHeight(*gc);
        gc->DrawText(shown, textX, rect.y + std::max(0.0, (rect.height - textH) / 2.0));

        if (HasFocus() && m_focusTracker.KeyboardFocused()) {
            DrawFocusRing(*gc, this, rect, t, 0.0, A().accent);
        }
    }

    void OnKeyDown(wxKeyEvent& event)
    {
        UiFocusTrace::NoteKeyboard();
        const int code = event.GetKeyCode();
        switch (code) {
            case WXK_UP:
                if (m_owner != nullptr) {
                    m_owner->MoveHeaderFocus(m_index, -1);
                }
                return;
            case WXK_DOWN:
                if (m_owner != nullptr) {
                    m_owner->MoveHeaderFocus(m_index, +1);
                }
                return;
            case WXK_SPACE:
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER: {
                const bool isSpace = code == WXK_SPACE;
                bool& down = isSpace ? m_spaceDown : m_enterDown;
                if (down) {
                    return;  // 自动重复不重复切换
                }
                down = true;
                m_pressed = true;
                Refresh(false);
                if (m_owner != nullptr) {
                    m_owner->ToggleByUser(m_index);
                }
                return;
            }
            default:
                event.Skip();
                return;
        }
    }

    AgentAccordion* m_owner = nullptr;
    int m_index = -1;
    wxString m_title;
    bool m_expanded = false;
    bool m_last = false;
    bool m_hover = false;
    bool m_pressed = false;
    bool m_spaceDown = false;
    bool m_enterDown = false;
    UiFocusRing::Tracker m_focusTracker;
};

// ===========================================================================
// AgentAccordion
// ===========================================================================

AgentAccordion::AgentAccordion(wxWindow* parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetName(wxString::FromUTF8("AgentAccordion"));
    Bind(wxEVT_PAINT, &AgentAccordion::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Arrange();
        event.Skip();
    });
    m_theme = tokens::SubscribeTheme([this] {
        ApplyTheme();
    });
}

AgentAccordion::~AgentAccordion() = default;

int AgentAccordion::HeaderHeightPx() const
{
    const ComponentTokens& t = Tok(Component::Card);
    const int textH = tokens::LineHeightDip(this, t);
    return std::max(2 * Dip(this, S::AccHeadPadY) + textH, Dip(this, M::ButtonHeight));
}

int AgentAccordion::BodyHeightPx(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return 0;
    }
    const Section& section = m_sections[static_cast<size_t>(index)];
    if (section.content == nullptr || !section.expanded) {
        return 0;
    }
    return ContentBestHeight(section.content);
}

void AgentAccordion::SetSectionTitle(int index, const wxString& title)
{
    if (index < 0 || index >= static_cast<int>(m_sections.size()) || m_sections[index].title == title) return;
    m_sections[index].title = title;
    m_headers[index]->SetTitle(title);
}

int AgentAccordion::AddSection(const wxString& title, wxWindow* content, bool expanded)
{
    Section section;
    section.title = title;
    section.content = content;
    section.expanded = expanded;
    m_sections.push_back(section);

    auto* header = new SectionHeader(this, static_cast<int>(m_sections.size()) - 1, wxID_ANY,
                                     title);
    m_headers.push_back(header);

    if (content != nullptr && content->GetParent() != this) {
        content->Reparent(this);
    }
    if (content != nullptr) {
        content->SetBackgroundColour(tokens::SurfaceColour(Component::Button));
    }

    RefreshHeaders();
    ApplySection(static_cast<int>(m_sections.size()) - 1);
    InvalidateBestSize();
    Arrange();
    return static_cast<int>(m_sections.size()) - 1;
}

void AgentAccordion::SetExpanded(int index, bool expanded)
{
    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return;
    }
    if (m_sections[static_cast<size_t>(index)].expanded == expanded) {
        return;
    }
    m_sections[static_cast<size_t>(index)].expanded = expanded;
    ApplySection(index);
    InvalidateBestSize();
    Arrange();
    if (wxWindow* parent = GetParent()) {
        parent->Layout();
    }
    Refresh(false);
}

bool AgentAccordion::IsExpanded(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return false;
    }
    return m_sections[static_cast<size_t>(index)].expanded;
}

wxWindow* AgentAccordion::SectionContent(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return nullptr;
    }
    return m_sections[static_cast<size_t>(index)].content;
}

wxWindow* AgentAccordion::SectionHeaderWindow(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_headers.size())) {
        return nullptr;
    }
    return m_headers[static_cast<size_t>(index)];
}

wxRect AgentAccordion::HeaderRectPx(int index) const
{
    if (wxWindow* header = SectionHeaderWindow(index)) {
        return header->GetRect();
    }
    return wxRect();
}

wxRect AgentAccordion::BodyRectPx(int index) const
{
    wxWindow* content = SectionContent(index);
    if (content == nullptr || !content->IsShown()) {
        return wxRect();
    }
    return content->GetRect();
}

// AGUI-T09: hosts reflow the accordion when their own width changes. Section
// bodies are measured by the host at the new width; this only re-applies the
// geometry (no events, no business change) and repaints the headers.
void AgentAccordion::Relayout()
{
    InvalidateBestSize();
    Arrange();
    Refresh(false);
}

void AgentAccordion::ApplySection(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return;
    }
    Section& section = m_sections[static_cast<size_t>(index)];
    if (index < static_cast<int>(m_headers.size()) && m_headers[static_cast<size_t>(index)] != nullptr) {
        m_headers[static_cast<size_t>(index)]->SetExpandedState(section.expanded);
    }
    if (section.content != nullptr) {
        section.content->Show(section.expanded);
    }
}

void AgentAccordion::RefreshHeaders()
{
    for (size_t i = 0; i < m_headers.size(); ++i) {
        if (m_headers[i] == nullptr) {
            continue;
        }
        m_headers[i]->SetLastSection(i + 1 == m_headers.size());
    }
}

void AgentAccordion::ApplyTheme()
{
    for (size_t i = 0; i < m_headers.size(); ++i) {
        if (m_headers[i] != nullptr) {
            m_headers[i]->Refresh(false);
        }
    }
    for (size_t i = 0; i < m_sections.size(); ++i) {
        if (m_sections[i].content != nullptr) {
            m_sections[i].content->SetBackgroundColour(
                tokens::SurfaceColour(Component::Button));
            m_sections[i].content->Refresh(false);
        }
    }
    Refresh(false);
}

void AgentAccordion::ToggleByUser(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return;
    }
    const bool expanded = !m_sections[static_cast<size_t>(index)].expanded;
    m_sections[static_cast<size_t>(index)].expanded = expanded;
    ApplySection(index);
    InvalidateBestSize();
    Arrange();
    if (wxWindow* parent = GetParent()) {
        parent->Layout();
    }
    Refresh(false);

    // wxCommandEvent 只有一个 int 字段（GetSelection/GetInt 同源）：
    // index 放 SetInt，展开标志放 ExtraLong（1/0）与 SetString。
    wxCommandEvent event(EVT_AGENT_ACCORDION, GetId());
    event.SetEventObject(this);
    event.SetInt(index);
    event.SetExtraLong(expanded ? 1 : 0);
    event.SetString(expanded ? wxString::FromUTF8("expanded")
                             : wxString::FromUTF8("collapsed"));
    ProcessWindowEvent(event);
}

void AgentAccordion::MoveHeaderFocus(int index, int delta)
{
    if (delta == 0) {
        return;
    }
    for (int target = index + delta;
         target >= 0 && target < static_cast<int>(m_headers.size()); target += delta) {
        if (m_headers[static_cast<size_t>(target)] == nullptr) {
            continue;
        }
        if (!m_headers[static_cast<size_t>(target)]->AcceptsFocus()) {
            continue;
        }
        m_headers[static_cast<size_t>(target)]->SetFocus();
        return;
    }
}

void AgentAccordion::Arrange()
{
    const int width = std::max(0, GetClientSize().x);
    const int headerHeight = HeaderHeightPx();
    const int padX = Dip(this, S::AccBodyPadX);
    const int padBottom = Dip(this, S::AccBodyPadBottom);

    int y = 0;
    for (size_t i = 0; i < m_sections.size(); ++i) {
        if (i < m_headers.size() && m_headers[i] != nullptr) {
            m_headers[i]->SetSize(0, y, width, headerHeight);
        }
        y += headerHeight;
        Section& section = m_sections[i];
        if (section.content == nullptr) {
            continue;
        }
        if (!section.expanded) {
            section.content->Hide();
            continue;
        }
        const int bodyHeight = BodyHeightPx(static_cast<int>(i));
        section.content->SetSize(padX, y, std::max(0, width - 2 * padX),
                                 std::max(0, bodyHeight));
        section.content->Show(true);
        y += bodyHeight + padBottom;
    }
}

wxSize AgentAccordion::DoGetBestSize() const
{
    const int width = std::max(0, GetClientSize().x);
    const int headerHeight = HeaderHeightPx();
    const int padX = Dip(this, S::AccBodyPadX);
    const int padBottom = Dip(this, S::AccBodyPadBottom);
    int height = 0;
    int naturalWidth = width;
    for (size_t i = 0; i < m_sections.size(); ++i) {
        height += headerHeight;
        const Section& section = m_sections[i];
        if (section.content == nullptr) {
            continue;
        }
        if (section.expanded) {
            const int bodyHeight = BodyHeightPx(static_cast<int>(i));
            height += bodyHeight + padBottom;
            naturalWidth = std::max(naturalWidth, section.content->GetBestSize().x + 2 * padX);
        }
    }
    return wxSize(std::max(naturalWidth, Dip(this, M::KvKeyWidth)), height);
}

void AgentAccordion::OnPaint(wxPaintEvent&)
{
    // 容器只铺一层卡片玻璃面，分隔线由各标题行自己画（H143 .acc border-bottom）：
    // 这里刻意**不画外框**，避免出现「圆角内又套一个方框」（LAY-009）。
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(tokens::SurfaceColour(Component::Card)));
    dc.Clear();
}

}  // namespace agent::gui::controls
