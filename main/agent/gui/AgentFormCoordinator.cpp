#include "agent/gui/AgentFormCoordinator.h"

#include "agent/gui/AgentIcons.h"
#include "agent/gui/AgentLayoutPolicy.h"
#include "agent/gui/AgentTokens.h"
#include "agent/gui/AgentWorkspace.h"
#include "agent/gui/AgentWorkspaceHost.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/sizer.h>
#include <wx/dcclient.h>
#include <wx/panel.h>
#include <wx/frame.h>
#include <wx/display.h>
#include <wx/utils.h>
#include <wx/settings.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#include <algorithm>
#include <memory>
#include <utility>

namespace agent::gui {
namespace {

namespace M = UiTheme::AgentMetrics;
namespace T = UiTheme::AgentTypography;

int Dip(const wxWindow* window, int value)
{
    return tokens::CssPxToDip(window, value);
}

wxString Text(const char* utf8)
{
    return wxString::FromUTF8(utf8);
}

// Standby bubble: the fixed two-ball artwork from T06 plus an honest caption.
// T20 replaces the content with the animated pet; this form contract stays.
class PetStandbyBubble final : public wxPanel
{
public:
    PetStandbyBubble(wxWindow* parent, std::function<void()> open, bool desktop)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                  wxBORDER_NONE | wxTAB_TRAVERSAL),
          m_open(std::move(open)), m_desktop(desktop)
    {
        SetName("agent-pet-bubble");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetToolTip(Text("点我打开正式对话"));
        Bind(wxEVT_PAINT, &PetStandbyBubble::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            m_pressed = true;
            m_moved = false;
            m_start = ClientToScreen(event.GetPosition());
            m_origin = GetParent()->GetPosition();
            CaptureMouse();
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_pressed || !m_desktop) return;
            const auto delta = ClientToScreen(event.GetPosition()) - m_start;
            if (std::abs(delta.x) >= wxSystemSettings::GetMetric(wxSYS_DRAG_X) ||
                std::abs(delta.y) >= wxSystemSettings::GetMetric(wxSYS_DRAG_Y)) m_moved = true;
            if (m_moved) GetParent()->Move(m_origin + delta);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            const bool open = m_pressed && !m_moved;
            m_pressed = false;
            if (HasCapture()) ReleaseMouse();
            if (open) Open();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_pressed = false;
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE) {
                Open();
                return;
            }
            event.Skip();
        });
        m_theme = tokens::SubscribeTheme([this] { Refresh(false); });
    }

    // AGUI-T11: <=560 uses the narrow artwork size (140 DIP); the container width
    // itself is set by the layout policy (200 DIP).
    void SetNarrow(bool narrow)
    {
        if (m_narrow == narrow) return;
        m_narrow = narrow;
        Refresh(false);
    }

    bool AcceptsFocus() const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return true; }
    unsigned PaintCount() const { return m_paints; }

private:
    void Open()
    {
        if (m_open) m_open();
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0) return;
        const auto& colours = tokens::Colours();
        dc.SetBackground(wxBrush(m_desktop ? wxColour(255, 0, 255) : tokens::SurfaceColour(tokens::Component::Card)));
        dc.Clear();

        const int radius = Dip(this, M::CardRadius);
        {
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc) return;
            gc->SetPen(wxPen(tokens::BorderColour(tokens::Component::Card), 1));
            gc->SetBrush(wxBrush(tokens::SurfaceColour(tokens::Component::Card)));
            if (!m_desktop) gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, radius);

            const int figure = Dip(this, m_narrow ? M::PetFigureSizeNarrow : M::PetFigureSize);
            const int art = std::min(figure, std::max(0, size.x - Dip(this, 16)));
            const int artY = Dip(this, 12);
            if (art > 0) {
                icons::DrawPetArtwork(*gc, wxRect((size.x - art) / 2, artY, art, art), m_narrow);
            }
            gc->SetFont(tokens::UiFontPx(this, T::SmallBodyPx), colours.text2);
            const wxString caption = Text("点我打开正式对话");
            double textWidth = 0;
            double textHeight = 0;
            gc->GetTextExtent(caption, &textWidth, &textHeight);
            if (!m_desktop) gc->DrawText(caption, std::max(0.0, (size.x - textWidth) / 2.0),
                         static_cast<double>(artY + art + Dip(this, 8)));
        }
        ++m_paints;
        if (HasFocus() && !m_desktop) {
            dc.SetPen(wxPen(colours.accent, 2));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, radius);
        }
    }

    std::function<void()> m_open;
    bool m_desktop = false;
    bool m_pressed = false;
    bool m_moved = false;
    wxPoint m_start, m_origin;
    bool m_narrow = false;
    unsigned m_paints = 0;
    UiTheme::Subscription m_theme;
};

}  // namespace

