#include "ui/UiLayoutPicker.h"
#include <wx/access.h>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <algorithm>
#include <memory>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

namespace {
#if wxUSE_ACCESSIBILITY
class ChoiceAccessible final : public wxAccessible {
public:
    explicit ChoiceAccessible(UiLayoutChoice* choice) : wxAccessible(choice), m_choice(choice) {}
    wxAccStatus GetRole(int child, wxAccRole* role) override {
        if (child) return wxACC_NOT_IMPLEMENTED;
        *role = wxROLE_SYSTEM_CHECKBUTTON; return wxACC_OK;
    }
    wxAccStatus GetState(int child, long* state) override {
        if (child) return wxACC_NOT_IMPLEMENTED;
        *state = wxACC_STATE_SYSTEM_FOCUSABLE;
        if (!m_choice->IsEnabled()) *state |= wxACC_STATE_SYSTEM_UNAVAILABLE;
        if (m_choice->IsSelected()) *state |= wxACC_STATE_SYSTEM_CHECKED;
        if (m_choice->HasFocus()) *state |= wxACC_STATE_SYSTEM_FOCUSED;
        return wxACC_OK;
    }
    wxAccStatus GetName(int child, wxString* name) override {
        if (child) return wxACC_NOT_IMPLEMENTED;
        *name = m_choice->GetName(); return wxACC_OK;
    }
    wxAccStatus GetDefaultAction(int child, wxString* name) override {
        if (child) return wxACC_NOT_IMPLEMENTED;
        *name = "Activate"; return wxACC_OK;
    }
    wxAccStatus DoDefaultAction(int child) override {
        if (child) return wxACC_NOT_IMPLEMENTED;
        m_choice->Activate(); return wxACC_OK;
    }
private:
    UiLayoutChoice* m_choice;
};
#endif

bool AnimatePopup() {
#ifdef __WXMSW__
    BOOL enabled = FALSE;
    return ::SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && enabled;
#else
    return false; // Static fallback when the platform preference is unavailable.
#endif
}
}

UiLayoutChoice::UiLayoutChoice(wxWindow* parent, const wxString& label,
                               std::function<void()> action, const wxString& shortcut, bool chip)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      m_action(std::move(action)), m_label(label), m_shortcut(shortcut), m_chip(chip), m_pressFeedback(this) {
    // V6 / F7：自绘控件也必须给出**可访问名**。原实现只 SetName/SetToolTip，
    // GetLabel() 为空 → 屏幕阅读器与自动化工具拿不到标签。补 SetLabel（自绘内容不受影响）。
    SetName(label); SetLabel(label); SetToolTip(label); SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(chip ? 68 : 100, 28)));
#if wxUSE_ACCESSIBILITY
    SetAccessible(new ChoiceAccessible(this));
#endif
    m_themeSubscription = UiTheme::Subscribe([this] { Refresh(false); });
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { Paint(); });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { UiFocusTrace::NotePointer(); m_hover = true; Refresh(false); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { UiFocusTrace::NotePointer(); m_hover = false; Refresh(false); });
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        // A11Y-001：键盘（Tab/方向键）把焦点交给本行时画 accent 焦点环；
        // 点击后保留焦点走 DEC-14 的灰色下划线。
        m_focusTracker.OnFocusGained();
        Refresh(false);
        e.Skip();
    });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        m_keyDown = false;
        m_focusTracker.OnFocusLost();
        m_down = false;
        if (HasCapture()) ReleaseMouse();  // 不得留下「点住后按 Tab」的捕获陷阱（A11Y-002）
        Refresh(false);
        e.Skip();
    });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        UiFocusTrace::NotePointer();
        if (IsEnabled()) { SetFocus(); m_down = true; if (!HasCapture()) CaptureMouse(); Refresh(false); }
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        const bool activate = m_down && GetClientRect().Contains(e.GetPosition());
        m_down = false; if (HasCapture()) ReleaseMouse();
        Refresh(false);
        if (activate) Activate();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_down = false; Refresh(false); });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        UiFocusTrace::NoteKeyboard();
        if (e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN) {
            if (IsEnabled()) { m_keyDown = true; Refresh(false); }
        }
        else e.Skip();
    });
    Bind(wxEVT_KEY_UP, [this](wxKeyEvent& e) {
        UiFocusTrace::NoteKeyboard();
        if (e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN) {
            const bool activate = m_keyDown; m_keyDown = false; Refresh(false); if (activate) Activate();
        } else e.Skip();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_activationFlash = false; Refresh(false); }, m_pressFeedback.GetId());
}

