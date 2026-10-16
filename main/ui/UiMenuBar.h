#pragma once
//
// UiMenuBar — 自绘菜单条（方案 B：自绘条 + 原生下拉）
//
// 为什么自绘：MSW 的原生 `wxMenuBar` 没有窗口句柄，属系统非客户区，wx 在 3.2.9 上
// 既没有 `wxMenuBar::SetFont` 实现，也没有任何 per-item 绘制钩子（`SetOwnerDrawn`
// 是 wxMenuItem 的内部实现，只为位图项服务）。因此 32 DIP 高 / 内边距 / 圆角 /
// 展开态底色这些规格在原生条上**一项都做不到**。
//
// 本类的定位：只负责「File Edit …」这一行的自绘与交互，下拉仍然用**现有的 wxMenu
// 对象**走原生 `PopupMenu` —— 于是 142 个菜单项、事件 id、勾选/单选态、动态重建
// （Window / 最近文件 / Help 后端）全部零改动。
//
// 规格（app.html）：
//   · 条高 32 DIP（UiTheme::Skeleton::MenuBarH）
//   · 顶层项 12.5 DIP、--text-2、24 DIP 高、10 DIP 水平内边距、6 DIP 圆角
//   · 平时无底色；**展开时** --sel 底 + --accent 字；悬停只加深文字（--text-1）
//
// 红线：菜单结构与快捷键语义不变。原生条被摘除后，加速键由 MainFrame 安装的
//       `wxAcceleratorTable` 承担（见 MainFrame::InstallMenuAccelerators）。
//

#include <wx/menu.h>
#include <wx/panel.h>

#include <vector>

#include "ui/UiTheme.h"

class UiMenuBar : public wxPanel
{
public:
    UiMenuBar(wxWindow* parent, wxMenuBar* menus);

    /// 顶层项文案/启用态可能变化（如 Window 菜单重建），重新读取标签并重排。
    void SyncFromMenus() { Rebuild(); }

    /// Alt / F10：把键盘焦点交给菜单条。
    void FocusFromKeyboard();

    /// Alt+<ch>：命中助记符则展开对应菜单。返回是否命中。
    bool ActivateMnemonic(wxChar ch);

    bool IsAnyMenuOpen() const { return m_openIndex != wxNOT_FOUND; }

private:
    struct Item
    {
        wxString label;        ///< 已去掉 '&' 的显示文案
        wxChar   mnemonic = 0; ///< 助记符（大写比较）
        wxRect   rect;         ///< 命中区：整行高（点击更宽容）
        wxRect   pill;         ///< 视觉胶囊：24 DIP 高、圆角 6 DIP
    };

    void Rebuild();
    void Paint(wxPaintEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnMouseLeave(wxMouseEvent& event);
    void OnLeftDown(wxMouseEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void OnFocusChange(wxFocusEvent& event);
    int  HitTest(const wxPoint& pt) const;
    void OpenMenu(int index);
    wxFont ItemFont() const;

    wxMenuBar* m_menus = nullptr;
    std::vector<Item> m_items;
    int m_hoverIndex = wxNOT_FOUND;
    int m_openIndex  = wxNOT_FOUND;  ///< 当前展开的顶层项（-1 = 无）
    int m_focusIndex = wxNOT_FOUND;  ///< 键盘聚焦的顶层项（-1 = 无）
    UiTheme::Subscription m_theme;
};