AgentFormCoordinator::AgentFormCoordinator(AgentWorkspaceHost* page, bool detachedHosts)
    : m_page(page), m_detachedHosts(detachedHosts)
{
    if (m_page == nullptr) return;
    EnsureHosts();
    m_page->Bind(wxEVT_SIZE, &AgentFormCoordinator::OnPageSize, this);
    m_page->Bind(wxEVT_DPI_CHANGED, &AgentFormCoordinator::OnPageDpiChanged, this);
}

void AgentFormCoordinator::EnsureHosts()
{
    if (m_destroying || !m_page) return;
    if (m_detachedHosts && !m_floatWindow) {
        // Top-level, unowned hosts survive hiding/minimizing the workbench.
        // The coordinator explicitly owns and destroys both before MainFrame dies.
        m_floatWindow = new wxFrame(nullptr, wxID_ANY, Text("SigFlow · Agent"),
            wxDefaultPosition, wxDefaultSize,
            (wxDEFAULT_FRAME_STYLE & ~wxMINIMIZE_BOX & ~wxMAXIMIZE_BOX) |
            wxFRAME_TOOL_WINDOW | wxFRAME_NO_TASKBAR | wxSTAY_ON_TOP);
        m_floatWindow->SetName("agent-detached-float");
        m_floatWindow->Bind(wxEVT_SIZE, &AgentFormCoordinator::OnFloatSize, this);
        m_floatWindow->Bind(wxEVT_MOVE, &AgentFormCoordinator::OnFloatMove, this);
        m_floatWindow->Bind(wxEVT_CLOSE_WINDOW, &AgentFormCoordinator::OnFloatClose, this);
        m_floatWindow->Bind(wxEVT_DESTROY, &AgentFormCoordinator::OnHostDestroyed, this);
        m_floatWindow->Bind(wxEVT_DPI_CHANGED, &AgentFormCoordinator::OnPageDpiChanged, this);
    }
    if (m_detachedHosts && !m_petWindow) {
        m_petWindow = new wxFrame(nullptr, wxID_ANY, Text("SigFlow 桌宠"),
            wxDefaultPosition, wxDefaultSize,
            wxBORDER_NONE | wxFRAME_TOOL_WINDOW | wxFRAME_NO_TASKBAR | wxSTAY_ON_TOP);
        m_petWindow->SetName("agent-detached-pet");
#ifdef __WXMSW__
        // Colour-key only the empty background; the artwork stays fully opaque.
        const auto hwnd = reinterpret_cast<HWND>(m_petWindow->GetHandle());
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, RGB(255, 0, 255), 255, LWA_COLORKEY);
#endif
        m_petWindow->Bind(wxEVT_CLOSE_WINDOW, &AgentFormCoordinator::OnPetClose, this);
        m_petWindow->Bind(wxEVT_DESTROY, &AgentFormCoordinator::OnHostDestroyed, this);
        m_petWindow->Bind(wxEVT_DPI_CHANGED, &AgentFormCoordinator::OnPageDpiChanged, this);
    }
    if (!m_floatCard) {
        m_floatCard = new wxPanel(m_detachedHosts ? static_cast<wxWindow*>(m_floatWindow) : m_page,
                              wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxBORDER_NONE | wxTAB_TRAVERSAL);
        m_floatCard->SetName("agent-float-card");
        m_floatCard->SetBackgroundColour(tokens::SurfaceColour(tokens::Component::Card));
        m_floatCard->Hide();
    }
    if (!m_petBubble) {
        m_petBubble = new PetStandbyBubble(
            m_detachedHosts ? static_cast<wxWindow*>(m_petWindow) : m_page,
            [this] { SetPresentation(AgentForm::Center); },
#ifdef __WXMSW__
            m_detachedHosts);
#else
            false);