void UiLayoutChoice::Activate() {
    if (!IsEnabled() || !m_action) return;
    // The popup's CHAR_HOOK and accessibility action also enter here, so their
    // activation receives the same short feedback as a pointer press.
    m_activationFlash = true;
    m_pressFeedback.StartOnce(UiTheme::Motion::Press);
    Refresh(false);
    m_action();
}
void UiLayoutChoice::SetSelected(bool value) {
    if (m_selected != value) { m_selected = value; Refresh(false); }
}

void UiLayoutChoice::Paint() {
    wxAutoBufferedPaintDC dc(this);
    const auto& c = UiTheme::Palette();
    dc.SetBackground(wxBrush(c.surfaceOnWindow)); dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    const double dip = FromDIP(100) / 100.0;
    const auto r = GetClientRect();
    gc->SetPen(m_chip ? wxPen(c.strokeOnPanel) : *wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(m_selected ? c.selOnPanel : (m_hover && IsEnabled() ? c.hoverOnPanel : c.surfaceOnWindow)));
    gc->DrawRoundedRectangle(dip, dip, std::max(0.0, r.width - 2*dip), std::max(0.0, r.height - 2*dip), (m_chip ? 13 : 6)*dip);
    if (IsEnabled() && (m_hover || m_down || m_keyDown || m_activationFlash)) {
        // CMP-031 / DEC-14：蓝边只在 hover / 按压（含键盘按住）时出现。
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(c.focusRingOnPanel).Width(1.5*dip)));
        gc->DrawRoundedRectangle(2*dip, 2*dip, std::max(0.0,r.width-4*dip), std::max(0.0,r.height-4*dip), (m_chip ? 12 : 5)*dip);
    } else if (IsEnabled() && HasFocus()) {
        // DEC-14：键盘和鼠标聚焦均用灰线提示，蓝边仅用于 hover / 按压。
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(c.text3).Width(dip)));
        gc->StrokeLine(12*dip, r.height-5*dip, r.width-12*dip, r.height-5*dip);
    }
    gc->SetFont(UiTheme::UiFont(this, m_chip ? UiTheme::TextSize::P11 : UiTheme::TextSize::P12),
                IsEnabled() ? (m_selected && m_chip ? c.accent : c.text1) : c.text3);
    double w=0,h=0; gc->GetTextExtent(m_label,&w,&h);
    gc->DrawText(m_label, m_chip ? std::max(4*dip,(r.width-w)/2) : 10*dip, (r.height-h)/2);
    gc->SetFont(UiTheme::UiFont(this, UiTheme::TextSize::P11), c.text3);
    gc->GetTextExtent(m_shortcut,&w,&h);
    if (!m_shortcut.empty()) gc->DrawText(m_shortcut,r.width-w-30*dip,(r.height-h)/2);
    if (m_selected && !m_chip) {
        gc->SetFont(UiTheme::UiFont(this, UiTheme::TextSize::P12), c.accent);
        gc->DrawText(wxString::FromUTF8("✓"),r.width-23*dip,(r.height-h)/2);
    }
}

