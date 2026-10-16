//
// UiToolBarArt.cpp — 工具栏 / 活动栏 chip 外观实现（T-230 / T-240）
//

#include "ui/UiToolBarArt.h"

#include "ui/UiFocusRing.h"
#include "ui/UiTheme.h"

#include <wx/control.h>  // wxControl::Ellipsize
#include <wx/dc.h>  // aui.h 只前置声明 wxDC；绘制需要完整类型
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/timer.h>
#include <wx/window.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#include <algorithm>
#include <memory>

namespace {

// chip 内部间距的**唯一来源**：GetToolSize（算尺寸）与 DrawButton（画内容）必须用同一组值，
// 否则就会出现"预留的宽度小于绘制所需"→ 文字被截断成一个字母（2026-10-10 用户实测）。
int ChipPadX(const wxWindow* wnd)
{
    return wnd->FromDIP(UiTheme::Skeleton::ChipPadX);
}

int ChipInnerGap(const wxWindow* wnd)
{
    return wnd->FromDIP(UiTheme::Spacing::S1);
}

bool PressAnimationEnabled()
{
#ifdef __WXMSW__
    BOOL enabled = TRUE;
    if (::SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
        return enabled != FALSE;
    }
#endif
    // 无法探测减少动画偏好时采用静态反馈。
    return false;
}

wxColour GlassOpacity(const wxColour& colour, double opacity)
{
    return wxColour(colour.Red(), colour.Green(), colour.Blue(),
                    static_cast<unsigned char>(colour.Alpha() * opacity));
}

// 单色图标按当前 token 重着色。资源 SVG 自带固定填充色，wx 侧无法直接改色；
// 这里按 alpha 掩码整幅替换 RGB，既能让活动栏选中态变 --accent（CMP-028），
// 又让图标在深色档下跟随 --text-1，而无需为每个主题各出一套资源。
wxBitmap TintMonochrome(const wxBitmap& source, const wxColour& target)
{
    if (!source.IsOk()) return source;
    wxImage image = source.ConvertToImage();
    if (!image.IsOk() || !image.HasAlpha()) return source;

    unsigned char* data = image.GetData();
    const unsigned char* alpha = image.GetAlpha();
    const long pixels = static_cast<long>(image.GetWidth()) * image.GetHeight();
    for (long i = 0; i < pixels; ++i) {
        if (alpha[i] == 0) continue;  // 保留透明区，抗锯齿边缘仍按 alpha 合成
        data[i * 3]     = target.Red();
        data[i * 3 + 1] = target.Green();
        data[i * 3 + 2] = target.Blue();
    }
    return wxBitmap(image);
}

// 颜色混合：t = 第二色的权重（0..1）。用于由 --brand 派生 Synthesize 的
// 渐变/描边，避免在源码里写死十六进制（SAFE-012）。
wxColour MixColour(const wxColour& a, const wxColour& b, double t)
{
    const auto ch = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(x * (1.0 - t) + y * t + 0.5);
    };
    return wxColour(ch(a.Red(), b.Red()), ch(a.Green(), b.Green()), ch(a.Blue(), b.Blue()));
}

