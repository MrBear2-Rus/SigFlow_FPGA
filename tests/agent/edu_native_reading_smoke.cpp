#include "agent/gui/AgentControls.h"
#include "agent/gui/AgentWorkspace.h"

#include <wx/app.h>
#include <wx/debug.h>
#include <wx/dcmemory.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/msw/wrapwin.h>
#ifdef __WXMSW__
#include <wx/msw/private.h>
#endif

#include <iostream>

namespace {
int failures = 0;

void Check(bool ok, const char* message)
{
    if (!ok) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void Pump()
{
    for (int i = 0; i < 4; ++i) {
        wxTheApp->ProcessPendingEvents();
        wxTheApp->ProcessIdle();
    }
}

void Wheel(wxWindow* window, int rotation)
{
    wxMouseEvent wheel(wxEVT_MOUSEWHEEL);
    wheel.SetEventObject(window);
    wheel.SetPosition(wxPoint(4, 4));
    wheel.m_wheelRotation = rotation;
    wheel.m_wheelDelta = 120;
    wheel.m_linesPerAction = 3;
    window->GetEventHandler()->ProcessEvent(wheel);
    Pump();
}

void Key(wxWindow* window, int code)
{
    wxKeyEvent key(wxEVT_CHAR_HOOK);
    key.m_keyCode = code;
    key.SetEventObject(window);
    window->GetEventHandler()->ProcessEvent(key);
    Pump();
}

void ScrollThumb(wxScrolledWindow& window, int position)
{
    // Windows stores the thumb track position internally while dragging.
    // Exercise the wx event emitted by that path with an explicit position.
    wxScrollWinEvent drag(wxEVT_SCROLLWIN_THUMBTRACK, position, wxVERTICAL);
    drag.SetEventObject(&window);
    window.GetEventHandler()->ProcessEvent(drag);
    Pump();
}

void CheckEmptyConversationPaint(agent::gui::AgentConversation& thread)
{
    const auto size = thread.GetClientSize();
    Check(thread.IsEmptyState() && size.x > 0 && size.y > 0, "empty conversation has a viewport");
    if (size.x <= 0 || size.y <= 0) return;
    wxBitmap bitmap(size.x, size.y);
    wxMemoryDC surface(bitmap);
    surface.SetBackground(*wxRED_BRUSH);
    surface.Clear();
    // Dispatch the native background paint into an offscreen DC, so the
    // hidden fixture can verify the viewport without taking desktop focus.
    ::SendMessageW(thread.GetHWND(), WM_ERASEBKGND,
                   reinterpret_cast<WPARAM>(GetHdcOf(surface)), 0);
    wxColour bottom;
    const bool measured = surface.GetPixel(size.x / 2, size.y - 1, &bottom);
    const auto expected = thread.GetBackgroundColour();
    if (measured && bottom != expected)
        std::cerr << "painted=" << bottom.GetAsString().ToStdString()
                  << " expected=" << expected.GetAsString().ToStdString() << '\n';
    Check(measured && bottom == expected,
          "the first empty conversation paint fills the viewport with its theme background");
}

class ReadingApp final : public wxApp
{
public:
    bool OnInit() override { return true; }
};
} // namespace

wxIMPLEMENT_APP_NO_MAIN(ReadingApp);

int main(int argc, char** argv)
{
    wxSetAssertHandler([](const wxString& file, int line, const wxString&,
                          const wxString& condition, const wxString& detail) {
        std::cerr << file.ToStdString() << ':' << line << ": "
                  << condition.ToStdString() << ' ' << detail.ToStdString() << '\n';
        ++failures;
    });
#ifdef __WXMSW__
    wxSetInstance(GetModuleHandleW(nullptr));
#endif
    if (!wxEntryStart(argc, argv) || !wxTheApp->CallOnInit()) return 2;
    auto* frame = new wxFrame(nullptr, wxID_ANY, "Edu Agent reading regression",
                              wxDefaultPosition, wxSize(630, 565));
    auto* workspace = new agent::gui::AgentWorkspace(frame, [] {});
    auto* page = dynamic_cast<wxScrolledWindow*>(
        wxWindow::FindWindowByName("agent-workspace-scroll", workspace));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(workspace, 1, wxEXPAND);
    frame->SetSizer(sizer);
    frame->Layout();
    Pump();
    CheckEmptyConversationPaint(*workspace->Conversation());

    using agent::gui::nav::Section;
    auto* accordion = workspace->SupportAccordion();
    auto* support = dynamic_cast<wxScrolledWindow*>(accordion->GetParent());
    Check(support != nullptr, "teaching support has a native scrollbar");
    Check(page != nullptr, "workspace page has a native scrollbar");
    const int index = workspace->SupportSectionIndex(Section::Record);
    auto* header = accordion->SectionHeaderWindow(index);
    Check(header != nullptr, "learning records header exists");
    if (support && header) {
        const int collapsedHeight = support->GetVirtualSize().y;
        wxKeyEvent expand(wxEVT_KEY_DOWN);
        expand.m_keyCode = WXK_SPACE;
        expand.SetEventObject(header);
        header->GetEventHandler()->ProcessEvent(expand);
        Pump();
        Check(accordion->IsExpanded(index), "keyboard activation expands the records");
        Check(support->GetVirtualSize().y > collapsedHeight, "expansion updates the scroll extent");
#ifdef __WXMSW__
        ::SendMessageW(support->GetHWND(), WM_VSCROLL, SB_BOTTOM, 0);
#else
        support->Scroll(0, 100000);
#endif
        const wxPoint end = support->CalcUnscrolledPosition(wxPoint(0, support->GetClientSize().y));
        const int bodyEnd = accordion->GetPosition().y + accordion->BodyRectPx(index).GetBottom();
        Check(bodyEnd < support->GetClientSize().y && end.y >= support->GetVirtualSize().y,
              "bottom scroll position reaches the learning records");
        if (auto* body = accordion->SectionContent(index); body && !body->GetChildren().IsEmpty()) {
            auto* record = dynamic_cast<wxTextCtrl*>(body->GetChildren().GetFirst()->GetData());
            Check(record && !record->IsEditable() && !record->GetValue().empty(),
                  "learning records are selectable in their existing panel");
        }
        ScrollThumb(*support, 0);
        Check(support->GetViewStart().y == 0, "native scrollbar returns to its first position");
        wxKeyEvent release(wxEVT_KEY_UP);
        release.m_keyCode = WXK_SPACE;
        release.SetEventObject(header);
        header->GetEventHandler()->ProcessEvent(release);
        wxKeyEvent collapse(wxEVT_KEY_DOWN);
        collapse.m_keyCode = WXK_SPACE;
        collapse.SetEventObject(header);
        header->GetEventHandler()->ProcessEvent(collapse);
        Pump();
        Check(!accordion->IsExpanded(index) && support->GetVirtualSize().y == collapsedHeight,
              "collapsing the records restores the shorter scroll extent");
        if (page) {
            page->Scroll(0, 0);
            workspace->RevealSection(Section::Checker);
            Pump();
            const auto first = page->GetViewStart();
            workspace->RevealSection(Section::Checker);
            Pump();
            const auto* target = accordion->SectionHeaderWindow(workspace->SupportSectionIndex(Section::Checker));
            const wxPoint top = page->ScreenToClient(target->ClientToScreen(wxPoint()));
            Check(first == page->GetViewStart() && top.y >= 0 &&
                  top.y + target->GetSize().y <= page->GetClientSize().y,
                  "repeated sidebar navigation keeps the target header visible");
        }
    }

    auto* thread = workspace->Conversation();
    agent::gui::ConversationContent content;
    for (int i = 0; i < 30; ++i) {
        agent::gui::vm::MessageView message;
        message.messageId = "message-" + std::to_string(i);
        message.role = "assistant";
        message.text = "A selectable conversation message with several lines.\nSecond line.";
        content.messages.push_back(message);
    }
    thread->SetContent(content);
    thread->Scroll(0, 0);
    Pump();
    wxTextCtrl* text = nullptr;
    if (auto* row = thread->RowWindow(0)) {
        for (auto* child : row->GetChildren()) {
            if ((text = dynamic_cast<wxTextCtrl*>(child))) break;
        }
    }
    Check(text != nullptr, "message has a selectable native text control");
    if (text) {
        Wheel(text, -60);
        Check(thread->GetViewStart().y == 0, "a partial wheel detent is accumulated");
        Wheel(text, -60);
        Check(thread->GetViewStart().y > 0, "wheel over native message text scrolls the conversation");
        thread->Scroll(0, 0);
        const wxPoint point = text->ClientToScreen(wxPoint(4, 4));
        ::SendMessageW(text->GetHWND(), WM_MOUSEWHEEL,
                       MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), MAKELPARAM(point.x, point.y));
        Pump();
        Check(thread->GetViewStart().y > 0, "the native text window routes WM_MOUSEWHEEL to the conversation");
        Key(text, WXK_HOME);
        Check(thread->GetViewStart().y == 0, "Home scrolls the focused read-only conversation to its start");
        Key(text, WXK_PAGEDOWN);
        Check(thread->GetViewStart().y > 0, "Page Down scrolls the focused conversation");
        const auto anchor = thread->TopAnchorId();
        auto appended = content;
        appended.messages.push_back(content.messages.back());
        appended.messages.back().messageId = "last-message";
        thread->SetContent(appended);
        Check(thread->TopAnchorId() == anchor, "new messages preserve the current reading anchor");
        if (page) {
            thread->ScrollToBottom();
            page->Scroll(0, 0);
            const auto before = thread->GetViewStart();
            if (auto* last = thread->RowWindow(thread->RowCount() - 1)) {
                Wheel(last, -120);
                Check(thread->GetViewStart() == before && page->GetViewStart().y > 0,
                      "a wheel detent at the end scrolls only the outer page");
            }
        }
    }