#endif
        m_petBubble->Hide();
    }
}

AgentFormCoordinator::~AgentFormCoordinator()
{
    m_destroying = true;
    EndDrag(true);
    UnbindWorkspace();
    if (m_page != nullptr) {
        m_page->Unbind(wxEVT_SIZE, &AgentFormCoordinator::OnPageSize, this);
        m_page->Unbind(wxEVT_DPI_CHANGED, &AgentFormCoordinator::OnPageDpiChanged, this);
    }
    // The card owns the workspace while floating: hand it back before the card
    // is destroyed so the frame's workspace outlives this coordinator.
    if (m_workspace != nullptr && m_page != nullptr &&
        m_workspace->GetParent() == m_floatCard) {
        m_workspace->Reparent(m_page);
    }
    if (m_workspace != nullptr) m_workspace->Hide();
    if (m_detachedHosts) {
        delete m_floatWindow;
        delete m_petWindow;
        m_floatWindow = m_petWindow = nullptr;
        m_floatCard = nullptr;
        m_petBubble = nullptr;
    } else if (m_floatCard != nullptr) {
        m_floatCard->Destroy();
        m_floatCard = nullptr;
    }
    if (m_petBubble != nullptr) {
        m_petBubble->Destroy();
        m_petBubble = nullptr;
    }
}

void AgentFormCoordinator::Attach(AgentWorkspace* workspace)
{
    if (workspace == nullptr || workspace == m_workspace) return;
    UnbindWorkspace();
    m_workspace = workspace;
    BindWorkspace();
    if (m_page != nullptr && m_workspace->GetParent() != m_page) {
        m_workspace->Reparent(m_page);
    }
}

void AgentFormCoordinator::BindWorkspace()
{
    if (m_workspace == nullptr || m_workspaceBound) return;
    m_workspaceBound = true;
    m_workspace->Bind(wxEVT_LEFT_DOWN, &AgentFormCoordinator::OnWorkspaceLeftDown, this);
    m_workspace->Bind(wxEVT_MOTION, &AgentFormCoordinator::OnWorkspaceMotion, this);
    m_workspace->Bind(wxEVT_LEFT_UP, &AgentFormCoordinator::OnWorkspaceLeftUp, this);
    m_workspace->Bind(wxEVT_MOUSE_CAPTURE_LOST,
                      &AgentFormCoordinator::OnWorkspaceCaptureLost, this);
    m_workspace->Bind(wxEVT_CHAR_HOOK, &AgentFormCoordinator::OnWorkspaceCharHook, this);
    // AGUI-T09: the page owns an in-page top bar; its brand band is the drag
    // source (the buttons inside it consume their own clicks).
    m_dragSource = m_workspace->DragSourceWindow();
    if (m_dragSource != nullptr && m_dragSource != m_workspace) {
        m_dragSource->Bind(wxEVT_LEFT_DOWN, &AgentFormCoordinator::OnWorkspaceLeftDown, this);
        m_dragSource->Bind(wxEVT_MOTION, &AgentFormCoordinator::OnWorkspaceMotion, this);
        m_dragSource->Bind(wxEVT_LEFT_UP, &AgentFormCoordinator::OnWorkspaceLeftUp, this);
        m_dragSource->Bind(wxEVT_MOUSE_CAPTURE_LOST,
                           &AgentFormCoordinator::OnWorkspaceCaptureLost, this);
    } else {
        m_dragSource = m_workspace;
    }
}

