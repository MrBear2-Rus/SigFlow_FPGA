#include "ui/UiRegionHost.h"

#include <algorithm>
#include <memory>

UiRegionHost::UiRegionHost(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(UiTheme::Palette().bgPanel);
    Bind(wxEVT_PAINT, &UiRegionHost::Paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Arrange();
        event.Skip();
    });
    m_themeSubscription = UiTheme::Subscribe([this] {
        SetBackgroundColour(UiTheme::Palette().bgPanel);
        if (m_content != nullptr) {
            m_content->SetBackgroundColour(UiTheme::Palette().surfaceOnPanel);
        }
        Refresh(false);
    });
}

void UiRegionHost::SetContent(wxWindow* content)
{
    m_content = content;
    if (m_content != nullptr) {
        m_content->SetBackgroundColour(UiTheme::Palette().surfaceOnPanel);
    }
    Arrange();
}

void UiRegionHost::SetBalance(UiTheme::Balance balance)
{
    m_balance = balance;
    Arrange();
}

void UiRegionHost::SetRadius(int radiusDip)
{
    m_radiusDip = std::max(0, radiusDip);
    Refresh(false);
}

void UiRegionHost::SetContentInset(int insetDip)
{
    m_contentInsetDip = std::max(0, insetDip);
    Arrange();
}

void UiRegionHost::Arrange()
{
    if (m_content == nullptr) {
        return;
    }

    const int inset = FromDIP(m_contentInsetDip);
    const wxSize hostSize = GetClientSize();
    const wxSize available(std::max(0, hostSize.x - inset * 2),
                           std::max(0, hostSize.y - inset * 2));
    const wxRect aligned = UiTheme::AlignedPanelRect(available, m_balance, FromDIP(100) / 100.0);
    m_content->SetSize(wxRect(aligned.x + inset, aligned.y + inset, aligned.width, aligned.height));
}

void UiRegionHost::Paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const auto& colours = UiTheme::Palette();
    dc.SetBackground(wxBrush(colours.bgPanel));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));
    if (!graphics) {
        return;
    }

    const double scale = FromDIP(100) / 100.0;
    const wxRect bounds = GetClientRect();
    const double radius = std::min<double>(FromDIP(m_radiusDip),
                                           std::min(bounds.width, bounds.height) / 2.0);

    graphics->SetBrush(graphics->CreateBrush(wxBrush(colours.surfaceOnPanel)));
    graphics->SetPen(graphics->CreatePen(wxGraphicsPenInfo(colours.strokeOnPanel)
                                             .Width(std::max(1, FromDIP(1)))));
    graphics->DrawRoundedRectangle(bounds.x + scale / 2.0,
                                   bounds.y + scale / 2.0,
                                   std::max(0.0, bounds.width - scale),
                                   std::max(0.0, bounds.height - scale),
                                   radius);
}
