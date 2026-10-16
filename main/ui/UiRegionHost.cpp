#include "ui/UiRegionHost.h"

#include <algorithm>
#include <memory>

namespace {

// RV-T02: the card is inset inside the host so the page colour shows as a
// margin. With a white page and a white card that margin is invisible; the card
// reads from its 1px outline plus the soft shadow drawn underneath. app.html
// uses the same mechanism (1px border + --elev-raised shadow).
constexpr int kCardMarginDip = 2;

}  // namespace

UiRegionHost::UiRegionHost(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(UiTheme::Palette().bgWindow);
    Bind(wxEVT_PAINT, &UiRegionHost::Paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Arrange();
        event.Skip();
    });
    m_themeSubscription = UiTheme::Subscribe([this] {
        SetBackgroundColour(UiTheme::Palette().bgWindow);
        if (m_content != nullptr) {
            m_content->SetBackgroundColour(UiTheme::Palette().strongOnPanel);
        }
        Refresh(false);
    });
}

void UiRegionHost::SetContent(wxWindow* content)
{
    m_content = content;
    if (m_content != nullptr) {
        m_content->SetBackgroundColour(UiTheme::Palette().strongOnPanel);
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

    const int margin = FromDIP(kCardMarginDip);
    const int inset = FromDIP(m_contentInsetDip);
    const int border = margin + inset;

    const wxSize hostSize = GetClientSize();
    const wxSize available(std::max(0, hostSize.x - border * 2),
                           std::max(0, hostSize.y - border * 2));
    const wxRect aligned = UiTheme::AlignedPanelRect(available, m_balance, FromDIP(100) / 100.0);
    m_content->SetSize(wxRect(aligned.x + border, aligned.y + border,
                              aligned.width, aligned.height));
}

void UiRegionHost::Paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    // 兜底：先用页面基色整块铺底，再画渐变。任何"渐变裁剪错位/未绘制"的边角
    // 都只会露出浅色基色，而不会露出未初始化的纯黑（用户实测：左下角黑块）。
    dc.SetBackground(wxBrush(UiTheme::Palette().bgWindow));
    dc.Clear();

    // ① 页面底：整窗一块冷灰渐变（锚在顶层窗口客户区），与 AUI 缝隙/分隔条连续。
    const wxWindow* top = wxGetTopLevelParent(this);
    const wxSize client = top != nullptr ? top->GetClientSize() : GetClientSize();
    const wxPoint origin = top != nullptr
        ? GetScreenPosition() - top->GetScreenPosition()
        : wxPoint(0, 0);
    UiTheme::PaintPageGradient(dc, client, origin, GetClientRect());

    // ② 模块面板本体：**纯白**圆角卡片（模块栏统一纯白；只有 Terminal 保留黑底）。
    std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));
    if (!graphics) {
        return;
    }

    const double scale = FromDIP(100) / 100.0;
    const wxRect bounds = GetClientRect();
    const double margin = FromDIP(kCardMarginDip);
    const double width = std::max(0.0, bounds.width - margin * 2.0);
    const double height = std::max(0.0, bounds.height - margin * 2.0);
    const double radius = std::min<double>(FromDIP(m_radiusDip),
                                           std::min(width, height) / 2.0);
    const double x = margin + scale / 2.0;
    const double y = margin + scale / 2.0;

    graphics->SetPen(*wxTRANSPARENT_PEN);
    graphics->SetBrush(graphics->CreateBrush(wxBrush(wxColour(255, 255, 255))));
    graphics->DrawRoundedRectangle(x, y, width, height, radius);
}