void AgentFormCoordinator::UnbindWorkspace()
{
    if (m_workspace == nullptr || !m_workspaceBound) return;
    m_workspaceBound = false;
    m_workspace->Unbind(wxEVT_LEFT_DOWN, &AgentFormCoordinator::OnWorkspaceLeftDown, this);
    m_workspace->Unbind(wxEVT_MOTION, &AgentFormCoordinator::OnWorkspaceMotion, this);
    m_workspace->Unbind(wxEVT_LEFT_UP, &AgentFormCoordinator::OnWorkspaceLeftUp, this);
    m_workspace->Unbind(wxEVT_MOUSE_CAPTURE_LOST,
                        &AgentFormCoordinator::OnWorkspaceCaptureLost, this);
    m_workspace->Unbind(wxEVT_CHAR_HOOK, &AgentFormCoordinator::OnWorkspaceCharHook, this);
    if (m_dragSource != nullptr && m_dragSource != m_workspace) {
        m_dragSource->Unbind(wxEVT_LEFT_DOWN, &AgentFormCoordinator::OnWorkspaceLeftDown, this);
        m_dragSource->Unbind(wxEVT_MOTION, &AgentFormCoordinator::OnWorkspaceMotion, this);
        m_dragSource->Unbind(wxEVT_LEFT_UP, &AgentFormCoordinator::OnWorkspaceLeftUp, this);
        m_dragSource->Unbind(wxEVT_MOUSE_CAPTURE_LOST,
                             &AgentFormCoordinator::OnWorkspaceCaptureLost, this);
    }
    m_dragSource = nullptr;
    m_captureWindow = nullptr;
}

void AgentFormCoordinator::OnPageSize(wxSizeEvent& event)
{
    Relayout();
    event.Skip();
}

void AgentFormCoordinator::OnWorkspaceLeftDown(wxMouseEvent& event)
{
    if (m_form == AgentForm::Float && m_workspace != nullptr) {
        // The gesture may arrive from the workspace background or from the
        // in-page top bar; test the header band in workspace coordinates.
        wxPoint position = event.GetPosition();
        auto* source = dynamic_cast<wxWindow*>(event.GetEventObject());
        if (source != nullptr && source != m_workspace) {
            position = m_workspace->ScreenToClient(source->ClientToScreen(position));
        }
        if (m_workspace->DragHandleRect().Contains(position)) {
            const wxPoint screen =
                source != nullptr ? source->ClientToScreen(event.GetPosition())
                                  : m_workspace->ClientToScreen(event.GetPosition());
            if (BeginDrag(screen)) {
                m_captureWindow = source != nullptr ? source : m_workspace;
                if (m_captureWindow != nullptr && !m_captureWindow->HasCapture()) {
                    m_captureWindow->CaptureMouse();
                }
            }
        }
    }
    event.Skip();
}

void AgentFormCoordinator::OnWorkspaceMotion(wxMouseEvent& event)
{
    if (m_dragging && m_workspace != nullptr) {
        auto* source = dynamic_cast<wxWindow*>(event.GetEventObject());
        if (source == nullptr) source = m_workspace;
        UpdateDrag(source->ClientToScreen(event.GetPosition()));
    }
    event.Skip();
}

void AgentFormCoordinator::OnWorkspaceLeftUp(wxMouseEvent& event)
{
    if (m_dragging) EndDrag(false);
    event.Skip();
}

void AgentFormCoordinator::OnWorkspaceCaptureLost(wxMouseCaptureLostEvent&)
{
    // Capture can be stolen by another window or a modal dialog: drop the drag
    // state instead of tracking pointer input we no longer receive.
    if (m_dragging) {
        m_dragging = false;
        if (m_captureWindow != nullptr && m_captureWindow->HasCapture()) {
            m_captureWindow->ReleaseMouse();
        }
        m_captureWindow = nullptr;
    }
}

void AgentFormCoordinator::OnWorkspaceCharHook(wxKeyEvent& event)
{
    // DEC-09: Esc belongs to the Agent only while its focus domain owns it. This
    // hook is reached solely from the workspace subtree, never from the editor,
    // canvas or a modal dialog.
    if (event.GetKeyCode() == WXK_ESCAPE && IsPresented() && m_form != AgentForm::Pet) {
        SetPresentation(AgentForm::Pet);
        return;
    }
    event.Skip();
}

void AgentFormCoordinator::SetFormObserver(std::function<void(AgentForm)> observer)
{
    m_observer = std::move(observer);
}