UiLayoutPicker::UiLayoutPicker(wxWindow* parent, ReadState read, Apply apply)
    : wxPopupTransientWindow(parent, wxBORDER_SIMPLE | wxPU_CONTAINS_CONTROLS), m_read(std::move(read)),
      m_apply(std::move(apply)), m_animation(this) {
    SetName("Customize Layout");
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(this,wxID_ANY,"Customize Layout");
    title->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12,UiTheme::Weight::Semibold));
    head->Add(title,1,wxALIGN_CENTER_VERTICAL|wxLEFT,FromDIP(8));
    auto* close = new UiLayoutChoice(this,"Esc",[this] { Dismiss(); RestoreFocus(); },{},true);
    close->SetMinSize(FromDIP(wxSize(44,28)));
    head->Add(close,0,wxALIGN_CENTER_VERTICAL);
    outer->Add(head,0,wxEXPAND|wxALL,FromDIP(6));
    m_scroll = new wxScrolledWindow(this,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxVSCROLL|wxBORDER_NONE);
    m_scroll->SetScrollRate(0,FromDIP(28));
    auto* rows = new wxBoxSizer(wxVERTICAL);
    auto group = [&](const wxString& label) {
        auto* text = new wxStaticText(m_scroll,wxID_ANY,label);
        text->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P10));
        text->SetForegroundColour(UiTheme::Palette().text3);
        rows->Add(text,0,wxLEFT|wxTOP|wxBOTTOM,FromDIP(8));
    };
    auto add = [&](wxSizer* sizer,const wxString& label,LayoutAction action,int value,int groupId,
                   const wxString& shortcut=wxString(),bool chip=false) {
        auto* control = new UiLayoutChoice(m_scroll,label,[this,action,value] { m_apply(action,value); Synchronize(); },shortcut,chip);
        m_choices.push_back({control,action,value,groupId});
        sizer->Add(control,chip ? 1 : 0,wxEXPAND|wxALL,FromDIP(1));
    };
    group("Layout presets");
    auto* chips = new wxBoxSizer(wxHORIZONTAL);
    const char* labels[] = {"Classic","Code","Canvas","Chat"};
    for(int i=0;i<4;++i) add(chips,labels[i],LayoutAction::Preset,i,0,{},true);
    rows->Add(chips,0,wxEXPAND);
    group("Visibility");
    const char* names[] = {"Menu Bar","Activity Bar","Primary Side Bar","Secondary Side Bar","Panel","Status Bar"};
    const char* keys[] = {"Ctrl+Shift+M","Ctrl+Shift+A","Ctrl+B","Ctrl+Alt+B","Ctrl+J",""};
    for(int i=0;i<6;++i) add(rows,names[i],LayoutAction::Visibility,i,1,keys[i]);
    group("Primary side bar position");
    add(rows,"Left",LayoutAction::Side,0,2); add(rows,"Right",LayoutAction::Side,1,2);
    group("Panel alignment");
    const char* align[] = {"Align Left","Align Right","Center","Justify"};
    for(int i=0;i<4;++i) add(rows,align[i],LayoutAction::Balance,i,3);
    group("Panel position");
    add(rows,"Bottom",LayoutAction::Position,0,4); add(rows,"Top",LayoutAction::Position,1,4);
    group("Separate windows (later version)");
    for(const char* name : {"Canvas — later","Code Editor — later","Agent Chat — later"}) {
        auto* control = new UiLayoutChoice(m_scroll,wxString::FromUTF8(name),[] {});
        control->Enable(false);
        control->SetToolTip("Deferred by DEC-06; native AUI pane floating remains available.");
        rows->Add(control,0,wxEXPAND|wxALL,FromDIP(1));
    }
    m_scroll->SetSizer(rows); m_scroll->FitInside();
    outer->Add(m_scroll,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(6));
    SetSizer(outer);
    Bind(wxEVT_CHAR_HOOK,&UiLayoutPicker::OnKey,this);
    Bind(wxEVT_TIMER,[this](wxTimerEvent&) {
        m_elapsed = std::min(UiTheme::Motion::Quick,m_elapsed+15);
        const double t = 1.0 - m_elapsed/double(UiTheme::Motion::Quick);
        Move(m_target+wxPoint(0,int(FromDIP(6)*t*t)));
        if(m_elapsed == UiTheme::Motion::Quick) m_animation.Stop();
    },m_animation.GetId());
    Synchronize();
}

void UiLayoutPicker::Synchronize() {
    const auto& state=m_read(); const auto presets=UiTheme::PresetIds();
    for(const auto& item:m_choices) {
        bool selected=false;
        switch(item.action) {
            case LayoutAction::Preset:selected=UiTheme::MatchesPreset(state,presets[item.value]);break;
            case LayoutAction::Visibility:selected=UiTheme::GetVisibility(state,static_cast<UiTheme::Visibility>(item.value));break;
            case LayoutAction::Side:selected=int(state.side)==item.value;break;
            case LayoutAction::Balance:selected=int(state.bal)==item.value;break;
            case LayoutAction::Position:selected=int(state.pos)==item.value;break;
        }
        item.control->SetSelected(selected);
    }
    SetBackgroundColour(UiTheme::Palette().surfaceOnWindow);
    m_scroll->SetBackgroundColour(UiTheme::Palette().surfaceOnWindow);
}

