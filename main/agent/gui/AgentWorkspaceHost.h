#pragma once

#include "ui/UiTheme.h"

#include <wx/panel.h>
#include <wx/weakref.h>

namespace agent::gui {

// AGUI-T07: two presentation slots, with the existing complete workbench kept
// alive. No AUI state, project model, request or Agent session belongs here.
// Replaced windows are hidden/detached but remain owned by their wx parent.
class AgentWorkspaceHost final : public wxPanel
{
public:
    explicit AgentWorkspaceHost(wxWindow* parent, wxWindowID id = wxID_ANY);
    ~AgentWorkspaceHost() override;

    void SetWorkbench(wxWindow* window);
    void SetAgentView(wxWindow* window);
    bool ShowAgent(bool show);
    bool IsAgentShown() const;
    wxWindow* Workbench() const;
    wxWindow* AgentView() const;

private:
    wxWindow* Valid(const wxWeakRef<wxWindow>& slot) const;
    bool Prepare(wxWindow* window);
    void Release(wxWindow* window);
    void SetSlot(wxWeakRef<wxWindow>& slot, wxWindow* window);
    void Arrange();
    void UpdateVisibility();
    void RestoreWorkbenchFocus(bool allowed);
    void OnSlotDestroyed(wxWindowDestroyEvent& event);

    wxWeakRef<wxWindow> m_workbench;
    wxWeakRef<wxWindow> m_agent;
    wxWeakRef<wxWindow> m_workbenchFocus;
    bool m_agentShown = false;
    bool m_focusEntered = false;
    bool m_destroying = false;
    UiTheme::Subscription m_themeSubscription;
};

}  // namespace agent::gui
