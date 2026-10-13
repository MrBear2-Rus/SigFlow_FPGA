#pragma once
//
// UiFocusRing — 自绘控件的键盘焦点环（T-620 / A11Y-001、A11Y-002）
//
// 规范依据：
//   A11Y-001 键盘聚焦时必须可见：accent 3–4 DIP、约 45% 透明；不得只靠系统虚线框
//   A11Y-002 自绘控件必须参与焦点链，点击后焦点不得消失
//   THE-026 / SAFE-012 颜色只能取 main/ui/UiTheme.h 的 token，不得新增字面量
//
// 取色：`UiTheme::Palette().focusRingOnPanel` —— 该 token 的定义就是
//       「accent 45% 合成于面板底」（UiTheme.h:188 / UiTheme.cpp:106），
//       语义与本条完全对应，因此不再另造颜色。
//
// DEC-14 的按钮例外：Synthesize、布局按钮和普通工具栏 chip 的键盘焦点
// 使用 text3 灰色下划线，蓝边只用于 hover / 按压；这里的蓝环供其它控件使用。
// 2026-10-11 把 DEC-14 限定为鼠标焦点的实施者解释已撤回（SPEC DEC-15）。
//
// 纯 header：CMakeLists.txt 使用显式源列表（第 255 行起），本轮不得新增 .cpp。
//

#include <wx/dc.h>
#include <wx/gdicmn.h>
#include <wx/graphics.h>
#include <wx/pen.h>

#include <algorithm>

#include "ui/UiFocusTrace.h"
#include "ui/UiTheme.h"

namespace UiFocusRing
{
/// A11Y-001 规定 3–4 DIP；取 3 DIP，与 DEC-14 的瞬时 1.5 DIP 蓝边明显区分。
inline constexpr double WidthDip = 3.0;
/// 焦点环距控件外沿的内缩（与既有 focusRingOnPanel 用法一致，避免被裁切）。
inline constexpr double InsetDip = 2.0;

/// 焦点来源闩存：只在收到 wxEVT_SET_FOCUS 时采样一次 UiFocusTrace。
/// 之后按键不再改变它，保证「鼠标点击→保留焦点」不会升级成键盘焦点环。
class Tracker
{
public:
    void OnFocusGained() { m_keyboardFocus = UiFocusTrace::FromKeyboard(); }
    void OnFocusLost() { m_keyboardFocus = false; }
    bool KeyboardFocused() const { return m_keyboardFocus; }

private:
    bool m_keyboardFocus = false;
};

/// wxGraphicsContext 版本（CanvasControls / UiLayoutChoice 走这条路径）
inline void Draw(wxGraphicsContext& gc, const wxRect& r, double dip, double radius)
{
    const double inset = InsetDip * dip;
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(
        wxGraphicsPenInfo(UiTheme::Palette().focusRingOnPanel).Width(WidthDip * dip)));
    gc.DrawRoundedRectangle(r.x + inset, r.y + inset,
                            std::max(0.0, r.width - 2 * inset),
                            std::max(0.0, r.height - 2 * inset), radius);
}

/// wxDC 版本（UiToolBarArt::DrawButton 走这条路径，该方法只拿到 wxDC）
inline void Draw(wxDC& dc, const wxRect& r, int width, int radius, int inset)
{
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(UiTheme::Palette().focusRingOnPanel, width));
    dc.DrawRoundedRectangle(r.x + inset, r.y + inset,
                            std::max(0, r.width - 2 * inset),
                            std::max(0, r.height - 2 * inset), radius);
}
}  // namespace UiFocusRing