void UiLayoutPicker::OpenAt(const wxPoint& anchor) {
    m_returnFocus=wxWindow::FindFocus(); Synchronize();
    int displayIndex=wxDisplay::GetFromPoint(anchor);
    if(displayIndex==wxNOT_FOUND) displayIndex=0;
    const wxRect screen=wxDisplay(displayIndex).GetClientArea();
    const int desired=m_scroll->GetSizer()->GetMinSize().y+FromDIP(52);
    SetSize(FromDIP(340),std::min(desired,screen.height-FromDIP(24)));
    m_target=wxPoint(std::clamp(anchor.x-GetSize().x,screen.x,screen.GetRight()-GetSize().x+1),
                     std::clamp(anchor.y,screen.y,screen.GetBottom()-GetSize().y+1));
    Move(m_target); Popup(m_choices.front().control);
    m_choices.front().control->SetFocus(); m_scroll->Scroll(0,0);
    if(AnimatePopup()) { m_elapsed=0; Move(m_target+wxPoint(0,FromDIP(6))); m_animation.Start(15); }
}

void UiLayoutPicker::OnKey(wxKeyEvent& event) {
    const int key=event.GetKeyCode();
    // T-620：浮层内部的键盘导航同样属于「键盘输入」，让被聚焦的行画 A11Y-001 焦点环。
    UiFocusTrace::NoteKeyboard();
    if(key==WXK_ESCAPE) { Dismiss(); RestoreFocus(); return; }
    // KEY-007/INT-019：Ctrl+Tab / Alt+Tab 之类的系统组合不得被浮层吞掉。
    if(key==WXK_TAB && (event.ControlDown() || event.AltDown())) { event.Skip(); return; }
    const auto it=std::find_if(m_choices.begin(),m_choices.end(),[](const Entry& e){return e.control->HasFocus();});
    if(it==m_choices.end()) { if(key==WXK_TAB) {m_choices.front().control->SetFocus();return;} event.Skip();return; }
    // Popup/dialog processing may consume Return/Space before the child gets
    // KEY_DOWN/KEY_UP. Activate here and consume the hook, once per press.
    if(key==WXK_SPACE || key==WXK_RETURN || key==WXK_NUMPAD_ENTER) {
        if(!event.IsAutoRepeat()) it->control->Activate();
        return;
    }
    int index=int(it-m_choices.begin()),target=index;
    if(key==WXK_TAB || key==WXK_UP || key==WXK_DOWN) {
        const int direction=(key==WXK_UP || (key==WXK_TAB && event.ShiftDown())) ? -1 : 1;
        target=(index+direction+int(m_choices.size()))%int(m_choices.size());
        if(key!=WXK_TAB && it->group!=1 && m_choices[target].group==it->group)
            m_choices[target].control->Activate();
    } else if(key==WXK_LEFT || key==WXK_RIGHT) {
        const int group=it->group;
        if(group==1) { event.Skip();return; }
        const int direction=key==WXK_LEFT?-1:1;
        do {target=(target+direction+int(m_choices.size()))%int(m_choices.size());} while(m_choices[target].group!=group);
        m_choices[target].control->Activate();
    } else { event.Skip();return; }
    m_choices[target].control->SetFocus();
    const auto row=m_scroll->CalcUnscrolledPosition(m_choices[target].control->GetPosition());
    int scrollX=0,scrollY=0; m_scroll->GetViewStart(&scrollX,&scrollY);
    const int step=FromDIP(28),top=scrollY*step;
    if(row.y<top) m_scroll->Scroll(-1,row.y/step);
    else if(row.y+step>top+m_scroll->GetClientSize().y)
        m_scroll->Scroll(-1,(row.y+2*step-m_scroll->GetClientSize().y)/step);
}

void UiLayoutPicker::RestoreFocus() {
    m_animation.Stop();
    if(m_returnFocus && m_returnFocus->IsShownOnScreen()) m_returnFocus->SetFocus();
}
void UiLayoutPicker::OnDismiss() {
    // An outside click may activate another application. Only an explicit Esc
    // or close action restores the old focus; never steal it on deactivation.
    m_animation.Stop();
}

UiAlignedPanel::UiAlignedPanel(wxWindow* parent) : wxPanel(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_NONE) {
    SetBackgroundColour(UiTheme::Palette().bgWindow);
    m_themeSubscription=UiTheme::Subscribe([this] { SetBackgroundColour(UiTheme::Palette().bgWindow);Refresh(false); });
    Bind(wxEVT_SIZE,[this](wxSizeEvent& e){Arrange();e.Skip();});
}
void UiAlignedPanel::SetContent(wxWindow* content) { m_content=content;Arrange(); }
void UiAlignedPanel::SetBalance(UiTheme::Balance balance) { m_balance=balance;Arrange(); }
void UiAlignedPanel::Arrange() {
    if(m_content) m_content->SetSize(UiTheme::AlignedPanelRect(GetClientSize(),m_balance,FromDIP(100)/100.0));
}