// Soft composited shadow and directional bevels replace the old concentric
// rings. All colours still derive from the glass tokens; no platform blur.
//
// DEC-14：聚焦时保留可见灰线；蓝边仅在 hover / 按压期间出现。
void DrawPrimaryChip(wxGraphicsContext& gc, const wxRect2DDouble& r,
                     double dip, bool hover, bool pressed, bool focus, bool disabled)
{
    const auto& c = UiTheme::Palette();
    const double radius = r.m_height / 2.0;
    gc.SetPen(*wxTRANSPARENT_PEN);
    // Overlapping translucent fills give a continuous falloff, without hard
    // shadow outlines. Stay within the existing 4 DIP control margin.
    for (int i = 8; i > 0; --i) {
        const double grow = i * .35 * dip;
        gc.SetBrush(wxBrush(GlassOpacity(c.glassShadow, disabled ? .025 : (hover ? .10 : .065))));
        gc.DrawRoundedRectangle(r.m_x - grow, r.m_y + dip - grow * .5,
                                r.m_width + grow * 2, r.m_height + grow,
                                radius + grow * .5);
    }
    // Frosted body: a quiet vertical gradient, with green confined to the icon.
    gc.SetBrush(gc.CreateLinearGradientBrush(r.m_x, r.m_y, r.m_x, r.GetBottom(),
                                             c.strongOnPanel, c.surfaceOnWindow));
    gc.DrawRoundedRectangle(r.m_x, r.m_y, r.m_width, r.m_height, radius);
    auto body = gc.CreatePath();
    body.AddRoundedRectangle(r.m_x, r.m_y, r.m_width, r.m_height, radius);
    wxGraphicsGradientStops sheen(GlassOpacity(c.glassEdge, .65), GlassOpacity(c.glassRim, .45));
    sheen.Add(GlassOpacity(c.glassEdge, 0), .55f);
    gc.SetBrush(gc.CreateLinearGradientBrush(r.m_x, r.m_y, r.GetRight(), r.GetBottom(), sheen));
    gc.FillPath(body);

    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.glassBorder).Width(.8*dip)));
    gc.StrokePath(body);
    // Two directional edge glints, rather than additional closed rings.
    auto top = gc.CreatePath();
    top.MoveToPoint(r.m_x + dip, r.m_y + radius);
    top.AddCurveToPoint(r.m_x + dip, r.m_y + radius*.45,
                        r.m_x + radius*.45, r.m_y + dip, r.m_x + radius, r.m_y + dip);
    top.AddLineToPoint(r.GetRight() - radius, r.m_y + dip);
    top.AddCurveToPoint(r.GetRight() - radius*.45, r.m_y + dip,
                        r.GetRight() - dip, r.m_y + radius*.45, r.GetRight() - dip, r.m_y + radius);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.glassEdgeStrong).Width(dip)));
    gc.StrokePath(top);
    auto lower = gc.CreatePath();
    lower.MoveToPoint(r.m_x + radius*.45, r.GetBottom() - radius*.2);
    lower.AddCurveToPoint(r.m_x + radius*.65, r.GetBottom() - dip,
                          r.m_x + radius*.8, r.GetBottom() - dip, r.m_x + radius, r.GetBottom() - dip);
    lower.AddLineToPoint(r.GetRight() - radius, r.GetBottom() - dip);
    lower.AddCurveToPoint(r.GetRight() - radius*.8, r.GetBottom() - dip,
                          r.GetRight() - radius*.65, r.GetBottom() - dip,
                          r.GetRight() - radius*.45, r.GetBottom() - radius*.2);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(GlassOpacity(c.glassRing, disabled ? .05 : .13)).Width(.8*dip)));
    gc.StrokePath(lower);
    if (!disabled && (hover || pressed)) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.focusRingOnPanel).Width(1.5*dip)));
        gc.DrawRoundedRectangle(r.m_x, r.m_y, r.m_width, r.m_height, radius);
    } else if (!disabled && focus) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.text3).Width(dip)));
        gc.StrokeLine(r.m_x + radius, r.GetBottom() - 3*dip,
                       r.GetRight() - radius, r.GetBottom() - 3*dip);
    }
}

