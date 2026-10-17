//
// UiMenuBar.cpp — 自绘菜单条实现（方案 B）
//

#include "ui/UiMenuBar.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>

#include <algorithm>

namespace {

// app.html 顶层项：高 24px、内边距 0 10px、圆角 6px。
constexpr int kItemH     = 24;  // DIP
constexpr int kPadX      = 10;  // DIP
constexpr int kItemRadius = 6;  // DIP
constexpr int kLeftInset = 6;   // DIP：条左端留白，避免第一项贴边

}  // namespace

UiMenuBar::UiMenuBar(wxWindow* parent, wxMenuBar* menus)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxBORDER_NONE | wxWANTS_CHARS),
      m_menus(menus)
{
    SetName("UiMenuBar");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(UiTheme::Palette().bgWindow);
    SetMinSize(wxSize(-1, FromDIP(UiTheme::Skeleton::MenuBarH)));

    Bind(wxEVT_PAINT, &UiMenuBar::Paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Rebuild(); event.Skip(); });
    Bind(wxEVT_MOTION, &UiMenuBar::OnMouseMove, this);
    Bind(wxEVT_LEAVE_WINDOW, &UiMenuBar::OnMouseLeave, this);
    Bind(wxEVT_LEFT_DOWN, &UiMenuBar::OnLeftDown, this);
    Bind(wxEVT_KEY_DOWN, &UiMenuBar::OnKeyDown, this);
    Bind(wxEVT_SET_FOCUS, &UiMenuBar::OnFocusChange, this);
    Bind(wxEVT_KILL_FOCUS, &UiMenuBar::OnFocusChange, this);

    m_theme = UiTheme::Subscribe([this] {
        SetBackgroundColour(UiTheme::Palette().bgWindow);
        Rebuild();
    });

    Rebuild();
}

wxFont UiMenuBar::ItemFont() const
{
    // app.html 12.5px/510 → 取 12 DIP Normal（token 档位无 12.5）
    return UiTheme::UiFont(const_cast<UiMenuBar*>(this), UiTheme::TextSize::P12,
                           UiTheme::Weight::Normal);
}

void UiMenuBar::Rebuild()
{
    // 弹出下拉期间，顶层菜单被临时从 wxMenuBar 摘下，此刻重排会读到少一项的标签。
    if (m_openIndex != wxNOT_FOUND) return;

    m_items.clear();
    if (m_menus == nullptr) {
        Refresh(false);
        return;
    }

    wxClientDC dc(this);
    dc.SetFont(ItemFont());

    const int barH  = FromDIP(UiTheme::Skeleton::MenuBarH);
    const int itemH = FromDIP(kItemH);
    const int y = std::max(0, (barH - itemH) / 2);
    int x = FromDIP(kLeftInset);

    const size_t count = m_menus->GetMenuCount();
    m_items.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const wxString raw = m_menus->GetMenuLabel(i);  // 形如 "&File"
        Item item;
        for (size_t c = 0; c < raw.length(); ++c)
        {
            if (raw[c] == wxT('&'))
            {
                if (c + 1 < raw.length() && raw[c + 1] != wxT('&'))
                {
                    item.mnemonic = raw[c + 1];
                }
                continue;  // '&&' 当作单个 '&'，够用且不会错位
            }
            item.label += raw[c];
        }

        const int width = dc.GetTextExtent(item.label).x + FromDIP(kPadX) * 2;
        // 命中区占满整行（否则行内上下各 6 DIP 会变成点不到的死区）；
        // 视觉胶囊仍按 app.html 的 24 DIP / 6 DIP 圆角绘制。
        item.rect = wxRect(x, 0, width, barH);
        item.pill = wxRect(x, y, width, itemH);
        x = item.rect.GetRight();
        m_items.push_back(item);
    }

    if (m_openIndex >= static_cast<int>(m_items.size())) m_openIndex = wxNOT_FOUND;
    if (m_focusIndex >= static_cast<int>(m_items.size())) m_focusIndex = wxNOT_FOUND;

    Refresh(false);
}

void UiMenuBar::Paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    // 模块栏底 = 纯白（与模块面板一致）
    const UiTheme::Colours& colours = UiTheme::Palette();
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();

    if (m_items.empty()) return;

    dc.SetFont(ItemFont());
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        const Item& item = m_items[i];
        if (static_cast<int>(i) == m_openIndex)
        {
            // 展开态：--sel 底 + --accent 字（app.html §3.2）
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(colours.selOnPanel));
            dc.DrawRoundedRectangle(item.pill, FromDIP(kItemRadius));
            dc.SetTextForeground(colours.accent);
        }
        else if (static_cast<int>(i) == m_hoverIndex)
        {
            dc.SetTextForeground(colours.text1);  // 悬停只加深文字，无底色
        }
        else
        {
            dc.SetTextForeground(colours.text2);
        }

        const wxSize extent = dc.GetTextExtent(item.label);
        dc.DrawText(item.label,
                    item.pill.x + (item.pill.width - extent.x) / 2,
                    item.pill.y + (item.pill.height - extent.y) / 2);
    }

    // 键盘聚焦：底部 2 DIP accent 线（与工具栏 DEC-14 的焦点提示同一语言）
    if (HasFocus() && m_openIndex == wxNOT_FOUND && m_focusIndex >= 0 &&
        m_focusIndex < static_cast<int>(m_items.size()))
    {
        const wxRect& r = m_items[m_focusIndex].pill;
        const int inset = FromDIP(6);
        dc.SetPen(wxPen(colours.accent, std::max(1, FromDIP(2))));
        dc.DrawLine(r.x + inset, r.GetBottom() - FromDIP(4),
                    r.GetRight() - inset, r.GetBottom() - FromDIP(4));
    }
}

