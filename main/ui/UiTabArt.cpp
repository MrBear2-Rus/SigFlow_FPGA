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

    // 页签行底 = 纯白（图2：去掉灰色页签块，仅用下端绿色指示线标识"打开"的页）
    SetColour(wxColour(255, 255, 255));
    SetActiveColour(wxColour(255, 255, 255));

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

    // 空画布（notebook 无页面）：未打开任何文件时也要是一块"白底 + 20/100 DIP 网格"的画布，
    // 而不是笔记本的裸窗口底色（实测为 Windows BTNFACE #DCDCDC 死灰）。
    if (auto* book = dynamic_cast<wxAuiNotebook*>(wnd); book != nullptr && book->GetPageCount() == 0) {
        UiTheme::PaintCanvasBackdrop(dc, rect, wnd);
        return;
    }

    // 页签行底 = 纯白（图2）：不再铺灰，指示线由 DrawTab 画在下端。
    const wxColour surface = wxColour(255, 255, 255);
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
    const int pad = std::max(dipPx(12), dip * 12);       // 左右内距
    const int closeGap = std::max(dipPx(10), dip * 10);  // 标题与 × 之间的留白（避免竖向重叠）
    const int closeSize = dipPx(16);
    const bool hasClose = (closeButtonState & wxAUI_BUTTON_STATE_HIDDEN) == 0;

    const wxSize measured = wxAuiGenericTabArt::GetTabSize(
        dc, wnd, pane.caption, pane.bitmap, pane.active, closeButtonState, xExtent);

    // 先量"图标 + 标题"，算出一个能同时放下标题与 ×（中间留 closeGap）的宽度，
    // 保证 × 永远在标题右侧而不压在标题上。
    dc.SetFont(pane.active ? m_selectedFont : m_normalFont);
    wxCoord textW = 0;
    wxCoord textH = 0;
    dc.GetTextExtent(pane.caption, &textW, &textH);
    int contentW = static_cast<int>(textW);
    if (pane.bitmap.IsOk()) {
        contentW += dipPx(16) + dipPx(6);
    }
    const int needed = pad * 2 + contentW + (hasClose ? closeGap + closeSize : 0);

    wxRect tab = inRect;
    tab.width = std::max(std::max(measured.x, needed), dipPx(34));
    tab.height = std::max(measured.y, dipPx(40));
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

    // 图2：页签底**一律纯白**（绝不填充黑/灰），选中只由下端绿线表示；
    // 悬停给一层极浅的灰反馈。这样即使底层窗口未绘制，页签也不会是黑的。
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    dc.DrawRoundedRectangle(tab.x + dip, tab.y + dip,
                            std::max(0, tab.width - dip * 2),
                            std::max(0, tab.height - dip * 2),
                            dipPx(UiTheme::Radius::SM));
    // 悬停：必须用**实心浅灰**。原来用 wxColour(0,0,0,8)（带 alpha 的黑），
    // 普通 wxDC 在 MSW 下会忽略 alpha → 直接画成纯黑块（用户实测：悬停页签变黑）。
    if (pane.hover && !pane.active) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(colours.sunkenOnPanel));
        dc.DrawRoundedRectangle(tab.x + dip, tab.y + dip,
                                std::max(0, tab.width - dip * 2),
                                std::max(0, tab.height - dip * 2),
                                dipPx(UiTheme::Radius::SM));
    }

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
    const int textY = tab.y + std::max(0, (tab.height - static_cast<int>(textH)) / 2 - dip * 3);
    dc.DrawText(pane.caption, contentX, textY);

    if (hasClose) {
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

    // 选中：下端一条绿色指示线（图2）。用实际分配到的矩形底边定位，
    // 避免画在"我自己撑大的 tab 高度"底边而被页签行裁掉。
    if (pane.active) {
        const int markerHeight = std::max(dip * 2, 1);   // 更细
        const int inset = dip * 4;
        const int bottom = std::min(tab.GetBottom(), inRect.GetBottom());
        const int y = bottom - markerHeight;             // 贴到最底，不压字
        dc.SetPen(wxPen(colours.brand, markerHeight));
        dc.DrawLine(tab.x + inset, y, tab.GetRight() - inset, y);
    }
}