// Synthesize 主 CTA（app.html 的 .lg-glass 在 wx 侧的近似）：
//   绿色垂直渐变本体 + 顶部 1px 白高光 + 深绿描边 + 外投影，
//   白字白图标。这是全工具栏唯一保留"玻璃"复合工艺的按钮（材质三条之唯一例外）。
void DrawSynthesizeChip(wxGraphicsContext& gc, const wxRect2DDouble& r, double dip,
                        bool hover, bool pressed, bool focus, bool disabled)
{
    const auto& c = UiTheme::Palette();
    const double radius = r.m_height / 2.0;
    const wxColour bodyTop = MixColour(c.brand, wxColour(255, 255, 255), 0.08);
    const wxColour bodyBottom = c.brand;
    const wxColour edge = MixColour(c.brand, wxColour(0, 0, 0), 0.30);
    const wxColour ink = disabled ? c.text3 : wxColour(255, 255, 255);

    // 1) 外投影：层叠的半透明圆角矩形近似 0 4px 8px / hover 0 8px 20px。
    gc.SetPen(*wxTRANSPARENT_PEN);
    const int layers = hover ? 5 : 3;
    for (int i = layers; i >= 1; --i) {
        const double grow = i * 1.1 * dip;
        const unsigned char alpha = static_cast<unsigned char>(hover ? 20 : 14);
        gc.SetBrush(wxBrush(wxColour(0, 0, 0, alpha)));
        gc.DrawRoundedRectangle(r.m_x - grow, r.m_y + grow * 0.55,
                                r.m_width + grow * 2.0, r.m_height + grow,
                                radius + grow * 0.5);
    }

    // 2) 绿色垂直渐变本体。
    gc.SetBrush(gc.CreateLinearGradientBrush(r.m_x, r.m_y, r.m_x, r.GetBottom(),
                                             bodyTop, bodyBottom));
    gc.DrawRoundedRectangle(r.m_x, r.m_y, r.m_width, r.m_height, radius);

    // 3) 顶部 30% 白内高光（lg-glass 的顶部高光层）。
    if (!disabled) {
        auto sheen = gc.CreatePath();
        const double sheenH = r.m_height * 0.30;
        sheen.AddRoundedRectangle(r.m_x + dip, r.m_y + dip, r.m_width - 2.0 * dip,
                                  sheenH, std::max(1.0, radius - dip));
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(wxColour(255, 255, 255, hover ? 62 : 48)));
        gc.FillPath(sheen);
    }

    // 4) 深绿描边（近似双向内描边 + 暗内环）。
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(edge).Width(std::max(1.0, dip))));
    gc.DrawRoundedRectangle(r.m_x, r.m_y, r.m_width, r.m_height, radius);

    // 5) 焦点/悬停反馈。
    if (!disabled && (hover || pressed)) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(c.focusRingOnPanel).Width(1.5 * dip)));
        gc.DrawRoundedRectangle(r.m_x - dip, r.m_y - dip, r.m_width + 2.0 * dip,
                                r.m_height + 2.0 * dip, radius + dip);
    } else if (!disabled && focus) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink).Width(dip)));
        gc.StrokeLine(r.m_x + radius, r.GetBottom() - 3.0 * dip,
                      r.GetRight() - radius, r.GetBottom() - 3.0 * dip);
    }
}

class SynthesizeButton final : public wxControl
{
public:
    SynthesizeButton(wxWindow* parent, wxWindowID id)
        : wxControl(parent, id, wxDefaultPosition, wxDefaultSize,
                    wxBORDER_NONE | wxWANTS_CHARS), m_timer(this)
    {
        SetName("Synthesize");
        SetLabel("Synthesize");
        SetToolTip("Open FPGA synthesis tools");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        UpdateSize();
        m_theme = UiTheme::Subscribe([this] { Refresh(false); });
        Bind(wxEVT_PAINT, &SynthesizeButton::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { UiFocusTrace::NotePointer(); m_hover = true; Refresh(false); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { UiFocusTrace::NotePointer(); m_hover = false; Refresh(false); });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
            // A11Y-001：键盘来的焦点画 accent 环；点击后保留的焦点只留灰色下划线（DEC-14）。
            m_focusTracker.OnFocusGained();
            Refresh(false);
            e.Skip();
        });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
            m_keyPressed = false;
            m_focusTracker.OnFocusLost();
            if (HasCapture()) ReleaseMouse();
            Animate(false);
            e.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            UiFocusTrace::NotePointer();
            if (!IsEnabled()) return;
            SetFocus(); m_mousePressed = true;
            if (!HasCapture()) CaptureMouse();
            Animate(true);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
            const bool activate = m_mousePressed && GetClientRect().Contains(e.GetPosition());
            m_mousePressed = false;
            if (HasCapture()) ReleaseMouse();
            Animate(false);
            if (activate) Activate();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_mousePressed = false; Animate(false);
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            UiFocusTrace::NoteKeyboard();
            if (e.GetKeyCode() != WXK_SPACE && e.GetKeyCode() != WXK_RETURN) { e.Skip(); return; }
            if (!m_keyPressed && IsEnabled()) { m_keyPressed = true; Animate(true); }
        });
        Bind(wxEVT_KEY_UP, [this](wxKeyEvent& e) {
            UiFocusTrace::NoteKeyboard();
            if (e.GetKeyCode() != WXK_SPACE && e.GetKeyCode() != WXK_RETURN) { e.Skip(); return; }
            const bool activate = m_keyPressed; m_keyPressed = false; Animate(false);
            if (activate) Activate();
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            m_elapsed = std::min(UiTheme::Motion::Press, m_elapsed + 16);
            const double t = static_cast<double>(m_elapsed) / UiTheme::Motion::Press;
            m_scale = m_startScale + (m_targetScale - m_startScale) * (1.0 - (1.0-t)*(1.0-t));
            if (m_elapsed == UiTheme::Motion::Press) m_timer.Stop();
            Refresh(false);
        });
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) { UpdateSize(); Refresh(false); e.Skip(); });
    }