int UiMenuBar::HitTest(const wxPoint& pt) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].rect.Contains(pt)) return static_cast<int>(i);
    }
    return wxNOT_FOUND;
}

void UiMenuBar::OnMouseMove(wxMouseEvent& event)
{
    if (m_openIndex != wxNOT_FOUND) { event.Skip(); return; }

    const int hit = HitTest(event.GetPosition());
    if (hit != m_hoverIndex)
    {
        m_hoverIndex = hit;
        Refresh(false);
    }
    event.Skip();
}

void UiMenuBar::OnMouseLeave(wxMouseEvent& event)
{
    if (m_hoverIndex != wxNOT_FOUND)
    {
        m_hoverIndex = wxNOT_FOUND;
        Refresh(false);
    }
    event.Skip();
}

void UiMenuBar::OnLeftDown(wxMouseEvent& event)
{
    SetFocus();
    const int hit = HitTest(event.GetPosition());
    if (hit >= 0) OpenMenu(hit);
    event.Skip();
}

void UiMenuBar::OnKeyDown(wxKeyEvent& event)
{
    const int count = static_cast<int>(m_items.size());
    switch (event.GetKeyCode())
    {
    case WXK_LEFT:
        if (count > 0)
        {
            m_focusIndex = (m_focusIndex <= 0 ? count : m_focusIndex) - 1;
            Refresh(false);
        }
        return;
    case WXK_RIGHT:
        if (count > 0)
        {
            m_focusIndex = (m_focusIndex + 1) % count;
            Refresh(false);
        }
        return;
    case WXK_DOWN:
    case WXK_RETURN:
    case WXK_SPACE:
        if (m_focusIndex >= 0) { OpenMenu(m_focusIndex); return; }
        break;
    case WXK_ESCAPE:
        m_focusIndex = wxNOT_FOUND;
        m_hoverIndex = wxNOT_FOUND;
        Refresh(false);
        return;
    default:
        break;
    }
    event.Skip();
}

void UiMenuBar::OnFocusChange(wxFocusEvent& event)
{
    Refresh(false);
    event.Skip();
}

void UiMenuBar::OpenMenu(int index)
{
    if (m_menus == nullptr || index < 0 || index >= static_cast<int>(m_items.size())) return;

    wxMenu* menu = m_menus->GetMenu(static_cast<size_t>(index));
    if (menu == nullptr) return;

    m_openIndex  = index;
    m_focusIndex = index;
    m_hoverIndex = wxNOT_FOUND;
    Refresh(false);
    Update();  // 先把"展开"态画出来，再进入下拉的模态循环

    // ⚠️ 挂在 wxMenuBar 上的菜单**不能**单独弹出：wx 3.2.9 menucmn.cpp:798
    //    assert "!IsAttached()" failed in SetInvokingWindow()
    //    （native 菜单由系统弹出，wx 从未走 PopupMenu 这条路，所以原生条时不会触发）。
    //    修法：Remove() 只把菜单从栏里摘下（Detach，不销毁对象），弹完立刻 Insert 回原位——
    //    菜单栏内容/勾选态/最近文件关联都不受影响。
    const wxString title = m_menus->GetMenuLabel(static_cast<size_t>(index));
    m_menus->Remove(static_cast<size_t>(index));

    // ⚠️ 关键修复（菜单项静默失效 / TraceBridge 点不开）：
    //    菜单一旦被从菜单栏"摘下"，wx 的 `wxMenuBase::DoProcessEvent`
    //    （src/common/menucmn.cpp:684-750）就拿不到菜单栏 —— 它先看
    //    `mb = menu->GetMenuBar()`（此时为 nullptr），再退回
    //    `win = 弹出它的窗口`（= 本自绘面板），事件只会沿窗口链冒泡到 MainFrame。
    //    而本仓**绝大多数菜单项的处理器挂在 `MainMenuBar` 上**（95 个 EVT_MENU；
    //    `MainMenuBar` 是 wxMenuBar，不是窗口、不在窗口链上）⇒ 这些项全部静默失效，
    //    只有 6 个挂在 MainFrame 上的项（面板显隐等）还活着。
    //    修法：弹出期间把该菜单的**事件处理器临时指向菜单栏**，让 DoProcessEvent 的
    //    `handler->SafelyProcessEvent(event)` 那一步先跑到 MainMenuBar 的处理器；
    //    弹完恢复原处理器。菜单结构 / id / 勾选态一律不变。
    wxEvtHandler* const previousHandler = menu->GetEventHandler();
    menu->SetEventHandler(m_menus);

    const wxRect rect = m_items[index].pill;
    PopupMenu(menu, ClientToScreen(wxPoint(rect.x, rect.GetBottom())));

    menu->SetEventHandler(previousHandler);

    m_menus->Insert(static_cast<size_t>(index), menu, title);

    m_openIndex = wxNOT_FOUND;
    m_hoverIndex = wxNOT_FOUND;
    Rebuild();  // 弹出期间 Rebuild 被跳过，这里补一次
}

void UiMenuBar::FocusFromKeyboard()
{
    if (m_items.empty()) return;
    SetFocus();
    if (m_focusIndex < 0) m_focusIndex = 0;
    Refresh(false);
}

bool UiMenuBar::ActivateMnemonic(wxChar ch)
{
    const wxChar want = wxToupper(ch);
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].mnemonic != 0 && wxToupper(m_items[i].mnemonic) == want)
        {
            const int index = static_cast<int>(i);
            SetFocus();
            m_focusIndex = index;
            // 从 CHAR_HOOK 里直接弹模态菜单会打断事件链，放到下一轮事件循环再弹。
            CallAfter([this, index] { OpenMenu(index); });
            return true;
        }
    }
    return false;
}
