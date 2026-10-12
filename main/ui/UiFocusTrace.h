#pragma once
//
// UiFocusTrace — 「这次焦点是键盘来的，还是鼠标来的」记录器（T-620）
//
// 规范依据：docs/GUI-CHUGAO/SigFlow-界面改造SPEC.md
//   A11Y-001 键盘聚焦时必须有可见焦点环（accent 3–4 DIP / 45% 透明）
//   DEC-14   鼠标点击后的**焦点保留**不得维持蓝边，改用轻量灰色提示
//
// 两条要求同时成立，因此自绘控件必须能区分焦点来源：
//   * 键盘导航（Tab / 方向键 / Esc 之后的程序化聚焦）→ 画 accent 焦点环；
//   * 鼠标按下拿到的焦点（点击后仍持有焦点）      → 只画灰色提示。
//
// 实现方式：任意一处输入处理调用 NoteKeyboard()/NotePointer()，控件在
// wxEVT_SET_FOCUS 时读一次 FromKeyboard() 并把结果**闩存**下来（见
// UiFocusRing::Tracker）。闩存很关键：之后按键只改变控件自身的 press 态，
// 不会把鼠标焦点追溯性变成键盘焦点（T510B 夹具的
// 「idle focused/selected has no blue edge」正是靠这一点保持通过）。
//
// 纯 header：CMakeLists 用的是显式源列表（CMakeLists.txt:255），
// 本轮不得新增 .cpp，也不得改 CMakeLists。
//

namespace UiFocusTrace
{
/// 最近一次已观察到的用户输入是否来自键盘。默认 false（＝未知时按鼠标处理，
/// 这样「点击后保留焦点」不会凭空出现蓝边）。
inline bool& LastWasKeyboard()
{
    static bool value = false;
    return value;
}

/// 键盘输入（KEY_DOWN / KEY_UP / CHAR_HOOK / 浮层内部的键导航）
inline void NoteKeyboard() { LastWasKeyboard() = true; }

/// 指针输入（LEFT_DOWN / LEFT_UP / ENTER / LEAVE）
inline void NotePointer() { LastWasKeyboard() = false; }

/// 供控件在聚焦瞬间读取
inline bool FromKeyboard() { return LastWasKeyboard(); }
}  // namespace UiFocusTrace
