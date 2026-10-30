#pragma once

#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>

#include <functional>

class wxPanel;
class wxFrame;
class wxWindow;

namespace agent::gui {

class AgentWorkspace;
class AgentWorkspaceHost;

// AGUI-T08 / index.html setForm(): one conversation entity, four presentations.
//
//   Center  the page itself (whole-page mode; business docks stepped aside)
//   Dock    the same page, conversation in the narrow side column and the
//           support area in the main column (prototype setForm('dock'))
//   Float   the workbench visible again, the same workspace in a detached native
//           window; native caption/header drag, clamped to desktop work area
//   Pet     the workbench visible, workspace hidden, independent desktop bubble;
//           clicking it (or Enter/Space) goes back to Center
//
// The coordinator owns presentation only: no model, request adapter, fixture or
// persistence. It never destroys the workspace and never creates a second one;
// Desktop hosts are the production default (user direction 2026-10-22).
// detachedHosts=false retains the embedded host for layout-policy fixtures.

// Four presentations of one conversation entity (index.html setForm()).
enum class AgentForm
{
    None,
    Center,
    Dock,
    Float,
    Pet
};

class AgentFormCoordinator : public wxEvtHandler
{
public:

    explicit AgentFormCoordinator(AgentWorkspaceHost* page, bool detachedHosts = true);
    ~AgentFormCoordinator() override;

    // The single workspace instance. Safe to call repeatedly; binding is
    // idempotent.
    void Attach(AgentWorkspace* workspace);
    AgentWorkspace* Workspace() const { return m_workspace; }

    // Idempotent. Form::None parks the workspace hidden in the page and returns
    // the workbench. Returns false when a non-None form has no workspace.
    bool SetPresentation(AgentForm form);
    AgentForm CurrentForm() const { return m_form; }
    bool IsPresented() const { return m_form != AgentForm::None; }
    // Full-page presentation (Center/Dock) versus a detached host.
    bool IsFullPage() const { return m_form == AgentForm::Center || m_form == AgentForm::Dock; }

    // MainFrame applies AUI dock visibility here: only it owns those panes.
    void SetFormObserver(std::function<void(AgentForm)> observer);

    // Page resize: re-clamp the overlay without moving the workspace.
    void Relayout();

    // Screen coordinates for detached hosts; page coordinates for embedded fixtures.
    wxRect FloatBounds() const;
    wxRect PetBounds() const;

    // Drag starts on the workspace header band (never on the return action).
    // AGUI-T09: when the page draws its own top bar, that child window receives
    // the gesture; positions are mapped back into workspace coordinates first.
    bool IsDragging() const { return m_dragging; }
    bool BeginDrag(const wxPoint& screenPoint);
    void UpdateDrag(const wxPoint& screenPoint);
    void EndDrag(bool cancelled);
    wxRect FloatingRect() const { return m_floatRect; }
    wxWindow* DragSource() const { return m_dragSource; }

    // Diagnostics for fixtures; not style APIs.
    wxWindow* FloatCard() const { return m_floatCard; }
    wxWindow* PetBubble() const { return m_petBubble; }
    bool WorkspaceLivesInFloatCard() const;
    wxFrame* DetachedFloatWindow() const { return m_floatWindow; }
    wxFrame* DetachedPetWindow() const { return m_petWindow; }
    unsigned PetPaintCount() const;

private:
    void OnPageSize(wxSizeEvent& event);
    void OnWorkspaceLeftDown(wxMouseEvent& event);
    void OnWorkspaceMotion(wxMouseEvent& event);
    void OnWorkspaceLeftUp(wxMouseEvent& event);
    void OnWorkspaceCaptureLost(wxMouseCaptureLostEvent& event);
    void OnWorkspaceCharHook(wxKeyEvent& event);

    void EnsureParent(wxWindow* window, wxWindow* parent);
    void BindWorkspace();
    void UnbindWorkspace();
    void LayoutFloatCard();
    void UpdatePetBubble();
    int FloatClampPx() const;
    bool FloatIsFullBleed() const;
    wxSize PageClientSize() const;
    void OnPageDpiChanged(wxDPIChangedEvent& event);
    void EnsureHosts();
    wxRect DesktopArea(wxWindow* window) const;
    void OnFloatSize(wxSizeEvent& event);
    void OnFloatMove(wxMoveEvent& event);
    void OnFloatClose(wxCloseEvent& event);
    void OnPetClose(wxCloseEvent& event);
    void OnHostDestroyed(wxWindowDestroyEvent& event);

    AgentWorkspaceHost* m_page = nullptr;
    AgentWorkspace* m_workspace = nullptr;
    wxPanel* m_floatCard = nullptr;
    wxWindow* m_petBubble = nullptr;
    bool m_detachedHosts = true;
    bool m_destroying = false;
    bool m_hasFloatPlacement = false;
    bool m_hasPetPlacement = false;
    wxFrame* m_floatWindow = nullptr;
    wxFrame* m_petWindow = nullptr;
    AgentForm m_form = AgentForm::None;
    std::function<void(AgentForm)> m_observer;

    bool m_dragging = false;
    wxPoint m_dragStartScreen;
    wxPoint m_dragStartCard;
    wxRect m_floatRect;
    bool m_workspaceBound = false;
    // The window that actually received the drag gesture (workspace background or
    // its in-page top bar). Capture is always taken/released on this window.
    wxWindow* m_dragSource = nullptr;
    wxWindow* m_captureWindow = nullptr;
};

}  // namespace agent::gui
