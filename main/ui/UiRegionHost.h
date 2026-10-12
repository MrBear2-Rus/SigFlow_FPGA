#pragma once

#include "ui/UiLayoutState.h"
#include "ui/UiTheme.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/panel.h>

// RV-T02: shared visual host for a rounded workspace region.
//
// The host owns presentation only. A content window remains the same wxWindow
// instance and keeps its existing event handlers, focus path and business
// bindings. The content is inset so native child windows cannot paint over the
// rounded corners; no SetShape/region clipping is used for the child HWND.
class UiRegionHost final : public wxPanel
{
public:
    explicit UiRegionHost(wxWindow* parent);

    void SetContent(wxWindow* content);
    wxWindow* Content() const { return m_content; }
    void SetBalance(UiTheme::Balance balance);
    void SetRadius(int radiusDip);
    void SetContentInset(int insetDip);

private:
    void Arrange();
    void Paint(wxPaintEvent& event);

    wxWindow* m_content = nullptr;
    UiTheme::Balance m_balance = UiTheme::Balance::Justify;
    int m_radiusDip = UiTheme::Radius::LG;
    int m_contentInsetDip = UiTheme::RegionMetrics::ContentInset;
    UiTheme::Subscription m_themeSubscription;
};