private:
    void UpdateSize()
    {
        SetFont(UiTheme::UiFont(this, UiTheme::TextSize::P13, UiTheme::Weight::Medium));
        wxClientDC dc(this); dc.SetFont(GetFont());
        const wxSize size(dc.GetTextExtent(GetLabel()).x + FromDIP(50),
                          FromDIP(UiTheme::Skeleton::ToolBarChipH + 8));
        SetMinSize(size); SetSize(size);
    }
    void Animate(bool pressed)
    {
        m_startScale = m_scale; m_targetScale = pressed ? 0.97 : 1.0; m_elapsed = 0;
        if (PressAnimationEnabled()) m_timer.Start(16);
        else { m_timer.Stop(); m_scale = m_targetScale; }
        Refresh(false);
    }
    void Activate()
    {
        if (!IsEnabled()) return;
        wxCommandEvent event(wxEVT_TOOL, GetId());
        event.SetEventObject(this);
        GetEventHandler()->ProcessEvent(event);
    }
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const auto& c = UiTheme::Palette();
        dc.SetBackground(wxBrush(c.surfaceOnWindow)); dc.Clear();
        const auto size = GetClientSize();
        if (size.x < FromDIP(40) || size.y < FromDIP(20)) return;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsRenderer::GetDefaultRenderer()->CreateContext(dc));
        if (!gc) { dc.SetFont(GetFont()); dc.SetTextForeground(c.text1); dc.DrawText(GetLabel(), FromDIP(8), FromDIP(8)); return; }
        gc->Translate(size.x / 2.0, size.y / 2.0);
        gc->Scale(m_scale, m_scale);
        gc->Translate(-size.x / 2.0, -size.y / 2.0);
        const double dip = FromDIP(100) / 100.0;
        // 注意用 IsThisEnabled()：MSW 上 IsEnabled() 会连带祖先窗口（工程加载期
        // 主窗曾被短暂禁用），会让主 CTA 被画成灰色且不重绘。这里只按自身启用态取色。
        const bool ctaEnabled = IsThisEnabled();
        DrawSynthesizeChip(*gc, wxRect2DDouble(4*dip, 4*dip, size.x-8*dip, size.y-8*dip),
                        dip, m_hover, m_mousePressed || m_keyPressed, HasFocus(), !ctaEnabled);
        // 白色的综合逻辑符号；不依赖位图缓存，规避 CMP-082 空窗口断言。
        const wxColour synthInk = ctaEnabled ? wxColour(255, 255, 255) : c.text3;
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(synthInk).Width(1.5*dip)));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(14*dip, size.y/2.0-5*dip, 12*dip, 10*dip, 2*dip);
        for (int i = -1; i <= 1; ++i) {
            gc->StrokeLine(11*dip, size.y/2.0+i*3*dip, 14*dip, size.y/2.0+i*3*dip);
            gc->StrokeLine(26*dip, size.y/2.0+i*3*dip, 29*dip, size.y/2.0+i*3*dip);
        }
        gc->SetFont(GetFont(), synthInk);
        dc.SetFont(GetFont());
        const wxString label = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
                                                    std::max(1, size.x - FromDIP(42)));
        double width = 0, height = 0;
        gc->GetTextExtent(label, &width, &height);
        gc->DrawText(label, 34*dip, (size.y-height)/2.0);
    }
    bool m_hover = false, m_mousePressed = false, m_keyPressed = false;
    UiFocusRing::Tracker m_focusTracker;  ///< T-620：键盘焦点 vs 点击后焦点保留
    double m_scale = 1.0, m_startScale = 1.0, m_targetScale = 1.0;
    int m_elapsed = 0;
    wxTimer m_timer;
    UiTheme::Subscription m_theme;
};

}  // namespace

wxControl* UiToolBarArt::CreateSynthesizeButton(wxWindow* parent, wxWindowID id)
{
    return new SynthesizeButton(parent, id);
}

UiToolBarArt::UiToolBarArt()
{
    ApplyTheme();
}