    frame->SetClientSize(workspace->FromDIP(wxSize(375, 320)));
    frame->Layout();
    Pump();
    workspace->RevealSection(Section::Guide);
    auto* guide = dynamic_cast<wxScrolledWindow*>(workspace->GuideScrollWindow());
    Check(guide && guide->GetClientSize().y > 0 &&
          guide->GetVirtualSize().y > guide->GetClientSize().y,
          "learning steps remain scrollable at a narrow, short window size");
    if (support) {
        agent::gui::support::Projection projection;
        std::string longText;
        for (int i = 0; i < 30; ++i) longText += "这是一条教学证据，包含较长的说明和上下文。\n";
        longText += "https://example.test/" + std::string(150, 'w') + "\nLAST-EVIDENCE-LINE";
        projection.evidence.push_back({"证据", longText});
        auto* evidence = workspace->EvidenceSection();
        evidence->SetProjection(projection);
        workspace->RevealSection(Section::Evidence);
        Pump();
        const int limit = support->GetScrollRange(wxVERTICAL) - support->GetScrollThumb(wxVERTICAL);
        ScrollThumb(*support, limit);
        wxTextCtrl* bodyText = nullptr;
        for (auto* child : evidence->GetChildren()) {
            auto* field = dynamic_cast<wxTextCtrl*>(child);
            if (field && field->GetValue().EndsWith("LAST-EVIDENCE-LINE")) bodyText = field;
        }
        const int bottom = support->ScreenToClient(
            evidence->ClientToScreen(wxPoint(0, evidence->GetClientSize().y))).y;
        const bool reachesEnd = limit > 0 && support->GetViewStart().y == limit &&
            bottom <= support->GetClientSize().y && bodyText &&
            bodyText->GetSize().y >= bodyText->GetNumberOfLines() * bodyText->GetCharHeight();
        if (!reachesEnd) {
            std::cerr << "scroll=" << support->GetViewStart().y << '/' << limit
                      << " evidence bottom=" << bottom << " viewport=" << support->GetClientSize().y
                      << " lines=" << (bodyText ? bodyText->GetNumberOfLines() : 0)
                      << " text height=" << (bodyText ? bodyText->GetSize().y : 0) << '\n';
        }
        Check(reachesEnd,
              "native thumb reaches the long CJK evidence with its final line laid out");
    }
    if (page) {
        auto* draft = workspace->DraftControl();
        page->Scroll(0, 0);
        Key(draft, WXK_END);
        Check(page->GetViewStart().y == 0, "End in the composer remains an editing key");
    }
    std::cout << "Native reading checks at " << workspace->GetDPI().x << " DPI" << '\n';

    frame->Destroy();
    Pump();
    wxTheApp->OnExit();
    wxEntryCleanup();
    return failures ? 1 : 0;
}