bool AgentFormCoordinator::SetPresentation(AgentForm form)
{
    if (m_destroying || m_page == nullptr) return false;
    if (form != AgentForm::None && m_workspace == nullptr) return false;
    EnsureHosts();
    if (form == m_form) {
        Relayout();
        return true;
    }

    AgentWorkspace::ViewState state;
    if (m_workspace != nullptr) state = m_workspace->CaptureViewState();
    EndDrag(true);

    switch (form) {
    case AgentForm::None:
        if (m_workspace != nullptr) {
            EnsureParent(m_workspace, m_page);
            m_page->SetAgentView(m_workspace);
            m_workspace->Hide();
        }
        m_page->ShowAgent(false);
        m_floatCard->Hide();
        m_petBubble->Hide();
        if (m_floatWindow) m_floatWindow->Hide();
        if (m_petWindow) m_petWindow->Hide();
        break;
    case AgentForm::Center:
    case AgentForm::Dock:
        if (m_workspace != nullptr) {
            EnsureParent(m_workspace, m_page);
            m_page->SetAgentView(m_workspace);
            m_page->ShowAgent(true);
            m_workspace->SetDocked(form == AgentForm::Dock);
        }
        m_floatCard->Hide();
        m_petBubble->Hide();
        if (m_floatWindow) m_floatWindow->Hide();
        if (m_petWindow) m_petWindow->Hide();
        break;
    case AgentForm::Float:
        m_page->ShowAgent(false);
        if (m_workspace != nullptr) {
            EnsureParent(m_workspace, m_floatCard);
            m_workspace->SetDocked(false);
            m_workspace->Show();
        }
        if (!m_detachedHosts || !m_hasFloatPlacement) m_floatRect = FloatBounds();
        m_hasFloatPlacement = true;
        if (m_floatWindow) m_floatWindow->SetSize(m_floatRect);
        LayoutFloatCard();
        m_floatCard->Show();
        m_floatCard->Raise();
        if (m_floatWindow) { m_floatWindow->Show(); m_floatWindow->Raise(); }
        m_petBubble->Hide();
        if (m_petWindow) m_petWindow->Hide();
        break;
    case AgentForm::Pet:
        m_page->ShowAgent(false);
        if (m_workspace != nullptr) {
            EnsureParent(m_workspace, m_floatCard);
            m_workspace->Hide();
        }
        m_floatCard->Hide();
        if (m_floatWindow) m_floatWindow->Hide();
        UpdatePetBubble();
        m_petBubble->Show();
        m_petBubble->Raise();
        if (m_petWindow) {
            m_petWindow->ShowWithoutActivating();
            m_petWindow->Raise();
            // ShowWithoutActivating can defer the first child paint until idle.
            // Paint the actual visible child now, without a reentrant wxYield.
            m_petBubble->Refresh(false);
            m_petBubble->Update();
            m_petWindow->Update();
        }
        break;
    }

    m_form = form;
    if (m_workspace != nullptr && form != AgentForm::None) {
        m_workspace->RestoreViewState(state);
    }
    if (m_form == AgentForm::Center || m_form == AgentForm::Dock) {
        if (m_detachedHosts) {
            if (auto* owner = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(m_page))) {
                if (owner->IsIconized()) owner->Iconize(false);
                owner->Show();
                owner->Raise();
            }
        }
        if (m_workspace != nullptr) m_workspace->FocusComposer();
    }
    if (m_observer) m_observer(m_form);
    return true;
}

wxSize AgentFormCoordinator::PageClientSize() const
{
    return m_page != nullptr ? m_page->GetClientSize() : wxSize(0, 0);
}

bool AgentFormCoordinator::FloatIsFullBleed() const
{
    const wxSize area = PageClientSize();
    const layout::Metrics metrics = tokens::LayoutMetrics();
    return layout::FloatFullBleed(tokens::DipToCssPx(m_page, area.x), metrics);
}

int AgentFormCoordinator::FloatClampPx() const
{
    const wxSize area = PageClientSize();
    const layout::Metrics metrics = tokens::LayoutMetrics();
    return tokens::CssPxToDip(
        m_page, layout::FloatClamp(tokens::DipToCssPx(m_page, area.x), metrics));
}

