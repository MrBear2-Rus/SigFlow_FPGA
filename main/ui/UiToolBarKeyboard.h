#pragma once
//
// UiToolBarKeyboard — AUI 工具栏/活动栏逐项键盘焦点（T-620 / A11Y-001、A11Y-002）
//
// 背景（已核实 wx 3.2.9 源码）：
//   * wxAuiToolBar 是 wxControl，`wxControl::MSWGetStyle()` 在
//     `AcceptsFocusFromKeyboard()` 为真时加上 WS_TABSTOP（src/msw/control.cpp:204-214），
//     所以顶部工具栏与活动栏**本来就在 Tab 链上**；
//   * 但它的每一项都是 UiToolBarArt 自绘的，聚焦时没有任何可见提示，也没有键盘激活
//     —— 正是 A11Y-001 / A11Y-002 要修的「能聚焦但看不见、不能用键盘操作」。
//
// 为什么按键在 MainFrame 的 CHAR_HOOK 里处理、而不是绑 wxEVT_KEY_DOWN：
//   MSW 的消息预处理（src/msw/window.cpp:2505-2650）会把方向键/Tab 交给
//   IsDialogMessage 式的对话框导航；普通 AUI tool item 没有 DLGC_WANTARROWS，
//   因此 KEY_DOWN 根本收不到方向键。CHAR_HOOK 走全局键盘钩子，在预处理之前到达
//   顶层窗口（src/msw/window.cpp:7211-7231），是本轮唯一能在不新增原生子类的前提下
//   拿到方向键的地方。Space/Enter 只在**工具栏自身**持有焦点时消费，
//   子控件（Synthesize、布局三键等）的 Space/Enter 语义不受影响（KEY-009/A11Y-002）。
//
// 不吞键：Tab / Ctrl+Tab / Shift+Tab / Esc 一律不消费（KEY-007 不得出现 Tab 陷阱、
//         INT-019 Esc 交给浮层与系统菜单）。
//
// 激活路径与鼠标完全一致：复制 wxAuiToolBar::OnLeftUp()（src/aui/auibar.cpp:2656-2717）——
//   check/radio 先 ToggleTool 再发 wxEVT_MENU（wxEVT_TOOL 就是 wxEVT_MENU，event.h:883）。
//
// 纯 header：CMakeLists.txt 是显式源列表（第 255 行起），本轮不得新增 .cpp 或改 CMakeLists。
//

#include <wx/aui/aui.h>
#include <wx/control.h>
#include <wx/dc.h>
#include <wx/event.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "ui/UiFocusTrace.h"
#include "ui/UiToolBarArt.h"

class UiToolBarKeyboard
{
public:
    /// T-620：工具栏自身也是 Tab 链上的一站（它提供逐项键盘焦点）。
    /// MainFrame 的 Tab 链收集器用它区分「带子控件的工具栏」与普通容器。
    static bool IsKeyboardToolBar(const wxWindow* window)
    {
        const std::vector<const wxWindow*>& registry = Registry();
        return std::find(registry.begin(), registry.end(), window) != registry.end();
    }

    UiToolBarKeyboard(wxAuiToolBar* bar, UiToolBarArt* art)
        : m_state(std::make_shared<State>())
    {
        m_state->bar = bar;
        m_state->art = art;
        Registry().push_back(bar);
        bar->Bind(wxEVT_DESTROY, [bar](wxWindowDestroyEvent& event) {
            std::vector<const wxWindow*>& registry = Registry();
            registry.erase(std::remove(registry.begin(), registry.end(), bar), registry.end());
            event.Skip();
        });

        // 生命周期：状态由 shared_ptr 持有，绑定到工具栏上的闭包各自持有一份，
        // 因此即使宿主对象先析构（MainFrame 成员先于 wxFrame 基类销毁），
        // 工具栏随后触发的焦点事件也不会踩空指针。
        const std::shared_ptr<State> state = m_state;
        bar->Bind(wxEVT_SET_FOCUS, [state](wxFocusEvent& event) {
            state->keyboardFocus = UiFocusTrace::FromKeyboard();
            // 修复：Move/Publish 的形参是 State&，此处 state 是 const shared_ptr<State>，需解引用
            if (state->index < 0) Move(*state, 1);
            Publish(*state);
            event.Skip();
        });
        bar->Bind(wxEVT_KILL_FOCUS, [state](wxFocusEvent& event) {
            state->keyboardFocus = false;
            if (state->art) state->art->SetKeyboardFocusTool(-1);
            if (state->bar) state->bar->Refresh(false);
            event.Skip();
        });
    }

