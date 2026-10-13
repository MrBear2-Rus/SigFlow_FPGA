#pragma once
//
// UiToolBarArt — 工具栏 / 活动栏 chip 外观（T-230 / T-240）
//
// 规范依据：docs/GUI_CHUGAO/SigFlow-界面改造SPEC.md v2.0
//   CMP-020…CMP-029 chip 规格与五变体（默认 / hover / primary(液态玻璃) / warn / steady）
//   CMP-028 活动栏选中指示条 3×18 DIP、圆角 2、accent
//   DES-013/DES-014 图标线宽与尺寸档
//   THE-031/DES-004 材质三条：结构面 + 按键平面化
//
// 为什么继承 `wxAuiGenericToolBarArt` 而不是 `wxAuiDefaultToolBarArt`：
//   本仓 wx 3.2.9 的 `wx/aui/auibar.h:760-768` 在 `__WXMSW__ && wxUSE_UXTHEME` 下把
//   `wxAuiDefaultToolBarArt` **#define 成原生 `wxAuiMSWToolBarArt`**（barartmsw.h），
//   原生实现不响应通用 setter，且基类随平台变化。改用始终存在的通用实现，
//   才能得到跨平台一致、可 token 化的 chip 外观。
//
// 内容绘制策略：**不调用基类 DrawButton**，自行绘制图标与文本。
//   原因：基类会在 hover/pressed/checked 时自己铺一层底，覆盖本类绘制的 chip 底与描边。
//
// 所有权：`wxAuiToolBar::SetArtProvider` 后由工具栏接管释放，本类不自持生命周期（THE-032）。
//

#include <wx/aui/aui.h>

class wxControl;

class UiToolBarArt : public wxAuiGenericToolBarArt
{
public:
    UiToolBarArt();

    /// 主题变更时重新取色（由 MainFrame::ApplyTheme 调用）
    void ApplyTheme();

    void DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override;
    void DrawPlainBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override;
    void DrawButton(wxDC& dc, wxWindow* wnd, const wxAuiToolBarItem& item,
                    const wxRect& rect) override;

    /// chip / 活动栏按键的最小尺寸（CMP-021 chip 高 30 DIP；LAY-003 活动栏键 34×34）
    wxSize GetToolSize(wxDC& dc, wxWindow* wnd, const wxAuiToolBarItem& item) override;

    /// T-620 / A11Y-001：键盘聚焦的 tool item（tool id）。-1 = 无。
    /// UiToolBarKeyboard 选中的项以 text3 灰线显示焦点（DEC-14）；
    /// 悬停 / 按压才显示蓝边，避免恢复常驻蓝环。
    void SetKeyboardFocusTool(int toolId) { m_keyboardFocusTool = toolId; }
    int  KeyboardFocusTool() const { return m_keyboardFocusTool; }

    // AddControl 使用真正可聚焦的控件；普通 AUI tool item 不能提供逐项键盘焦点。
    // 激活发 wxEVT_TOOL，沿用 MainFrame 的综合路由。返回值由父工具栏持有。
    static wxControl* CreateSynthesizeButton(wxWindow* parent, wxWindowID id);

private:
    /// 供 DrawButton 复用的圆角冒胶囊底
    void DrawChipBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect, bool disabled,
                            bool hover, bool pressed, bool checked) const;

    int m_keyboardFocusTool = -1;  ///< T-620：UiToolBarKeyboard 维护的键盘焦点项
};