wxRect AgentFormCoordinator::FloatBounds() const
{
    if (m_detachedHosts) {
        const auto area = DesktopArea(m_floatWindow);
        const int inset = Dip(m_floatWindow, 16);
        const int width = std::min(Dip(m_floatWindow, 400), std::max(1, area.width - 2 * inset));
        const int height = std::min(Dip(m_floatWindow, 596), std::max(1, area.height - 2 * inset));
        return wxRect(area.GetRight() - width - inset + 1, area.y + inset, width, height);
    }
    const wxSize area = PageClientSize();
    if (area.x <= 0 || area.y <= 0) return wxRect(0, 0, 0, 0);
    // AGUI-T11: one shared DIP policy decides the breakpoint; pixel conversion
    // happens after the decision so a high-DPI window cannot pick the wrong tier.
    const layout::Metrics metrics = tokens::LayoutMetrics();
    const layout::Box box = layout::FloatBounds(tokens::DipToCssPx(m_page, area.x),
                                                tokens::DipToCssPx(m_page, area.y), metrics);
    if (box.IsEmpty()) return wxRect(0, 0, 0, 0);
    wxRect rect(tokens::CssPxToDip(m_page, box.x), tokens::CssPxToDip(m_page, box.y),
                tokens::CssPxToDip(m_page, box.width), tokens::CssPxToDip(m_page, box.height));
    rect.width = std::min(rect.width, area.x);
    rect.height = std::min(rect.height, area.y);
    rect.x = std::min(std::max(0, rect.x), std::max(0, area.x - rect.width));
    rect.y = std::min(std::max(0, rect.y), std::max(0, area.y - rect.height));
    return rect;
}

wxRect AgentFormCoordinator::PetBounds() const
{
    if (m_detachedHosts) {
        const auto area = DesktopArea(m_petWindow);
        const int inset = Dip(m_petWindow, 16);
        const int width = std::min(Dip(m_petWindow, 240), std::max(1, area.width - 2 * inset));
        const int height = std::min(Dip(m_petWindow, 240), std::max(1, area.height - 2 * inset));
        return wxRect(area.GetRight() - width - inset + 1,
                      area.GetBottom() - height - inset + 1, width, height);
    }
    const wxSize area = PageClientSize();
    if (area.x <= 0 || area.y <= 0) return wxRect(0, 0, 0, 0);
    const layout::Metrics metrics = tokens::LayoutMetrics();
    const layout::Box box = layout::PetBounds(tokens::DipToCssPx(m_page, area.x),
                                              tokens::DipToCssPx(m_page, area.y), metrics);
    if (box.IsEmpty()) return wxRect(0, 0, 0, 0);
    wxRect rect(tokens::CssPxToDip(m_page, box.x), tokens::CssPxToDip(m_page, box.y),
                tokens::CssPxToDip(m_page, box.width), tokens::CssPxToDip(m_page, box.height));
    rect.width = std::min(rect.width, area.x);
    rect.height = std::min(rect.height, area.y);
    rect.x = std::min(std::max(0, rect.x), std::max(0, area.x - rect.width));
    rect.y = std::min(std::max(0, rect.y), std::max(0, area.y - rect.height));
    return rect;
}

void AgentFormCoordinator::UpdatePetBubble()
{
    if (m_petBubble == nullptr) return;
    if (m_detachedHosts) {
        if (!m_hasPetPlacement) { m_petWindow->SetSize(PetBounds()); m_hasPetPlacement = true; }
        m_petBubble->SetSize(m_petWindow->GetClientRect());
        m_petBubble->Refresh(false);
        return;
    }
    const wxSize area = PageClientSize();
    const layout::Metrics metrics = tokens::LayoutMetrics();
    const int dipWidth = tokens::DipToCssPx(m_page, area.x);
    if (auto* bubble = dynamic_cast<PetStandbyBubble*>(m_petBubble)) {
        bubble->SetNarrow(dipWidth > 0 && dipWidth <= metrics.narrowMax);
    }
    m_petBubble->SetSize(PetBounds());
    m_petBubble->Raise();
}

void AgentFormCoordinator::OnPageDpiChanged(wxDPIChangedEvent& event)
{
    // A window may move to a monitor with a different scale while a float/pet form
    // is presented: recompute sizes from the policy in DIP, then re-clamp.
    Relayout();
    event.Skip();
}