void UiToolBarArt::ApplyTheme()
{
    // 字体走 token（CMP-021：chip 文字 13 DIP / medium）
    SetFont(UiTheme::UiFont(nullptr, UiTheme::TextSize::P13, UiTheme::Weight::Medium));

    // 文字方向：图标在左、文字在右（与设计稿 chip 一致，也与 DrawButton 的排版一致）。
    // 只有工具栏带 wxAUI_TB_TEXT 时才会真正为文字预留宽度（见 MainFrame 的 topBar 样式）。
    SetTextOrientation(wxAUI_TBTOOL_TEXT_RIGHT);
}

wxSize UiToolBarArt::GetToolSize(wxDC& dc, wxWindow* wnd, const wxAuiToolBarItem& item)
{
    // 防御：某些布局路径可能不传窗口；此时避免触碰位图（GetBitmapFor(nullptr) 会触发 wx 断言），
    // 退化为一个保守尺寸。
    if (wnd == nullptr) {
        return wxSize(dc.GetCharWidth() * 5, dc.GetCharHeight() + dc.GetCharHeight() / 2);
    }

    const bool vertical = (m_flags & wxAUI_TB_VERTICAL) != 0;

    wxSize bitmapSize(0, 0);
    const wxBitmap bundleBitmap = item.GetBitmapBundle().GetBitmapFor(wnd);
    if (bundleBitmap.IsOk()) {
        bitmapSize = bundleBitmap.GetSize();
    }

    const int padX = ChipPadX(wnd);
    const int padY = wnd->FromDIP(UiTheme::Spacing::S1);

    // 活动栏（纵向）：按键固定 34×34（LAY-003 / CMP-032）
    if (vertical) {
        const int key = wnd->FromDIP(UiTheme::Skeleton::RailKeySize);
        const int gap = wnd->FromDIP(UiTheme::Skeleton::RailKeyGap);
        return wxSize(std::max(key, bitmapSize.x + padX * 2),
                      std::max(key, bitmapSize.y + padY * 2) + gap);
    }

    // 顶部工具栏（横向 chip）：宽度 = 内距×2 + 图标 + 间距 + 文字（CMP-021）。
    // 这里**不再调用基类**：基类只按 3 DIP 的间距估算，与 DrawButton 的实际排版不一致。
    wxString label = ((m_flags & wxAUI_TB_TEXT) != 0) ? item.GetLabel() : wxString();

    wxSize textSize(0, 0);
    if (!label.empty()) {
        dc.SetFont(UiTheme::UiFont(wnd, UiTheme::TextSize::P13, UiTheme::Weight::Medium));
        textSize = dc.GetTextExtent(label);
    }

    const int gap = (bitmapSize.x > 0 && textSize.x > 0) ? ChipInnerGap(wnd) : 0;

    wxSize size;
    size.x = padX * 2 + bitmapSize.x + gap + textSize.x;
    size.y = std::max(wnd->FromDIP(UiTheme::Skeleton::ToolBarChipH), bitmapSize.y + padY * 2);
    return size;
}

void UiToolBarArt::DrawBackground(wxDC& dc, wxWindow* /*wnd*/, const wxRect& rect)
{
    // 模块栏底 = **纯白**（与模块面板一致；页面冷灰渐变只在卡片之外的缝隙里可见）。
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    dc.DrawRectangle(rect);
}

void UiToolBarArt::DrawPlainBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect)
{
    // wxAUI_TB_PLAIN_BACKGROUND bypasses DrawBackground. Both paths must share
    // the control surface, otherwise custom children sit on pale rectangles.
    DrawBackground(dc, wnd, rect);
}

void UiToolBarArt::DrawSeparator(wxDC& dc, wxWindow* wnd, const wxRect& rect)
{
    // app.html `.sep`：1×18 DIP、--stroke 色，垂直居中。
    const int height = std::min(rect.height, wnd->FromDIP(18));
    const int x = rect.x + rect.width / 2;
    const int top = rect.y + (rect.height - height) / 2;
    dc.SetPen(wxPen(UiTheme::Palette().strokeOnWindow, std::max(1, wnd->FromDIP(1))));
    dc.DrawLine(x, top, x, top + height);
}

