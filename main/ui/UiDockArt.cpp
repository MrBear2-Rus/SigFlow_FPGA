//
// UiDockArt.cpp — AUI 停靠外观实现（T-130）
//

#include "ui/UiDockArt.h"

#include "ui/UiTheme.h"

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

    SetColour(wxAUI_DOCKART_BACKGROUND_COLOUR, c.bgPanel);
    SetColour(wxAUI_DOCKART_SASH_COLOUR, c.strokeOnPanel);
    SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, c.surfaceOnWindow);
    SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_COLOUR, c.surfaceOnWindow);
    SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, c.text3);
    SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR, c.text2);
}

void UiDockArt::UpdateMetrics(wxWindow* parent)
{
    if (parent == nullptr) {
        return;
    }
    // sash 用细线（THE-031 / 原实现 FromDIP(2)）
    SetMetric(wxAUI_DOCKART_SASH_SIZE, parent->FromDIP(2));

    // caption 字体走 token（THE-016/THE-017：12 DIP、medium）
    SetFont(wxAUI_DOCKART_CAPTION_FONT,
            UiTheme::UiFont(parent, UiTheme::TextSize::P12, UiTheme::Weight::Medium));
}