void AgentFormCoordinator::Relayout()
{
    if (m_destroying || m_page == nullptr) return;
    if (m_detachedHosts) {
        if (m_form == AgentForm::Float && m_floatWindow) {
            const auto area = DesktopArea(m_floatWindow);
            wxRect rect = m_floatWindow->GetRect();
            const int inset = std::min(Dip(m_floatWindow, 8), std::min(area.width, area.height) / 2);
            rect.width = std::min(rect.width, std::max(1, area.width - 2 * inset));
            rect.height = std::min(rect.height, std::max(1, area.height - 2 * inset));
            rect.x = std::clamp(rect.x, area.x + inset, std::max(area.x + inset, area.GetRight() + 1 - inset - rect.width));
            rect.y = std::clamp(rect.y, area.y + inset, std::max(area.y + inset, area.GetBottom() + 1 - inset - rect.height));
            if (rect != m_floatWindow->GetRect()) m_floatWindow->SetSize(rect);
            m_floatRect = rect;
            LayoutFloatCard();
        } else if (m_form == AgentForm::Pet) UpdatePetBubble();
        return;
    }
    const wxSize page = PageClientSize();
    if (m_form == AgentForm::Float) {
        // T08 regression: a degenerate page must clear the stale geometry instead of
        // keeping a card that was sized for the previous window.
        if (page.x <= 1 || page.y <= 1) {
            m_floatRect = wxRect(0, 0, 0, 0);
            LayoutFloatCard();
            return;
        }
        const wxRect area = FloatBounds();
        if (area.width <= 0 || area.height <= 0) {
            m_floatRect = area;
            LayoutFloatCard();
            return;
        }
        const bool fullBleed = FloatIsFullBleed();
        const int clamp = FloatClampPx();
        if (m_floatRect.width <= 0 || m_floatRect.height <= 0) m_floatRect = area;
        // The size always comes from the current breakpoint (so crossing 960/560 or
        // a DPI change restores it), while a still-valid dragged position is kept.
        m_floatRect.width = area.width;
        m_floatRect.height = area.height;
        if (fullBleed) {
            // ≤560: the card fills the page and the 8 DIP clamp is lifted.
            m_floatRect = area;
        } else {
            m_floatRect.x = std::min(std::max(m_floatRect.x, clamp),
                                     std::max(clamp, page.x - clamp - m_floatRect.width));
            m_floatRect.y = std::min(std::max(m_floatRect.y, clamp),
                                     std::max(clamp, page.y - clamp - m_floatRect.height));
        }
        LayoutFloatCard();
        m_floatCard->Raise();
    } else if (m_form == AgentForm::Pet) {
        UpdatePetBubble();
    }
}

void AgentFormCoordinator::LayoutFloatCard()
{
    if (m_floatCard == nullptr) return;
    m_floatCard->SetSize(m_floatWindow ? m_floatWindow->GetClientRect() : m_floatRect);
    if (m_workspace != nullptr && m_workspace->GetParent() == m_floatCard) {
        const wxSize client = m_floatCard->GetClientSize();
        m_workspace->SetSize(0, 0, std::max(0, client.x), std::max(0, client.y));
    }
}

void AgentFormCoordinator::EnsureParent(wxWindow* window, wxWindow* parent)
{
    if (window == nullptr || parent == nullptr || window->GetParent() == parent) return;
    if (auto* sizer = window->GetContainingSizer()) sizer->Detach(window);
    window->Reparent(parent);
}

bool AgentFormCoordinator::WorkspaceLivesInFloatCard() const
{
    return m_workspace != nullptr && m_floatCard != nullptr &&
           m_workspace->GetParent() == m_floatCard;
}

unsigned AgentFormCoordinator::PetPaintCount() const
{
    const auto* bubble = dynamic_cast<PetStandbyBubble*>(m_petBubble);
    return bubble ? bubble->PaintCount() : 0;
}

bool AgentFormCoordinator::BeginDrag(const wxPoint& screenPoint)
{
    if (m_form != AgentForm::Float || m_floatCard == nullptr || !m_floatCard->IsShown()) {
        return false;
    }
    m_dragging = true;
    m_dragStartScreen = screenPoint;
    m_dragStartCard = m_floatRect.GetPosition();
    return true;
}

