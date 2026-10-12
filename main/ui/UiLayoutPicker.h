#pragma once

#include "ui/UiFocusRing.h"
#include "ui/UiLayoutState.h"
#include "ui/UiTheme.h"
#include <wx/control.h>
#include <wx/panel.h>
#include <wx/popupwin.h>
#include <wx/scrolwin.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <functional>
#include <vector>

// Presentation-only actions. The owner routes every surface through the same
// layout methods, with no project/model/EDA callbacks.
enum class LayoutAction { Preset, Visibility, Side, Balance, Position };

class UiLayoutChoice final : public wxControl {
public:
    UiLayoutChoice(wxWindow* parent, const wxString& label, std::function<void()> action,
                   const wxString& shortcut = {}, bool chip = false);
    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return AcceptsFocus(); }
    void SetSelected(bool value);
    bool IsSelected() const { return m_selected; }
    void Activate();
    /// T-620 / A11Y-001：当前是否因**键盘**拿到焦点（画 accent 焦点环）。
    /// 鼠标点击后保留焦点时为 false（DEC-14：只画灰色下划线）。
    bool HasKeyboardFocusRing() const { return m_focusTracker.KeyboardFocused(); }
private:
    void Paint();
    std::function<void()> m_action;
    wxString m_label, m_shortcut;
    bool m_chip = false, m_selected = false, m_hover = false, m_down = false, m_keyDown = false;
    bool m_activationFlash = false;
    UiFocusRing::Tracker m_focusTracker;  ///< T-620：键盘焦点 vs 点击后焦点保留
    wxTimer m_pressFeedback;
    UiTheme::Subscription m_themeSubscription;
};

class UiLayoutPicker final : public wxPopupTransientWindow {
public:
    using ReadState = std::function<const UiTheme::LayoutState&()>;
    using Apply = std::function<void(LayoutAction, int)>;
    UiLayoutPicker(wxWindow* parent, ReadState read, Apply apply);
    void OpenAt(const wxPoint& anchor);
    void Synchronize();
    size_t ChoiceCount() const { return m_choices.size(); }
protected:
    void OnDismiss() override;
private:
    struct Entry { UiLayoutChoice* control; LayoutAction action; int value; int group; };
    void OnKey(wxKeyEvent& event);
    void RestoreFocus();
    ReadState m_read;
    Apply m_apply;
    wxScrolledWindow* m_scroll = nullptr;
    std::vector<Entry> m_choices;
    wxWeakRef<wxWindow> m_returnFocus;
    wxTimer m_animation;
    wxPoint m_target;
    int m_elapsed = 0;
};

// Historical alignment helper retained for compatibility with the layout
// module. The production bottom_tabs pane now uses UiRegionHost, which keeps
// the same SetContent/SetBalance contract and adds the rounded region shell.
class UiAlignedPanel final : public wxPanel {
public:
    explicit UiAlignedPanel(wxWindow* parent);
    void SetContent(wxWindow* content);
    void SetBalance(UiTheme::Balance balance);
private:
    void Arrange();
    wxWindow* m_content = nullptr;
    UiTheme::Balance m_balance = UiTheme::Balance::Justify;
    UiTheme::Subscription m_themeSubscription;
};
