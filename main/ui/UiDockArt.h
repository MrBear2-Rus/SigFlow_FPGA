#pragma once
//
// UiDockArt — AUI 停靠外观（T-130）
//
// 规范依据：docs/GUI_CHUGAO/SigFlow-界面改造SPEC.md v2.0
//   THE-031 去掉渐变、统一 caption 色/字体、sash 色、激活页强调线
//   THE-032 Art provider 所有权按 wxAUI 规则移交，不得重复 delete
//   THE-033 DPI 与主题变化时必须用**类型安全**方式获取 Art（GAP-E2 修复点）
//
// 说明：`wxAuiDefaultDockArt` 在本仓 wx 3.2.9 中是**真实类**（`wx/aui/dockart.h:90`，
//       未被 `#define` 别名化），因此本类可安全继承它。
//       （对比：`wxAuiDefaultTabArt` 在 __WXMSW__ + wxUSE_UXTHEME 下被定义为原生
//        `wxAuiMSWTabArt`，见 aui/tabart.h:323-332 —— 故 TabArt 走 wxAuiGenericTabArt，见 UiTabArt.h）
//
// 原实现位置：`main/MainFrame.h` 的 `ModernDockArt`（T-130 移出并 token 化）。
//

#include <wx/aui/aui.h>

class wxWindow;

/// AUI 停靠外观：无渐变、无色带、caption/sash/边框全部来自 UiTheme。
class UiDockArt : public wxAuiDefaultDockArt
{
public:
    UiDockArt();

    /// 按当前主题重新取色（由 MainFrame::ApplyTheme 调用，THE-030）
    void ApplyThemeColours();

    /// 按 DPI 重算需要随缩放变化的度量与字体（由 wxEVT_DPI_CHANGED 调用，PLT-009）
    void UpdateMetrics(wxWindow* parent);

    /// 自绘 pane 标题栏按钮（最大化-还原 / 关闭 / 最小化 / 图钉）。
    /// 基类绘制的是 3D 位图按钮，在扁平圆角卡片上显示为"坏掉的小方块"（RV-T03）。
    void DrawPaneButton(wxDC& dc, wxWindow* window, int button, int buttonState,
                        const wxRect& rect, wxAuiPaneInfo& pane) override;

    /// 页面底渐变（app.html §0）：AUI 的 dock 背景即"页面"，改画 160° 冷灰渐变。
    void DrawBackground(wxDC& dc, wxWindow* window, int orientation,
                        const wxRect& rect) override;

    /// 分隔条 / pane 边框也是"页面"的一部分：不铺实色，避免在全窗渐变上留下浅色隔断。
    void DrawSash(wxDC& dc, wxWindow* window, int orientation,
                  const wxRect& rect) override;
    void DrawBorder(wxDC& dc, wxWindow* window, const wxRect& rect,
                    wxAuiPaneInfo& pane) override;
};