void AgentFormCoordinator::UpdateDrag(const wxPoint& screenPoint)
{
    if (!m_dragging || m_page == nullptr) return;
    if (m_detachedHosts) {
        const auto area = DesktopArea(m_floatWindow);
        const int inset = Dip(m_floatWindow, 8);
        const auto point = m_dragStartCard + screenPoint - m_dragStartScreen;
        m_floatRect.x = std::clamp(point.x, area.x + inset,
            std::max(area.x + inset, area.GetRight() + 1 - inset - m_floatRect.width));
        m_floatRect.y = std::clamp(point.y, area.y + inset,
            std::max(area.y + inset, area.GetBottom() + 1 - inset - m_floatRect.height));
        m_floatWindow->Move(m_floatRect.GetPosition());
        LayoutFloatCard();
        return;
    }
    const wxSize area = PageClientSize();
    // AGUI-T11: the clamp follows the same breakpoint policy; in the ≤560 full-bleed
    // form it is 0, so the card stays exactly on the page instead of being inset.
    const int clamp = FloatClampPx();
    const int x = m_dragStartCard.x + (screenPoint.x - m_dragStartScreen.x);
    const int y = m_dragStartCard.y + (screenPoint.y - m_dragStartScreen.y);
    const int maxX = std::max(clamp, area.x - m_floatRect.width - clamp);
    const int maxY = std::max(clamp, area.y - m_floatRect.height - clamp);
    m_floatRect.x = std::min(std::max(x, clamp), maxX);
    m_floatRect.y = std::min(std::max(y, clamp), maxY);
    LayoutFloatCard();
}

void AgentFormCoordinator::EndDrag(bool cancelled)
{
    (void)cancelled;  // the prototype keeps the last valid position either way
    if (!m_dragging) return;
    m_dragging = false;
    if (m_captureWindow != nullptr && m_captureWindow->HasCapture()) {
        m_captureWindow->ReleaseMouse();
    }
    m_captureWindow = nullptr;
}

wxRect AgentFormCoordinator::DesktopArea(wxWindow* window) const
{
    int display = window && window->IsShownOnScreen() ? wxDisplay::GetFromWindow(window) : wxNOT_FOUND;
    if (display == wxNOT_FOUND) display = wxDisplay::GetFromWindow(m_page);
    if (display != wxNOT_FOUND) return wxDisplay(static_cast<unsigned>(display)).GetClientArea();
    return wxGetClientDisplayRect();
}

void AgentFormCoordinator::OnFloatSize(wxSizeEvent& event)
{
    if (!m_destroying && m_floatWindow) {
        if (m_hasFloatPlacement) m_floatRect = m_floatWindow->GetRect();
        LayoutFloatCard();
    }
    event.Skip();
}

void AgentFormCoordinator::OnFloatMove(wxMoveEvent& event)
{
    if (!m_destroying && m_floatWindow && m_form == AgentForm::Float) m_floatRect = m_floatWindow->GetRect();
    event.Skip();
}

void AgentFormCoordinator::OnFloatClose(wxCloseEvent& event)
{
    if (m_destroying) { event.Skip(); return; }
    if (event.CanVeto()) {
        event.Veto();
        SetPresentation(AgentForm::Pet); // Close the host, never discard the conversation.
    } else {
        SetPresentation(AgentForm::None); // Park children before a forced host destruction.
        event.Skip();
    }
}

void AgentFormCoordinator::OnPetClose(wxCloseEvent& event)
{
    if (m_destroying) { event.Skip(); return; }
    SetPresentation(AgentForm::None);
    if (event.CanVeto()) event.Veto();
    else event.Skip();
}

void AgentFormCoordinator::OnHostDestroyed(wxWindowDestroyEvent& event)
{
    if (!m_destroying) {
        if (event.GetEventObject() == m_floatWindow) {
            m_floatWindow = nullptr; m_floatCard = nullptr; m_hasFloatPlacement = false;
        }
        if (event.GetEventObject() == m_petWindow) { m_petWindow = nullptr; m_petBubble = nullptr; m_hasPetPlacement = false; }
    }
    event.Skip();
}

}  // namespace agent::gui
