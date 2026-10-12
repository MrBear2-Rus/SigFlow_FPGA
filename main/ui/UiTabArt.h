#pragma once
//
// UiTabArt — AUI 笔记本页签外观（T-130）
//
// 规范依据：docs/GUI_CHUGAO/SigFlow-界面改造SPEC.md v2.0
//   THE-031 统一激活页强调线与字号、无渐变
//   CMP-012/DES-012 页签字号 12 DIP、激活指示条 2 DIP `accent`（指示条的实际绘制在
//   笔记本自绘路径，本类负责 base/active 色与字体，尽量靠拢设计稿）
//
// 为什么继承 `wxAuiGenericTabArt` 而不是 `wxAuiDefaultTabArt`：
//   本仓 wx 3.2.9 的 `wx/aui/tabart.h:323-332` 在 `__WXMSW__ && wxUSE_UXTHEME` 下
//   把 `wxAuiDefaultTabArt` **#define 成原生 `wxAuiMSWTabArt`**（tabartmsw.h）。
//   直接继承 `wxAuiDefaultTabArt` 会随平台得到不同基类，且原生实现不响应 SetColour 等
//   通用接口。改用始终存在的通用实现，才能得到跨平台一致、可 token 化的页签外观。
//
// 所有权：`wxAuiNotebook::SetArtProvider` 后由笔记本接管释放（THE-032），本类不自持生命周期。
//

#include <wx/aui/aui.h>

class UiTabArt : public wxAuiGenericTabArt
{
public:
    UiTabArt();

    /// 按当前主题重新设置页签底色/激活色与字体（由 MainFrame::ApplyTheme 调用）
    void ApplyTheme();

    // ---------------------------------------------------------------------
    // T-719 / V9 修复（THE-030 的绘制级刷新缺口）
    //
    // 机制（wx 源码级）：`wxAuiTabContainer::SetArtProvider()` 会调用 `art->Clone()`，
    // 而 `wxAuiGenericTabArt::Clone()` 是 `new wxAuiGenericTabArt(*this)` —— 于是
    // **真正画页签的是另一个（基类）实例**，`MainFrame::ApplyTheme()` 只更新笔记本级
    // provider，克隆体此后再无任何路径取新 token（T-719 夹具实测：刷新调用 = 0）。
    // 后果：一旦开启深色档，页签条会停在浅色（"深壳白页签"）。
    //
    // 修法：① 覆写 `Clone()`，让绘制级实例也是本类；② 在绘制入口自刷新，
    // 使任何实例（含既有克隆体）在每帧都按当前调色板取色，不依赖外部调用 ApplyTheme。
    // ---------------------------------------------------------------------
    wxAuiTabArt* Clone() override;

    // The rounded UiRegionHost owns the outer region edge.  The generic AUI
    // implementation paints a second square notebook frame, which reads as
    // a rectangular box nested inside the rounded surface.
    void DrawBorder(wxDC& dc, wxWindow* wnd, const wxRect& rect) override;
    void DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override;
    void DrawTab(wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& pane, const wxRect& inRect,
                 int closeButtonState, wxRect* outTabRect, wxRect* outButtonRect,
                 int* xExtent) override;
    int GetBorderWidth(wxWindow* wnd) override;
    int GetAdditionalBorderSpace(wxWindow* wnd) override;
};