void UiToolBarArt::DrawChipBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect, bool disabled,
                                      bool hover, bool pressed, bool checked) const
{
    const UiTheme::Colours& c = UiTheme::Palette();

    // DEC-14: selection keeps its fill/indicator, blue edges are transient.
    wxColour fill = c.surfaceOnWindow;
    wxColour stroke = c.strokeOnWindow;
    if (disabled) {
        fill = c.bgPanel;
        stroke = c.strokeOnPanel;
    } else if (pressed || checked) {
        fill = c.selOnPanel;
        stroke = c.focusRingOnPanel;  // 选中：淡蓝描边（图2 的圆角方形选中态）
    } else if (hover) {
        fill = c.strongOnPanel;
    }
    if (!disabled && (hover || pressed)) stroke = c.focusRingOnPanel;

    // 顶部工具栏 chip = 胶囊（radius = 高/2）；活动栏按键 = **圆角正方形**（不要椭圆）。
    wxRect shape = rect;
    if (pressed) {
        shape.Deflate(std::max(1, rect.width / 66), std::max(1, rect.height / 66));
    }
    const bool vertical = (m_flags & wxAUI_TB_VERTICAL) != 0;
    const double radius = vertical
        ? static_cast<double>(wnd->FromDIP(UiTheme::Radius::SM))
        : shape.height / 2.0;
    dc.SetPen(wxPen(stroke, 1));
    dc.SetBrush(wxBrush(fill));
    dc.DrawRoundedRectangle(shape, radius);
}

