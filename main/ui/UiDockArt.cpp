//
// UiDockArt.cpp — AUI 停靠外观实现（T-130）
//

#include "ui/UiDockArt.h"

#include "ui/UiTheme.h"

#include <wx/dc.h>

#include <algorithm>

UiDockArt::UiDockArt()
{
    // --- 静态属性：不随 DPI 与主题变化（THE-031：无渐变、无拖拽柄）---
    SetMetric(wxAUI_DOCKART_GRADIENT_TYPE, wxAUI_GRADIENT_NONE);
    // RV-T09: the rounded UiRegionHost owns the region outline.  Leaving the
    // native AUI pane border enabled creates a second square frame inside it.
    SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, 0);
    SetMetric(wxAUI_DOCKART_GRIPPER_SIZE, 0);  // 彻底去掉拖拽柄
    SetMetric(wxAUI_DOCKART_SASH_SIZE, wxWindow::FromDIP(16, nullptr));

    // --- 颜色：统一取自 UiTheme（T-110 起生效；此处为唯一取色点）---
    ApplyThemeColours();
}

void UiDockArt::ApplyThemeColours()
{
    const UiTheme::Colours& c = UiTheme::Palette();

    SetColour(wxAUI_DOCKART_BACKGROUND_COLOUR, c.bgWindow);
    // Gap between cards: paint the pane border with the page colour so the
    // rounded UiRegionHost card reads as a card floating on the page.
    SetColour(wxAUI_DOCKART_BORDER_COLOUR, c.bgWindow);
    SetColour(wxAUI_DOCKART_SASH_COLOUR, c.bgWindow);
    // ⚠️ caption 是**功能部件**而非装饰：wxAUI 只能由 caption part 起拖
    // （`LayoutAddPane` 仅在 `pane.HasCaption()` 时分配，见 SPEC **LAY-031**）。
    // 之前把 caption 底色设成 `surfaceOnWindow`（= pane 卡片同色）+ `text3` 灰字，
    // 整条"抓取条"在视觉上消失了 → 用户找不到可以拖的地方（实测反馈：
    // "整个右侧边栏不能像 Terminal 那样自由脱出"）。改为用**页面色**做底，
    // 与卡片形成一条看得见的分隔条，标题用 text2/text1。
    SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, c.bgWindow);
    SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_COLOUR, c.bgWindow);
    SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, c.text2);
    SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR, c.text1);
}

void UiDockArt::UpdateMetrics(wxWindow* parent)
{
    if (parent == nullptr) {
        return;
    }
    // sash 用细线（THE-031 / 原实现 FromDIP(2)）
    SetMetric(wxAUI_DOCKART_SASH_SIZE, parent->FromDIP(2));
    // LAY: 6 DIP page-coloured gutter around every pane (card look).
    SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, parent->FromDIP(2));

    // ⚠️ caption 高度**显式设定**（基类默认 FromDIP(17)；这里按设计给 26 DIP）。
    //    必须保证 > 0：为 0 时 wxAUI 不分配 caption part → 该 pane **完全无法拖放**
    //    （SPEC LAY-031；实测反馈"整个右侧边栏不能像 Terminal 那样自由脱出"）。
    SetMetric(wxAUI_DOCKART_CAPTION_SIZE, parent->FromDIP(26));

    // caption 字体走 token（THE-016/THE-017：12 DIP、medium）
    SetFont(wxAUI_DOCKART_CAPTION_FONT,
            UiTheme::UiFont(parent, UiTheme::TextSize::P12, UiTheme::Weight::Medium));
}

void UiDockArt::DrawBackground(wxDC& dc, wxWindow* window, int orientation, const wxRect& rect)
{
    if (window == nullptr) {
        wxAuiDefaultDockArt::DrawBackground(dc, window, orientation, rect);
        return;
    }
    // wxAUI 每次只给一小块 rect，但所有块共用同一个"顶层客户区"锚点，
    // 拼起来就是 app.html §0 的那一条 160° 冷灰渐变。
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(UiTheme::Palette().bgWindow));
    dc.DrawRectangle(rect);
    UiTheme::PaintPageGradient(dc, window->GetClientSize(), wxPoint(0, 0), rect);
}

void UiDockArt::DrawSash(wxDC& dc, wxWindow* window, int orientation, const wxRect& rect)
{
    if (window == nullptr) {
        wxAuiDefaultDockArt::DrawSash(dc, window, orientation, rect);
        return;
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(UiTheme::Palette().bgWindow));
    dc.DrawRectangle(rect);
    UiTheme::PaintPageGradient(dc, window->GetClientSize(), wxPoint(0, 0), rect);
}

void UiDockArt::DrawBorder(wxDC& dc, wxWindow* window, const wxRect& rect, wxAuiPaneInfo& pane)
{
    if (window == nullptr) {
        wxAuiDefaultDockArt::DrawBorder(dc, window, rect, pane);
        return;
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(UiTheme::Palette().bgWindow));
    dc.DrawRectangle(rect);
    UiTheme::PaintPageGradient(dc, window->GetClientSize(), wxPoint(0, 0), rect);
}

void UiDockArt::DrawPaneButton(wxDC& dc, wxWindow* window, int button, int buttonState,
                               const wxRect& rect, wxAuiPaneInfo& pane)
{
    const UiTheme::Colours& c = UiTheme::Palette();
    const int unit = window != nullptr ? std::max(1, window->FromDIP(1)) : 1;

    // 扁平 hover / 按压底，替代基类的 3D 位图按钮（THE-031：无立体玻璃）。
    if ((buttonState & (wxAUI_BUTTON_STATE_HOVER | wxAUI_BUTTON_STATE_PRESSED)) != 0) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(c.hoverOnPanel));
        dc.DrawRoundedRectangle(rect, window != nullptr ? window->FromDIP(UiTheme::Radius::XS)
                                                        : UiTheme::Radius::XS);
    }

    const bool disabled = (buttonState & wxAUI_BUTTON_STATE_DISABLED) != 0;
    dc.SetPen(wxPen(disabled ? c.text3 : c.text2, unit));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    // 图形只在按钮矩形内缩后绘制，避免贴边或被裁切。
    const int inset = std::max(unit * 3, std::min(rect.width, rect.height) / 4);
    const int side = std::max(unit, std::min(rect.width, rect.height) - inset * 2);
    const wxRect glyph(rect.x + (rect.width - side) / 2, rect.y + (rect.height - side) / 2,
                       side, side);

    switch (button) {
    case wxAUI_BUTTON_MAXIMIZE_RESTORE:
        dc.DrawRectangle(glyph);
        break;
    case wxAUI_BUTTON_CLOSE:
        dc.DrawLine(glyph.GetLeft(), glyph.GetTop(), glyph.GetRight(), glyph.GetBottom());
        dc.DrawLine(glyph.GetRight(), glyph.GetTop(), glyph.GetLeft(), glyph.GetBottom());
        break;
    case wxAUI_BUTTON_MINIMIZE:
        dc.DrawLine(glyph.GetLeft(), glyph.GetBottom(), glyph.GetRight(), glyph.GetBottom());
        break;
    case wxAUI_BUTTON_PIN:
        dc.DrawCircle(glyph.x + glyph.width / 2, glyph.y + glyph.height / 2,
                      std::max(unit, glyph.width / 2));
        break;
    default:
        // 其余按钮（自定义 ID 等）保持基类行为。
        wxAuiDefaultDockArt::DrawPaneButton(dc, window, button, buttonState, rect, pane);
        break;
    }
}