    /// 由 MainFrame 的 CHAR_HOOK 调用；返回 true 表示按键已被本工具栏消费。
    bool HandleKey(wxKeyEvent& event)
    {
        State& state = *m_state;
        if (state.bar == nullptr || state.art == nullptr) return false;
        if (wxWindow::FindFocus() != state.bar) return false;  // 焦点在子控件或别处

        const int key = event.GetKeyCode();

        // 方向键 = 在可聚焦的 tool item 之间移动（A11Y-002）。
        const bool vertical = (state.bar->GetWindowStyleFlag() & wxAUI_TB_VERTICAL) != 0;
        int step = 0;
        if (vertical && key == WXK_UP) step = -1;
        else if (vertical && key == WXK_DOWN) step = 1;
        else if (!vertical && key == WXK_LEFT) step = -1;
        else if (!vertical && key == WXK_RIGHT) step = 1;
        else if (key == WXK_HOME) step = kHome;
        else if (key == WXK_END) step = kEnd;

        if (step != 0) {
            Move(state, step);
            Publish(state);
            return true;
        }

        // Space / Enter = 激活当前项，等价于鼠标单击（KEY-011）。
        if (key == WXK_SPACE || key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            if (!event.IsAutoRepeat()) Activate(state);
            return true;
        }

        // Tab / Shift+Tab / Ctrl+Tab / Esc / 其余按键：不消费，交回正常派发。
        return false;
    }

private:
    static constexpr int kHome = -1000;
    static constexpr int kEnd = 1000;

    /// 已挂载逐项键盘焦点的工具栏（function-local static：header-only 且不引入 ODR 问题）。
    static std::vector<const wxWindow*>& Registry()
    {
        static std::vector<const wxWindow*> registry;
        return registry;
    }

    struct State
    {
        wxAuiToolBar* bar = nullptr;
        UiToolBarArt* art = nullptr;
        int index = -1;            ///< 工具栏项下标（FindToolByIndex 的入参）
        bool keyboardFocus = false;
    };

    /// 只有普通/chip 类工具参与键盘导航。分隔条、spacer、label 与 AddControl 的
    /// 真实控件（Synthesize / 布局三键）不在此列：真实控件本身就有自己的 Tab 链。
    static bool Navigable(wxAuiToolBarItem& item)
    {
        const int kind = item.GetKind();
        if (kind != wxITEM_NORMAL && kind != wxITEM_CHECK && kind != wxITEM_RADIO) return false;
        if (item.GetWindow() != nullptr) return false;
        if (!item.IsActive()) return false;
        return (item.GetState() & wxAUI_BUTTON_STATE_DISABLED) == 0;
    }

    static std::vector<int> NavigableIndices(const State& state)
    {
        std::vector<int> indices;
        if (state.bar == nullptr) return indices;
        const int count = static_cast<int>(state.bar->GetToolCount());
        for (int i = 0; i < count; ++i) {
            wxAuiToolBarItem* item = state.bar->FindToolByIndex(i);
            if (item == nullptr || !Navigable(*item)) continue;
            // T-520 给 topBar 加了 wxAUI_TB_OVERFLOW 后，放不下的工具项会被停放进 chevron
            // 且不再绘制（auibar.cpp:2472 的 last_extent 提前 break）。键盘焦点只停在
            // **当前可见**的项上，否则会出现「焦点在某一项上但看不到焦点环」。
            if (!state.bar->GetToolFitsByIndex(i)) continue;
            indices.push_back(i);
        }
        return indices;
    }

    /// step = ±1 逐项移动；kHome / kEnd 跳到首/末项。
    static void Move(State& state, int step)
    {
        const std::vector<int> indices = NavigableIndices(state);
        if (indices.empty()) { state.index = -1; return; }

        if (step == kHome) { state.index = indices.front(); return; }
        if (step == kEnd) { state.index = indices.back(); return; }

        const auto current = std::find(indices.begin(), indices.end(), state.index);
        if (current == indices.end()) {
            state.index = step < 0 ? indices.back() : indices.front();
            return;
        }
        const int position = static_cast<int>(current - indices.begin());
        const int next = (position + (step < 0 ? -1 : 1) + static_cast<int>(indices.size())) %
                         static_cast<int>(indices.size());
        state.index = indices[static_cast<size_t>(next)];
    }

    static void Publish(State& state)
    {
        if (state.art == nullptr || state.bar == nullptr) return;
        wxAuiToolBarItem* item = state.bar->FindToolByIndex(state.index);
        state.art->SetKeyboardFocusTool(item != nullptr && Navigable(*item) ? item->GetId() : -1);
        state.bar->Refresh(false);
    }

    static void Activate(State& state)
    {
        if (state.bar == nullptr) return;
        wxAuiToolBarItem* item = state.bar->FindToolByIndex(state.index);
        if (item == nullptr || !Navigable(*item)) return;

        const int id = item->GetId();
        wxCommandEvent event(wxEVT_MENU, id);
        event.SetEventObject(state.bar);
        if (item->CanBeToggled()) {
            const bool toggle = !state.bar->GetToolToggled(id);
            state.bar->ToggleTool(id, toggle);
            event.SetInt(toggle);
            state.bar->Refresh(false);
        }
        state.bar->GetEventHandler()->ProcessEvent(event);
        Publish(state);
    }

    std::shared_ptr<State> m_state;
};