void UiToolBarArt::DrawButton(wxDC& dc, wxWindow* wnd, const wxAuiToolBarItem& item,
                              const wxRect& rect)
{
    const UiTheme::Colours& c = UiTheme::Palette();
    const int state = item.GetState();
    const bool disabled = (state & wxAUI_BUTTON_STATE_DISABLED) != 0;
    const bool hover = (state & wxAUI_BUTTON_STATE_HOVER) != 0;
    const bool pressed = (state & wxAUI_BUTTON_STATE_PRESSED) != 0;
    const bool checked = (state & wxAUI_BUTTON_STATE_CHECKED) != 0;
    const bool wantFocusRing = m_keyboardFocusTool != -1 && item.GetId() == m_keyboardFocusTool &&
                              (item.GetKind() == wxITEM_NORMAL || item.GetKind() == wxITEM_CHECK ||
                               item.GetKind() == wxITEM_RADIO);

    // 1) chip 底 + 描边
    //    活动栏（纵向）：只有**选中/悬停/按压**项才有圆角方形蓝底，未选中不画任何灰框（用户要求，图2）；
    //    顶部工具栏（横向）：保持常规 chip。
    const bool verticalBar = (m_flags & wxAUI_TB_VERTICAL) != 0;
    const bool wantsChip = (item.GetKind() == wxITEM_NORMAL) || (item.GetKind() == wxITEM_CHECK);
    const bool drawChip = verticalBar ? (checked || hover || pressed) : wantsChip;

    // 活动栏：把按键画成**正中心的正方形**（图6），并在上下留出间距。
    wxRect chipRect = rect;
    if (verticalBar && wnd != nullptr) {
        const int gap = wnd->FromDIP(UiTheme::Skeleton::RailKeyGap);
        const int side = std::max(1, std::min(rect.width,
                                             std::max(1, rect.height - gap)));
        chipRect = wxRect(rect.x + (rect.width - side) / 2,
                          rect.y + (rect.height - side) / 2,
                          side, side);
    }

    if (drawChip) {
        DrawChipBackground(dc, wnd, chipRect, disabled, hover, pressed, checked);
    }

    // 2) 活动栏 / 工具选中指示条（CMP-028：3 × 18 DIP、圆角 2、accent）
    if (checked) {
        const bool vertical = (m_flags & wxAUI_TB_VERTICAL) != 0;
        const int markW = wnd->FromDIP(UiTheme::Radius::RailMark);
        const int markH = std::max(1, std::min(wnd->FromDIP(UiTheme::Radius::RailMarkL),
                                               std::max(1, chipRect.height)));
        // 活动栏：指示条贴在**栏的最左侧边界**（x = 0 = 工具栏客户区左缘），竖直居中对齐按键。
        const int markX = vertical ? 0 : chipRect.x + wnd->FromDIP(2);
        const int markY = vertical
            ? chipRect.y + (chipRect.height - markH) / 2
            : chipRect.y + wnd->FromDIP(2) +
                  (std::max(1, chipRect.height - wnd->FromDIP(4)) - markH) / 2;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(c.accent));
        dc.DrawRoundedRectangle(markX, markY, markW, markH,
                                wnd->FromDIP(UiTheme::Radius::Indicator));
    }

    // 3) 内容：图标（+ 文字）居中排版。基类不再调用，避免覆盖上面绘制的底。
    //
    // ⚠️ 必须使用 *For(wnd) 变体，不能用 item.GetBitmap()/GetDisabledBitmap()：
    //    非 For 变体内部走 wxAuiToolBarItem 自己缓存的 m_window，在 wxAuiToolBar::OnPaint
    //    这条路径下该指针为空，会触发 wxBitmapBundle 的断言
    //      bmpbndl.cpp: assert "window" failed in GetPreferredBitmapSizeFor()
    //    → wxTrap/__debugbreak → 进程直接退出（用户表现为"点进工程后界面卡退"）。
    //    基类 wxAuiGenericToolBarArt 正是使用传入的 wnd 参数，这里与之保持一致。
    const wxBitmap bitmap = disabled ? item.GetDisabledBitmapFor(wnd) : item.GetBitmapFor(wnd);

    // CMP-028：活动栏选中项图标用 --accent；其余用 --text-1。禁用态保留原样。
    const wxBitmap tinted = disabled || !bitmap.IsOk()
        ? bitmap
        : TintMonochrome(bitmap, checked ? c.accent : c.text1);

    // ⚠️ 文字只在工具栏**确实启用 wxAUI_TB_TEXT** 时绘制：
    //    否则工具栏的 GetToolSize 不会为文字预留宽度，绘制会溢出到相邻按钮上
    //    （2026-10-10 用户实测："工具栏组件太小、容不下文字"，看到的就是相邻标签互相重叠）。
    wxString label = ((m_flags & wxAUI_TB_TEXT) != 0) ? item.GetLabel() : wxString();

    dc.SetFont(UiTheme::UiFont(wnd, UiTheme::TextSize::P13, UiTheme::Weight::Medium));

    // 与 GetToolSize 共用同一组内距（单一来源，二者不一致会导致文字被过早截断）
    const int padX = ChipPadX(wnd);
    const int gap = ChipInnerGap(wnd);

    wxSize bitmapSize(0, 0);
    if (tinted.IsOk()) {
        bitmapSize = tinted.GetSize();
    }

    wxSize labelSize(0, 0);
    if (!label.empty()) {
        const int available =
            chipRect.width - bitmapSize.x - (bitmapSize.x > 0 ? gap : 0) - padX * 2;
        if (available <= gap) {
            label.clear();  // 放不下就宁可不画，也不溢出到相邻按钮
        } else {
            labelSize = dc.GetTextExtent(label);
            if (labelSize.x > available) {
                // 截断而非溢出（wxControl::Ellipsize 需要 <wx/control.h>）
                label = wxControl::Ellipsize(label, dc, wxELLIPSIZE_END, available);
                labelSize = dc.GetTextExtent(label);
            }
        }
    }

    const int textGap = (bitmapSize.x > 0 && labelSize.x > 0) ? gap : 0;
    const int totalWidth = bitmapSize.x + textGap + labelSize.x;
    const int centerY = chipRect.y + chipRect.height / 2;

    int cursorX = chipRect.x + (chipRect.width - totalWidth) / 2;
    if (tinted.IsOk()) {
        dc.DrawBitmap(tinted, cursorX, centerY - bitmapSize.y / 2, true);
        cursorX += bitmapSize.x + textGap;
    }
    if (!label.empty()) {
        dc.SetTextForeground(disabled ? c.text3 : c.text1);
        dc.DrawText(label, cursorX, centerY - labelSize.y / 2);
    }

    // 4) T-620：逐项键盘导航保留灰线焦点提示（DEC-14），不增加常驻蓝边。
    if (!disabled && wantFocusRing && wnd != nullptr) {
        dc.SetPen(wxPen(c.text3, std::max(1, wnd->FromDIP(1))));
        const int inset = wnd->FromDIP(10);
        const int y = rect.GetBottom() - wnd->FromDIP(4);
        if (rect.width > 2 * inset) {
            dc.DrawLine(rect.x + inset, y, rect.GetRight() - inset, y);
        }
    }
}
