#include "agent/gui/AgentWorkspaceHost.h"

#include <wx/sizer.h>

#include <algorithm>

namespace agent::gui {
namespace {

bool Contains(wxWindow* root, wxWindow* window)
{
    return root && window && (root == window || root->IsDescendant(window));
}

}  // namespace

AgentWorkspaceHost::AgentWorkspaceHost(wxWindow* parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetName("AgentWorkspaceHost");
    SetBackgroundColour(UiTheme::Palette().bgWindow);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        UpdateVisibility();
        event.Skip();
    });
    m_themeSubscription = UiTheme::Subscribe([this] {
        SetBackgroundColour(UiTheme::Palette().bgWindow);
        Refresh(false);
    });
}

AgentWorkspaceHost::~AgentWorkspaceHost()
{
    m_destroying = true;
    m_themeSubscription.Reset();
    // Disconnect while child objects are still alive. wxWeakRef also protects
    // externally destroyed children, and parent validation protects migration.
    for (auto* child : {m_workbench.get(), m_agent.get()}) {
        if (child) child->Unbind(wxEVT_DESTROY, &AgentWorkspaceHost::OnSlotDestroyed, this);
    }
    m_workbench = nullptr;
    m_agent = nullptr;
    m_workbenchFocus = nullptr;
}

wxWindow* AgentWorkspaceHost::Valid(const wxWeakRef<wxWindow>& slot) const
{
    auto* window = slot.get();
    return window && !window->IsBeingDeleted() && window->GetParent() == this ? window : nullptr;
}

wxWindow* AgentWorkspaceHost::Workbench() const { return Valid(m_workbench); }
wxWindow* AgentWorkspaceHost::AgentView() const { return Valid(m_agent); }
bool AgentWorkspaceHost::IsAgentShown() const { return m_agentShown && AgentView(); }
bool AgentWorkspaceHost::Prepare(wxWindow* window)
{
    if (!window) return true;
    if (window == this || window->IsBeingDeleted() || window->IsDescendant(this)) return false;
    // Explicit migration between these hosts invalidates the old slot before
    // Reparent. Arbitrary external Reparent is additionally guarded by Valid().
    if (auto* previous = dynamic_cast<AgentWorkspaceHost*>(window->GetParent());
        previous && previous != this) previous->Release(window);
    if (auto* sizer = window->GetContainingSizer()) sizer->Detach(window);
    return window->GetParent() == this || window->Reparent(this);
}

void AgentWorkspaceHost::SetSlot(wxWeakRef<wxWindow>& slot, wxWindow* window)
{
    if (slot.get() == window && (!window || window->GetParent() == this)) return;
    if (!Prepare(window)) return;
    if (auto* old = slot.get()) {
        old->Unbind(wxEVT_DESTROY, &AgentWorkspaceHost::OnSlotDestroyed, this);
        if (old->GetParent() == this) {
            old->Hide();
            if (auto* sizer = old->GetContainingSizer()) sizer->Detach(old);
        }
    }
    slot = window;
    if (window) window->Bind(wxEVT_DESTROY, &AgentWorkspaceHost::OnSlotDestroyed, this);
}

void AgentWorkspaceHost::SetWorkbench(wxWindow* window)
{
    if (window && window == AgentView()) return;
    SetSlot(m_workbench, window);
    if (!Contains(Workbench(), m_workbenchFocus.get())) m_workbenchFocus = nullptr;
    UpdateVisibility();
}

void AgentWorkspaceHost::SetAgentView(wxWindow* window)
{
    if (window && window == Workbench()) return;
    const bool restore = IsAgentShown() &&
        (Contains(AgentView(), wxWindow::FindFocus()) ||
         (!wxWindow::FindFocus() && m_focusEntered));
    SetSlot(m_agent, window);
    if (!AgentView()) m_agentShown = false;
    UpdateVisibility();
    if (!AgentView()) RestoreWorkbenchFocus(restore);
}

void AgentWorkspaceHost::Release(wxWindow* window)
{
    if (window == m_agent.get()) SetAgentView(nullptr);
    if (window == m_workbench.get()) SetWorkbench(nullptr);
}

bool AgentWorkspaceHost::ShowAgent(bool show)
{
    if (show && !AgentView()) {
        m_agentShown = false;
        UpdateVisibility();
        return false;
    }
    if (IsAgentShown() == show) {
        UpdateVisibility();
        return true;
    }
    auto* focus = wxWindow::FindFocus();
    const bool ownedFocus = Contains(show ? Workbench() : AgentView(), focus) || focus == this;
    if (show) {
        m_workbenchFocus = Contains(Workbench(), focus) ? focus : nullptr;
        m_focusEntered = ownedFocus;
    }
    const bool restore = ownedFocus || (!focus && m_focusEntered);
    m_agentShown = show;
    UpdateVisibility();
    if (show) {
        if (ownedFocus && AgentView()->IsShownOnScreen() && AgentView()->IsEnabled())
            AgentView()->SetFocus();
    } else RestoreWorkbenchFocus(restore);
    return true;
}

void AgentWorkspaceHost::RestoreWorkbenchFocus(bool allowed)
{
    m_focusEntered = false;
    if (!allowed) return;
    auto* workbench = Workbench();
    if (!workbench || !workbench->IsShownOnScreen() || !workbench->IsEnabled()) return;
    auto* focus = m_workbenchFocus.get();
    if (Contains(workbench, focus) && !focus->IsBeingDeleted() &&
        focus->IsShownOnScreen() && focus->IsEnabled() && focus->AcceptsFocus()) focus->SetFocus();
    else workbench->SetFocus();
}

void AgentWorkspaceHost::UpdateVisibility()
{
    if (m_destroying) return;
    if (!AgentView()) m_agentShown = false;
    if (auto* workbench = Workbench()) workbench->Show(!m_agentShown);
    if (auto* agent = AgentView()) agent->Show(m_agentShown);
    Arrange();
}

void AgentWorkspaceHost::Arrange()
{
    const auto size = GetClientSize();
    const wxRect bounds(0, 0, std::max(0, size.x), std::max(0, size.y));
    // Both slots receive the same bounds, including the hidden one, so the
    // original splitter keeps its internal layout while switching visibility.
    for (auto* view : {Workbench(), AgentView()}) {
        if (view) { view->SetSize(bounds); view->Layout(); }
    }
}

void AgentWorkspaceHost::OnSlotDestroyed(wxWindowDestroyEvent& event)
{
    if (m_destroying) { event.Skip(); return; }
    auto* destroyed = event.GetEventObject();
    if (destroyed == m_agent.get()) {
        auto* focus = wxWindow::FindFocus();
        const bool restore = Contains(m_agent.get(), focus) || (!focus && m_focusEntered);
        m_agent = nullptr;
        m_agentShown = false;
        UpdateVisibility();
        RestoreWorkbenchFocus(restore);
    }
    if (destroyed == m_workbench.get()) {
        m_workbench = nullptr;
        m_workbenchFocus = nullptr;
    }
    event.Skip();
}

}  // namespace agent::gui
