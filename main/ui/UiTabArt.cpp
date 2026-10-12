//
// UiTabArt.cpp — AUI 笔记本页签外观实现（T-130）
//

#include "ui/UiTabArt.h"

#include "ui/UiTheme.h"

#include <wx/dc.h>

UiTabArt::UiTabArt()
{
    ApplyTheme();
}

void UiTabArt::ApplyTheme()
{
    const UiTheme::Colours& c = UiTheme::Palette();

    // 页签底色 = 结构面；激活页 = 更亮一档（THE-031：去掉渐变后的平面表达）
    SetColour(c.bgPanel);
    SetActiveColour(c.strongOnPanel);

    // 字号与字重走 token（CMP-012：12 DIP；激活页 medium）
    SetNormalFont(UiTheme::UiFont(nullptr, UiTheme::TextSize::P12, UiTheme::Weight::Normal));
    SetSelectedFont(UiTheme::UiFont(nullptr, UiTheme::TextSize::P12, UiTheme::Weight::Medium));
    SetMeasuringFont(UiTheme::UiFont(nullptr, UiTheme::TextSize::P12, UiTheme::Weight::Normal));
}

// T-719 / V9：让绘制级实例也是本类（否则它是 wxAuiGenericTabArt 的拷贝，永不取新 token）
wxAuiTabArt* UiTabArt::Clone()
{
    return new UiTabArt();
}

void UiTabArt::DrawBorder(wxDC& dc, wxWindow*, const wxRect& rect)
{
    // Keep the notebook surface flat and let UiRegionHost draw the only
    // visible region outline.  In particular, do not call the generic
    // implementation here: it emits a square one-pixel frame.
    const wxColour surface = UiTheme::Palette().bgPanel;
    dc.SetPen(wxPen(surface));
    dc.SetBrush(wxBrush(surface));
    dc.DrawRectangle(rect);
}

void UiTabArt::DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect)
{
    ApplyTheme();  // 每帧按当前调色板自刷新（覆盖克隆体与未显式调用 ApplyTheme 的实例）

    // Do not delegate to wxAuiGenericTabArt here.  Its background painter
    // emits a straight separator under the tab strip, which becomes a second
    // rectangular frame inside UiRegionHost's rounded surface.  Draw only a
    // flat surface and let DrawTab render the individual rounded tabs.
    const wxColour surface = UiTheme::Palette().bgPanel;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(surface));
    dc.DrawRectangle(rect);
}

int UiTabArt::GetBorderWidth(wxWindow*)
{
    return 0;
}

int UiTabArt::GetAdditionalBorderSpace(wxWindow*)
{
    return 0;
}

void UiTabArt::DrawTab(wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& pane,
                       const wxRect& inRect, int closeButtonState, wxRect* outTabRect,
                       wxRect* outButtonRect, int* xExtent)
{
    ApplyTheme();
    const int dip = wnd != nullptr ? wnd->FromDIP(1) : 1;
    const auto dipPx = [wnd](int value) { return wnd != nullptr ? wnd->FromDIP(value) : value; };
    const int pad = std::max(dipPx(10), dip * 10);
    const wxSize measured = wxAuiGenericTabArt::GetTabSize(
        dc, wnd, pane.caption, pane.bitmap, pane.active, closeButtonState, xExtent);

    wxRect tab = inRect;
    tab.width = std::max(measured.x, dipPx(34));
    tab.height = std::max(measured.y, dipPx(36));
    if (outTabRect != nullptr) {
        *outTabRect = tab;
    }
    if (outButtonRect != nullptr) {
        *outButtonRect = wxRect();
    }
    if (xExtent != nullptr) {
        *xExtent = tab.width;
    }

    const UiTheme::Colours& colours = UiTheme::Palette();
    wxColour fill = pane.active ? colours.strongOnPanel : colours.surfaceOnPanel;
    if (pane.hover && !pane.active) {
        fill = colours.bgPanel;
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(fill));
    dc.DrawRoundedRectangle(tab.x + dip, tab.y + dip,
                            std::max(0, tab.width - dip * 2),
                            std::max(0, tab.height - dip * 2),
                            dipPx(UiTheme::Radius::SM));

    int contentX = tab.x + pad;
    if (pane.bitmap.IsOk()) {
        const int iconSize = dipPx(16);
        const wxBitmap bitmap = pane.bitmap.GetBitmap(wxSize(iconSize, iconSize));
        if (bitmap.IsOk()) {
            dc.DrawBitmap(bitmap, contentX,
                          tab.y + std::max(0, (tab.height - bitmap.GetHeight()) / 2), true);
            contentX += bitmap.GetWidth() + dipPx(6);
        }
    }

    dc.SetFont(pane.active ? m_selectedFont : m_normalFont);
    dc.SetTextForeground(pane.active ? colours.text1 : colours.text2);
    wxCoord textW = 0;
    wxCoord textH = 0;
    dc.GetTextExtent(pane.caption, &textW, &textH);
    const int textY = tab.y + std::max(0, (tab.height - static_cast<int>(textH)) / 2);
    dc.DrawText(pane.caption, contentX, textY);

    if ((closeButtonState & wxAUI_BUTTON_STATE_HIDDEN) == 0) {
        const int closeSize = dipPx(16);
        wxRect closeRect(tab.GetRight() - pad - closeSize,
                         tab.y + std::max(0, (tab.height - closeSize) / 2),
                         closeSize, closeSize);
        wxRect drawnClose;
        wxAuiGenericTabArt::DrawButton(dc, wnd, closeRect, wxAUI_BUTTON_CLOSE,
                                       closeButtonState, wxTOP, &drawnClose);
        if (outButtonRect != nullptr) {
            *outButtonRect = drawnClose;
        }
    }

    if (pane.active) {
        const int markerHeight = std::max(1, dip * 2);
        const int inset = std::max(dip * 5, 1);
        const int y = tab.GetBottom() - markerHeight - dip;
        dc.SetPen(wxPen(colours.accent, markerHeight));
        dc.DrawLine(tab.x + inset, y, tab.GetRight() - inset, y);
    }
}
