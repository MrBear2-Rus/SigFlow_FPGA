#include "agent/gui/AgentWorkspace.h"

#include "agent/gui/AgentControls.h"
#include "agent/gui/AgentLayoutPolicy.h"
#include "agent/gui/AgentNavigation.h"
#include "agent/gui/AgentTokens.h"

#include <wx/dcclient.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <utility>

namespace agent::gui {
namespace {
namespace S = UiTheme::AgentSpacing;
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

wxString Text(const std::string& utf8)
{
    return wxString::FromUTF8(utf8.data(), utf8.size());
}

const char* GuideNoteText()
{
    return "完成态以服务端教学记录与当前版本证据为准。";
}

// AGUI-T12: the wrap helper moved to the token layer so the conversation
// component and the workspace wrap explanatory text with exactly the same
// metrics. Keeping one implementation avoids the stored label height drifting
// from the number of lines actually drawn.
using WrappedText = tokens::WrappedText;

WrappedText WrapForWidth(wxWindow* owner, const wxFont& font, const wxString& source,
                         int width)
{
    return tokens::WrapTextForWidth(owner, font, source, width);
}

// Reset the unwrapped source before wrapping again: otherwise widening keeps old
// inserted newlines. Explicit geometry prevents native best widths forcing the
// viewport wider than its available client area.
int PlaceText(wxStaticText* label, const wxString& source, int x, int y, int width)
{
    const auto wrapped = WrapForWidth(label, label->GetFont(), source, width);
    label->SetLabel(wrapped.text);
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(label->GetParent())) {
        scroll->CalcScrolledPosition(x, y, &x, &y);
    }
    label->SetSize(x, y, std::max(0, width), wrapped.height);
    return wrapped.height;
}

wxStaticText* Label(wxWindow* parent, const char* name)
{
    auto* result = new wxStaticText(parent, wxID_ANY, {}, wxDefaultPosition,
                                    wxDefaultSize, wxST_NO_AUTORESIZE);
    result->SetName(Text(name));
    result->SetMinSize(wxSize(0, 0));
    return result;
}

// AGUI-T09: accordion bodies are measured by the host at the current width; the
// deterministic best size keeps CJK text from re-wrapping behind our back.
class SectionBody final : public wxPanel
{
public:
    explicit SectionBody(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
    }

    wxSize DoGetBestSize() const override { return GetMinSize(); }
};

} // namespace

AgentWorkspace::AgentWorkspace(wxWindow* parent, std::function<void()> onReturn)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxBORDER_NONE | wxTAB_TRAVERSAL),
      m_onReturn(std::move(onReturn))
{
    SetName("agent-workspace");
    SetMinSize(wxSize(0, 0));
    // Host callbacks (return / submit adapter) may destroy this workspace on the
    // same stack frame; the token lets the caller detect that before touching us.
    m_alive = std::make_shared<bool>(true);
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize, wxBORDER_NONE | wxVSCROLL);
    m_scroll->SetName("agent-workspace-scroll");
    m_scroll->SetScrollRate(0, Dip(this, S::ThreadGap));
    m_scroll->SetMinSize(wxSize(0, 0));
    m_sheet = new wxPanel(m_scroll, wxID_ANY, wxDefaultPosition,
                          wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL);
    m_sheet->SetName("agent-workspace-sheet");

    // ---- AGUI-T09: in-page top bar (brand/status/mode/levels) ----
    m_topBar = new nav::AgentTopBar(m_sheet);
    m_topBar->SetIntentHandlers(
        [this](vm::AppMode mode) { m_model.SetMode(mode); },
        [this](int level) { m_model.SetLevel(nav::LevelForNumber(level)); });

    // Page-level action kept from T07: leave the Agent page, keep every EDA
    // feature and this page's draft/history.
    m_return = new controls::AgentButton(m_sheet, wxID_ANY, Text("返回工作台"),
                                         controls::Variant::Ghost);
    m_return->SetAccessibleName(Text("返回原工作台"));
    m_return->SetToolTip(Text("返回画布与编辑器，保留此处草稿"));
    m_return->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // The callback may delete this workspace. Do not access this afterwards.
        const auto callback = m_onReturn;
        if (callback) callback();
    });
    m_report = new controls::AgentButton(m_sheet, wxID_ANY, Text("选择报告"), controls::Variant::Ghost);
    m_report->Enable(false);
    m_report->SetToolTip(Text("选择当前工程当前版本的终态 Job 报告作为教学证据"));
    m_report->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const auto handler = m_reportSelection;
        if (handler) handler();
    });

    // ---- functional rail: newAsk, guide toggle, six jumps, four forms ----
    m_rail = new nav::AgentRail(m_sheet);
    m_rail->SetActionHandler([this](nav::Action action) { Trigger(action); });

    // ---- guide column: six-step path (scrolls inside the card) + footer note ----
    m_guide = new controls::AgentCardHost(m_sheet, wxID_ANY, Text("学习路径"));
    m_guide->SetName("agent-workspace-guide");
    m_guideBody = new wxPanel(m_guide, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxBORDER_NONE);
    m_guideBody->SetName("agent-guide-body");
    m_guideScroll = new wxScrolledWindow(m_guideBody, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxBORDER_NONE | wxVSCROLL);
    m_guideScroll->SetName("agent-guide-scroll");
    m_guideScroll->SetScrollRate(0, Dip(this, S::ThreadGap));
    m_path = new path::AgentLearningPath(m_guideScroll);
    // A navigation click only changes the local note/selection; re-arrange so a
    // wrapped note cannot leave a stale virtual height behind.
    m_path->SetNavigationHandler([this](path::StepId) {
        if (!m_destroying && !m_arranging) Arrange();
    });
    m_guideNote = Label(m_guideBody, "学习路径尾注");
    m_guide->SetContent(m_guideBody);

    // ---- conversation card: header, status strip, thread, composer ----
    m_conversation = new controls::AgentCardHost(m_sheet, wxID_ANY);
    m_conversation->SetName("agent-workspace-conversation");
    m_conversationBody = new wxPanel(m_conversation, wxID_ANY,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxBORDER_NONE | wxTAB_TRAVERSAL);
    m_conversation->SetContent(m_conversationBody);
    m_head = new nav::AgentConversationHeader(m_conversationBody);
    m_head->SetActionHandler([this](nav::Action action) { Trigger(action); });
    m_statusStrip = new nav::AgentStatusStrip(m_conversationBody);
    // AGUI-T12: the thread area is the real projection component. It owns the
    // empty state, the message/card rows and their scroll contract; the workspace
    // only feeds it a snapshot of the model.
    m_thread = new AgentConversation(m_conversationBody);
    m_thread->SetName(Text("对话记录"));
    m_composer = new controls::AgentCardHost(m_conversationBody, wxID_ANY,
                                             Text("提问草稿"));
    m_composer->SetName("agent-workspace-composer");
    // AGUI-T13: the composer card hosts the real .aw-compose block (40 DIP input +
    // gradient send + local feedback). The workspace never sends by itself: every
    // entry (mouse, Enter, chips, future pet) calls SubmitQuestion() below.
    m_composerView = new AgentComposer(m_composer);
    m_composerView->SetName("agent-workspace-composer-view");
    m_draft = m_composerView->DraftControl();
    m_composer->SetContent(m_composerView);
    m_composerView->SetDraftEditedCallback([this](const wxString& value) {
        if (m_destroying || m_syncingDraft) return;
        const wxScopedCharBuffer utf8 = value.utf8_str();
        m_model.SetDraft(std::string(utf8.data(), utf8.length()));
    });
    m_composerView->SetSubmitCallback([this](const wxString& text) {
        return SubmitQuestion(text);
    });

    // ---- support card: six honest placeholder sections (T15-T19 fill them) ----
    m_support = new controls::AgentCardHost(m_sheet, wxID_ANY, Text("教学辅助"));
    m_support->SetName("agent-workspace-support");
    m_supportBody = new wxScrolledWindow(m_support, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxBORDER_NONE | wxVSCROLL);
    m_supportBody->SetScrollRate(0, Dip(this, S::ThreadGap));
    m_supportBody->SetName(Text("教学辅助内容"));
    m_supportSections = new controls::AgentAccordion(m_supportBody, wxID_ANY);
    m_supportSections->SetName("agent-support-sections");
    m_support->SetContent(m_supportBody);
    EnsureSupportSectionBuilt();

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_scroll, 1, wxEXPAND);
    SetSizer(root);

    // AGUI-T13: chips are trusted follow-up text; they go through the same single
    // submit entry instead of a second send path.
    m_thread->SetSuggestionHandler([this](const wxString& text) {
        if (m_destroying) return;
        const auto utf8 = text.utf8_str();
        m_model.SetDraft(std::string(utf8.data(), utf8.length()));
        FocusComposer();
        PushSubmitFeedback(Text("已填入问题，按回车发送。"));
    });
    m_modelSubscription = m_model.Subscribe([this] {
        SyncDraft();
        SyncNavigation();
    });
    m_scroll->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Arrange();
        event.Skip();
    });
    m_conversationBody->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        if (!m_arranging) ArrangeConversation();
        event.Skip();
    });
    m_supportBody->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        if (!m_arranging) ArrangeSupport();
        event.Skip();
    });
    m_guideBody->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        if (!m_arranging && m_guide != nullptr && m_guide->IsShown()) {
            const wxRect rect = m_guide->GetRect();
            ArrangeGuide(rect.x, rect.y, rect.width, rect.height);
        }
        event.Skip();
    });
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        ApplyTheme();
        event.Skip();
    });
    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
    ApplyTheme();
    SyncNavigation();
}

AgentWorkspace::~AgentWorkspace()
{
    m_destroying = true;
    if (m_alive) *m_alive = false;
    m_modelSubscription.Reset();
    m_theme.Reset();
    m_submitHandler = nullptr;
    m_submit.Abandon();
}

void AgentWorkspace::EnsureSupportSectionBuilt()
{
    if (m_sectionsBuilt || m_supportSections == nullptr) return;
    m_sectionsBuilt = true;
    for (auto section : nav::SectionCatalog()) {
        if (section == nav::Section::Guide) continue;
        if (section == nav::Section::Issue || section == nav::Section::Evidence) {
            auto* body = new support::AgentSupportSection(m_supportSections, section == nav::Section::Evidence);
            if (section == nav::Section::Issue) m_issueSection = body;
            else m_evidenceSection = body;
            m_sectionBody.push_back(nullptr); // This section has structured read-only content.
            m_supportSections->AddSection(Text(nav::SectionTitle(section)), body, section == nav::Section::Issue);
            continue;
        }
        auto* body = new SectionBody(m_supportSections);
        body->SetName(Text("教学辅助分区正文"));
        auto* label = Label(body, "分区正文");
        m_sectionBody.push_back(label);
        const int index = m_supportSections->AddSection(
            Text(nav::SectionTitle(section)), body, section == nav::Section::Issue);
        if (index < 0) continue;
        m_sectionBody.back()->SetToolTip(Text("此分区尚未接入真实教学结果"));
    }
    // 惰性构建后立刻对齐标题摘要（计划分区的标题来自只读计划投影）。
    SyncSupport();
    m_supportSections->Relayout();
}

wxString AgentWorkspace::SupportSectionBody(nav::Section section) const
{
    if (section == nav::Section::Issue && m_issueSection) return m_issueSection->TextContent();
    if (section == nav::Section::Evidence && m_evidenceSection) return m_evidenceSection->TextContent();
    const int index = SupportSectionIndex(section);
    if (index < 0 || index >= static_cast<int>(m_sectionBody.size()) ||
        m_sectionBody[static_cast<size_t>(index)] == nullptr) {
        return {};
    }
    const auto* label = m_sectionBody[static_cast<size_t>(index)];
    return label->GetLabel();
}

int AgentWorkspace::SupportSectionIndex(nav::Section section) const
{
    if (section == nav::Section::Guide || section == nav::Section::Count) return -1;
    // Section order in the accordion is SectionCatalog() order without Guide.
    int index = 0;
    for (auto candidate : nav::SectionCatalog()) {
        if (candidate == nav::Section::Guide) continue;
        if (candidate == section) return index;
        ++index;
    }
    return -1;
}

bool AgentWorkspace::IsSupportSectionExpanded(nav::Section section) const
{
    const int index = SupportSectionIndex(section);
    return index >= 0 && m_supportSections != nullptr &&
           m_supportSections->IsExpanded(index);
}

bool AgentWorkspace::IsGuideVisible() const
{
    return m_guide != nullptr && m_guide->IsShown();
}

int AgentWorkspace::LevelSelection() const
{
    return nav::LevelNumber(m_model.CurrentLevel());
}

wxWindow* AgentWorkspace::GuidePanel() const
{
    return m_guide;
}

wxWindow* AgentWorkspace::GuideScrollWindow() const
{
    return m_guideScroll;
}

wxString AgentWorkspace::StatusLineText() const
{
    return m_statusStrip != nullptr ? m_statusStrip->Text() : wxString();
}

void AgentWorkspace::SetFormRequestHandler(std::function<bool(AgentForm)> handler)
{
    m_formRequest = std::move(handler);
}

void AgentWorkspace::SetFormState(AgentForm form)
{
    if (m_form == form) return;
    m_form = form;
    if (m_rail != nullptr) m_rail->SetFormPressed(m_form);
    if (m_head != nullptr) m_head->SetFormSelection(m_form);
    Refresh(false);
}

bool AgentWorkspace::NewAsk()
{
    // index.html newAsk: back to the center form, clear the composer, keep history.
    // It is not a session delete and sends no request. A submission that is still in
    // flight keeps its own snapshot: when its receipt arrives the draft is already
    // empty/different, so it can never clear a newer draft (ComposerState rule).
    m_model.SetDraft(std::string());
    if (m_composerView != nullptr) m_composerView->SetFeedback(wxString());
    bool accepted = true;
    if (m_form != AgentForm::Center && m_formRequest) {
        accepted = m_formRequest(AgentForm::Center);
    }
    FocusComposer();
    return accepted;
}

bool AgentWorkspace::JumpToSection(nav::Section section)
{
    return RevealSection(section);
}

bool AgentWorkspace::Trigger(nav::Action action)
{
    if (m_destroying || action == nav::Action::Count) return false;
    switch (action) {
    case nav::Action::NewAsk:
        return NewAsk();
    case nav::Action::ToggleGuide: {
        // index.html setMode(document.body.classList.contains('work')): the rail
        // toggle and the top bar segments are two views of one model value.
        const bool study = m_model.Mode() == vm::AppMode::Learning;
        m_model.SetMode(study ? vm::AppMode::Working : vm::AppMode::Learning);
        return true;
    }
    case nav::Action::JumpIssue:
    case nav::Action::JumpEvidence:
    case nav::Action::JumpPlan:
    case nav::Action::JumpCourse:
    case nav::Action::JumpRecord:
    case nav::Action::JumpChecker: {
        nav::Section section = nav::Section::Count;
        if (!nav::SectionForAction(action, &section)) return false;
        return JumpToSection(section);
    }
    case nav::Action::FormCenter:
    case nav::Action::FormDock:
    case nav::Action::FormFloat:
    case nav::Action::FormPet: {
        // The coordinator is the only owner of the presentation; this callback is
        // injected by MainFrame and never creates a second instance.
        const AgentForm form = nav::FormOf(action);
        if (!m_formRequest) return false;
        return m_formRequest(form);
    }
    case nav::Action::Count:
        break;
    }
    return false;
}

bool AgentWorkspace::RevealSection(nav::Section section)
{
    if (m_destroying || section == nav::Section::Count) return false;
    if (section == nav::Section::Guide) {
        if (m_model.Mode() != vm::AppMode::Learning) m_model.SetMode(vm::AppMode::Learning);
        Arrange();
        if (m_guide != nullptr) {
            ScrollPageTo(m_guide->GetPosition());
            return true;
        }
        return false;
    }
    const int index = SupportSectionIndex(section);
    if (index < 0) return false;
    Arrange();
    // "先可见再展开定位": bring the support region into the page viewport before
    // expanding the section, then scroll the section list to the header.
    if (m_support != nullptr) ScrollPageTo(m_support->GetPosition());
    m_supportSections->SetExpanded(index, true);
    ArrangeSupport();
    ScrollSectionIntoView(index);
    return true;
}

void AgentWorkspace::ScrollPageTo(const wxPoint& target)
{
    if (m_scroll == nullptr) return;
    int unitX = 0;
    int unitY = 0;
    m_scroll->GetScrollPixelsPerUnit(&unitX, &unitY);
    const int unit = std::max(1, unitY);
    const int top = std::max(0, target.y - Dip(this, S::PagePadding));
    m_scroll->Scroll(-1, top / unit);
}

void AgentWorkspace::ScrollSectionIntoView(int sectionIndex)
{
    if (m_supportBody == nullptr || m_supportSections == nullptr) return;
    const wxRect header = m_supportSections->HeaderRectPx(sectionIndex);
    if (header.IsEmpty()) return;
    int unitX = 0;
    int unitY = 0;
    m_supportBody->GetScrollPixelsPerUnit(&unitX, &unitY);
    const int unit = std::max(1, unitY);
    // The accordion starts at the support body's virtual padding; header.y is
    // relative to the accordion, so this is already a virtual coordinate.
    const int target = Dip(this, S::CardBodyPadX) + header.y;
    m_supportBody->Scroll(0, std::max(0, target - Dip(this, S::CardBodyPadY)) / unit);
}

void AgentWorkspace::FocusComposer()
{
    if (!m_destroying && m_draft && m_draft->IsShownOnScreen()) {
        m_draft->SetFocus();
    }
}

void AgentWorkspace::SetDocked(bool docked)
{
    if (m_docked == docked) return;
    m_docked = docked;
    Arrange();
    Refresh(false);
}

bool AgentWorkspace::ContainsFocus() const
{
    auto* focus = wxWindow::FindFocus();
    return focus && (focus == this || IsDescendant(focus));
}

AgentWorkspace::ViewState AgentWorkspace::CaptureViewState() const
{
    ViewState state;
    if (m_scroll) state.page = m_scroll->GetViewStart();
    if (m_thread) {
        state.thread = m_thread->GetViewStart();
        state.threadAnchor = m_thread->TopAnchorId();
        state.threadOffset = m_thread->TopAnchorOffset();
        state.threadAtBottom = m_thread->IsAtBottom();
    }
    if (m_supportBody) state.support = m_supportBody->GetViewStart();
    if (m_guideScroll) state.guide = m_guideScroll->GetViewStart();
    auto* focus = wxWindow::FindFocus();
    if (focus && (focus == this || IsDescendant(focus))) state.focus = focus;
    return state;
}

void AgentWorkspace::RestoreViewState(const ViewState& state)
{
    if (m_destroying) return;
    // Arrange() recomputes geometry first; restoring scroll units afterwards
    // keeps the reading position without resurrecting a stale layout.
    if (m_scroll) m_scroll->Scroll(state.page.x, state.page.y);
    if (m_thread) {
        if (state.threadAtBottom) m_thread->ScrollToBottom();
        else if (!state.threadAnchor.empty())
            m_thread->RestoreTopAnchor(state.threadAnchor, state.threadOffset);
        else m_thread->Scroll(state.thread.x, state.thread.y);
    }
    if (m_supportBody) m_supportBody->Scroll(state.support.x, state.support.y);
    if (m_guideScroll) m_guideScroll->Scroll(state.guide.x, state.guide.y);
    if (state.focus && state.focus != this && IsDescendant(state.focus) &&
        state.focus->IsShownOnScreen() && state.focus->IsEnabled()) {
        state.focus->SetFocus();
    }
}

void AgentWorkspace::SyncDraft()
{
    if (m_destroying || !m_draft || m_composerView == nullptr) return;
    const auto& draft = m_model.Draft();
    const wxString value = wxString::FromUTF8(draft.data(), draft.size());
    // Keep the submit state in step with the model draft: NewAsk and any other
    // programmatic clear must be visible to the "which draft does a receipt clear"
    // rule, never only to the control.
    m_submit.SetDraft(draft);
    if (value == m_draft->GetValue()) return;
    m_syncingDraft = true;
    // ChangeValue deliberately emits no wxEVT_TEXT and preserves model truth.
    m_composerView->SetDraftText(value);
    m_syncingDraft = false;
}

void AgentWorkspace::SyncConversation()
{
    if (m_destroying || m_thread == nullptr) return;
    // One snapshot of the model: messages, cards, the result fields the backend
    // actually returned, and the explicit demo flag. No suggestion text exists in
    // the v1 contract, so the chip row stays empty instead of inventing prompts.
    ConversationContent content;
    content.messages = m_model.Messages();
    content.cards = m_model.Cards();
    const vm::StatusView& status = m_model.Status();
    content.modelUsed = status.modelUsed;
    content.statusText = status.rawStatus;
    content.reason = status.reason;
    content.limitations = status.limitations;
    content.request = status.request;
    content.demo = m_model.DemoMode();
    content.stages = m_model.Stages();
    content.omittedStages = m_model.OmittedStageCount();
    content.stageScope = {status.projectId, status.revision, std::to_string(status.generation),
                          status.sessionId, status.runId, content.demo ? "demo" : "live"};
    content.runStatus = status.status;
    content.progressWaiting = status.busy;
    content.progressVisible = !content.stages.empty() || (!content.demo && !status.runId.empty());
    // SetContent returns false when nothing changed, so a draft keystroke (which
    // also lands here) never rebuilds or re-measures the message rows.
    m_thread->SetContent(content);
}

bool AgentWorkspace::InFlight() const
{
    // Adapter acceptance precedes the first model status update. Keep this gap
    // locked until explicit completion so a second click cannot submit twice.
    if (m_lastAcceptedSubmission != 0) return true;
    // The model owns "a request is running"; the composer never uses a timer to
    // decide completion. Cancelling states are still in flight until the terminal one.
    switch (m_model.Request()) {
    case vm::RequestState::Busy:
    case vm::RequestState::CancellingLocal:
    case vm::RequestState::CancelRequested:
        return true;
    default:
        return false;
    }
}

void AgentWorkspace::SyncComposer()
{
    if (m_destroying || m_composerView == nullptr) return;
    // Freeze-time context: the host's verified project/revision/session + the level
    // the learner selected now. Missing facts stay empty; the gate refuses to send.
    submit::Context context = m_submitContext;
    const vm::Level level = m_model.CurrentLevel();
    context.level = level.bound() ? level.ToText() : std::string();
    m_submit.SetContext(context);
    m_submit.SetReadiness(submit::EvaluateReadiness(m_submitHandler != nullptr, context));
    m_submit.SetDraft(m_model.Draft());
    const bool busy = m_externalBusy || m_submit.HasPending() || InFlight();
    m_submit.SetBusy(busy);
    m_composerView->SetReady(m_submit.Readiness());
    m_composerView->SetBusy(busy);
}

void AgentWorkspace::SetSubmitHandler(submit::Handler handler)
{
    m_submitHandler = std::move(handler);
    SyncComposer();
}

void AgentWorkspace::SetTeachingContext(submit::Context context)
{
    if (m_submitContext == context) return;
    if (m_submitContext != context) {
        m_submit.Abandon();
        m_lastAcceptedSubmission = 0;
    }
    m_submitContext = std::move(context);
    SyncNavigation();
}

void AgentWorkspace::SetExternalBusy(bool busy)
{
    if (m_externalBusy == busy) return;
    m_externalBusy = busy;
    SyncComposer();
}

void AgentWorkspace::SetReportSelectionHandler(std::function<void()> handler)
{
    m_reportSelection = std::move(handler);
}

void AgentWorkspace::SetReportJob(const std::string& jobId, bool enabled)
{
    if (!m_report) return;
    if (m_report->IsEnabled() != enabled) m_report->Enable(enabled);
    const wxString tip = jobId.empty() ? Text("选择当前工程当前版本的终态 Job 报告作为教学证据") :
                                        Text("当前报告：") + Text(jobId);
    if (m_report->GetToolTipText() != tip) m_report->SetToolTip(tip);
    const wxString label = jobId.empty() ? Text("选择报告") : Text("报告已选择");
    if (m_report->GetLabel() != label) m_report->SetLabel(label);
}

void AgentWorkspace::SetWorkflowActions(bool visible, bool hint, bool finish, bool cancel,
                                        std::function<void(const std::string&)> handler)
{
    if (!m_composerView) return;
    m_composerView->SetWorkflowActions(visible, hint, finish, cancel, std::move(handler));
    if (!m_arranging) Arrange();
}

void AgentWorkspace::SetRecoveryActions(bool refreshEvidenceVisible, bool recoverVisible,
                                        const wxString& recoverReason)
{
    if (m_destroying || !m_composerView) return;
    m_composerView->SetRecoveryActions(refreshEvidenceVisible, recoverVisible, recoverReason);
    if (!m_arranging) Arrange();
}

void AgentWorkspace::SetWorkflowFeedback(const wxString& message)
{
    PushSubmitFeedback(message);
}

submit::Readiness AgentWorkspace::Readiness() const
{
    return m_submit.Readiness();
}

std::uint64_t AgentWorkspace::PendingSubmissionId() const
{
    return m_submit.HasPending() ? m_submit.Pending().submissionId : 0;
}

void AgentWorkspace::PushSubmitFeedback(const wxString& text)
{
    if (m_destroying || m_composerView == nullptr) return;
    m_composerView->SetFeedback(text);
    // A feedback line changes the composer height; re-run the conversation layout so
    // it is never clipped and the thread keeps its reading position.
    if (!m_arranging) ArrangeConversation();
}

bool AgentWorkspace::SubmitQuestion(const wxString& text)
{
    if (m_destroying || m_composerView == nullptr) return false;
    if (m_submitting) {
        // Re-entrant submit (the adapter called back into us): refuse, never send twice.
        m_composerView->SetFeedback(Text(submit::RejectText(submit::Reject::Busy)));
        return false;
    }
    const wxScopedCharBuffer utf8 = text.utf8_str();
    const std::string body(utf8.data(), utf8.length());
    // The state's draft mirrors the model draft, not arbitrary external text.
    // A confirmed external submission must not clear a different draft.
    m_submit.SetDraft(m_model.Draft());
    m_submissionId += 1;
    std::string explanation;
    const submit::ComposerState::Start start = m_submit.Begin(m_submissionId, body, &explanation);
    if (start != submit::ComposerState::Start::Started) {
        PushSubmitFeedback(Text(explanation));
        return false;
    }
    const submit::Intent intent = m_submit.Pending();
    const std::shared_ptr<bool> alive = m_alive;
    if (!m_submitHandler) {
        // Readiness already refused this; keep the draft and say why.
        m_submit.Settle(intent.submissionId, submit::ReceiptStatus::NotReady, &explanation);
        PushSubmitFeedback(Text(m_submit.Readiness().explanation));
        return false;
    }
    m_submitting = true;
    const submit::Handler handler = m_submitHandler;
    submit::Receipt receipt;
    try {
        receipt = handler(intent);
    } catch (...) {
        // A throwing adapter is a rejection, never a silent success; the draft stays.
        receipt.status = submit::ReceiptStatus::Rejected;
        receipt.explanation = "提交时发生内部错误，请稍后重试。";
    }
    if (!alive || !*alive) {
        // The adapter destroyed this workspace on the same stack frame. Touch nothing.
        return false;
    }
    m_submitting = false;
    // The adapter may have changed the draft synchronously (models/config/mode are all
    // allowed). Re-read it before deciding "is the draft still the one we submitted":
    // never clear a draft that the adapter or the learner wrote during the flight.
    m_submit.SetDraft(m_model.Draft());
    std::string settledExplanation;
    const submit::ComposerState::SettleResult settled =
        m_submit.Settle(intent.submissionId, receipt.status, &settledExplanation);
    if (settled == submit::ComposerState::SettleResult::Stale ||
        settled == submit::ComposerState::SettleResult::Duplicate) {
        return false;
    }
    if (settled == submit::ComposerState::SettleResult::ClearedDraft &&
        m_model.Draft() != m_submit.Draft()) {
        // Clearing the model draft drives the control through the normal subscription;
        // never call ChangeValue here (that would bypass the shared draft).
        m_model.SetDraft(m_submit.Draft());
    }
    if (receipt.status == submit::ReceiptStatus::Accepted) {
        // The adapter accepted this submission and will report completion later; only
        // this id may produce a completion notice (a late/other id is ignored).
        m_lastAcceptedSubmission = intent.submissionId;
    }
    wxString feedback;
    if (receipt.status == submit::ReceiptStatus::Accepted) {
        feedback = receipt.explanation.empty() ? Text("已提交。") : Text(receipt.explanation);
    } else if (!receipt.explanation.empty()) {
        feedback = Text(receipt.explanation);
    } else {
        feedback = Text(submit::RejectText(
            receipt.status == submit::ReceiptStatus::Busy ? submit::Reject::Busy
                                                          : submit::Reject::NotReady));
    }
    // Recompute busy/readiness after the settlement: the adapter may have changed the
    // model (or nothing at all), and a stale "busy" would leave send disabled forever.
    SyncComposer();
    PushSubmitFeedback(feedback);
    return receipt.status == submit::ReceiptStatus::Accepted;
}

void AgentWorkspace::NotifySubmissionFinished(std::uint64_t submissionId, bool accepted,
                                              const wxString& explanation)
{
    if (m_destroying || m_composerView == nullptr) return;
    if (submissionId == 0 || submissionId != m_lastAcceptedSubmission) {
        // Late or unknown receipt: it must not change drafts, feedback or busy state.
        return;
    }
    m_lastAcceptedSubmission = 0;
    // Completion never clears the draft: the learner may already be writing the next
    // question. Only an Accepted receipt at submit time may clear its own snapshot.
    SyncComposer();
    wxString feedback = explanation;
    if (feedback.empty()) {
        feedback = accepted ? Text("回答完成，可以继续提问。")
                            : Text("这次没有完成，请查看结果说明后重试。");
    }
    PushSubmitFeedback(feedback);
}

bool AgentWorkspace::SyncSupport()
{
    if (!m_issueSection || !m_evidenceSection) return false;
    auto projection = support::Project(m_model,
        m_submitContext.projectId == m_model.BoundProjectId() ? m_submitContext.revision : std::string());
    if (!m_submitContext.projectId.empty() && !m_model.BoundProjectId().empty() &&
        m_submitContext.projectId != m_model.BoundProjectId()) {
        projection.issueSummary = "当前问题 · 等待新工程会话";
        const support::Block warning{"工程已切换", "以下为旧会话内容，不能作为当前工程证据；等待新会话更新。"};
        projection.issues.insert(projection.issues.begin(), warning);
        projection.evidence.insert(projection.evidence.begin(), warning);
        for (auto& target : projection.locations) target.eligible = false;
    }
    const bool issueChanged = m_issueSection->SetProjection(projection);
    const bool evidenceChanged = m_evidenceSection->SetProjection(projection);
    m_supportSections->SetSectionTitle(SupportSectionIndex(nav::Section::Issue), Text(projection.issueSummary));
    m_supportSections->SetSectionTitle(SupportSectionIndex(nav::Section::Evidence), Text(projection.evidenceSummary));
    // 验证计划分区：折叠标题显示真实计划摘要（无计划时诚实写“尚未生成”）。
    // 内容本身由 ArrangeSupport → nav::ProjectSection 渲染，只读、无审批入口。
    const nav::SectionProjection planProjection = nav::ProjectSection(m_model, nav::Section::Plan);
    const int planIndex = SupportSectionIndex(nav::Section::Plan);
    if (planIndex >= 0) {
        wxString planTitle = Text(nav::SectionTitle(nav::Section::Plan));
        if (!planProjection.summary.empty()) planTitle += Text(" · ") + planProjection.summary;
        m_supportSections->SetSectionTitle(planIndex, planTitle);
    }
    const bool planChanged = planProjection.summary != m_lastPlanSummary;
    m_lastPlanSummary = planProjection.summary;

    // 课程引用分区：折叠标题只显示条数/未生成，内容仍是只读元数据。
    const nav::SectionProjection courseProjection =
        nav::ProjectSection(m_model, nav::Section::Course);
    const int courseIndex = SupportSectionIndex(nav::Section::Course);
    if (courseIndex >= 0) {
        wxString courseTitle = Text(nav::SectionTitle(nav::Section::Course));
        if (!courseProjection.summary.empty()) courseTitle += Text(" · ") + courseProjection.summary;
        m_supportSections->SetSectionTitle(courseIndex, courseTitle);
    }
    const bool courseChanged = courseProjection.summary != m_lastCourseSummary;
    m_lastCourseSummary = courseProjection.summary;
    return issueChanged || evidenceChanged || planChanged || courseChanged;
}

void AgentWorkspace::SyncNavigation()
{
    if (m_destroying || m_topBar == nullptr) return;
    SyncConversation();
    SyncComposer();
    const bool supportChanged = SyncSupport();
    const nav::StatusProjection status = nav::ProjectStatus(m_model);
    const wxString line = nav::ComposeStatusLine(status);

    m_topBar->SetMode(m_model.Mode());
    m_topBar->SetLevel(nav::LevelNumber(m_model.CurrentLevel()));
    m_topBar->SetDemo(m_model.DemoMode());
    m_topBar->SetStatus(status);

    if (m_statusStrip != nullptr) {
        std::vector<wxString> segments;
        // Build every separator through FromUTF8: a narrow " · " literal would be
        // decoded with the system ANSI code page (0xC2B7 becomes "路" under GBK).
        segments.push_back(status.statusText + Text(" · ") + status.ruleMode + Text(" · ") +
                           status.modelUsed);
        segments.push_back(status.policy);
        segments.push_back(status.session);
        m_statusStrip->SetSegments(segments);
    }
    if (m_head != nullptr) {
        // No answer-mode field exists in v1 results yet; the honest tag says so
        // instead of hardcoding the prototype's "深答 · 思考链".
        m_head->SetModeTag(Text("回答模式未知"));
        m_head->SetFormSelection(m_form);
    }
    if (m_rail != nullptr) {
        m_rail->SetPressed(nav::Action::ToggleGuide,
                           m_model.Mode() == vm::AppMode::Learning);
        m_rail->SetFormPressed(m_form);
    }
    // Six-step path: one projection from the same model. Demo is only ever true
    // through AgentViewModel::DemoMode(); live stays explicitly unknown.
    if (m_path != nullptr) {
        const path::Projection projection = path::ProjectPath(m_model);
        if (!(projection == m_lastPath)) {
            m_path->SetProjection(projection);
            m_lastPath = projection;
        }
    }
    const bool guide = m_model.Mode() == vm::AppMode::Learning;
    const bool modeChanged = m_model.Mode() != m_lastMode;
    // Hiding the guide must not lose the learner's reading position: remember the
    // scroll units before the column disappears and restore them on the way back.
    if (modeChanged && !guide && m_guideScroll != nullptr) {
        m_guideScrollPos = m_guideScroll->GetViewStart();
        m_guideScrollSaved = true;
    }
    if (m_guide != nullptr && m_guide->IsShown() != guide) m_guide->Show(guide);

    const bool geometryChanged = supportChanged || guide != m_lastGuideShown ||
                                 m_model.Mode() != m_lastMode ||
                                 m_model.DemoMode() != m_lastDemo ||
                                 line != m_lastStatusLine;
    m_lastGuideShown = guide;
    m_lastMode = m_model.Mode();
    m_lastDemo = m_model.DemoMode();
    m_lastStatusLine = line;
    if (geometryChanged) {
        Arrange();
        if (modeChanged && guide && m_guideScrollSaved && m_guideScroll != nullptr) {
            m_guideScroll->Scroll(m_guideScrollPos.x, m_guideScrollPos.y);
        }
    } else {
        if (m_topBar != nullptr) m_topBar->Refresh(false);
        if (m_support != nullptr) m_support->Refresh(false);
    }
}

void AgentWorkspace::ApplyTheme()
{
    if (m_destroying) return;
    const auto& colours = tokens::Colours();
    SetBackgroundColour(colours.pageBg);
    m_scroll->SetBackgroundColour(colours.pageBg);
    m_sheet->SetBackgroundColour(colours.pageBg);
    for (auto* panel : {static_cast<wxWindow*>(m_conversationBody),
                        static_cast<wxWindow*>(m_guideBody),
                        static_cast<wxWindow*>(m_guideScroll),
                        static_cast<wxWindow*>(m_supportBody)}) {
        panel->SetBackgroundColour(tokens::SurfaceColour(tokens::Component::Card));
    }
    for (auto* label : {m_guideNote}) {
        label->SetFont(tokens::UiFontPx(this, T::BodyPx));
        label->SetForegroundColour(colours.text2);
        label->SetBackgroundColour(label->GetParent()->GetBackgroundColour());
    }
    for (auto* label : m_sectionBody) {
        if (label == nullptr) continue;
        label->SetFont(tokens::UiFontPx(this, T::SmallBodyPx));
        label->SetForegroundColour(colours.text2);
        label->SetBackgroundColour(label->GetParent()->GetBackgroundColour());
    }
    // Prototype .guide-note: 10.5 px, text3, top hairline (H133).
    m_guideNote->SetFont(tokens::UiFontPx(this, T::TinyPx));
    m_guideNote->SetForegroundColour(colours.text3);
    // The composer owns its own fonts/colours (and the send button's); the workspace
    // must not override them or the token roles drift.
    if (m_composerView != nullptr) m_composerView->ApplyTheme();
    m_return->SetFont(tokens::UiFontPx(this, T::LabelPx, T::Medium));
    m_scroll->SetScrollRate(0, std::max(1, Dip(this, S::ThreadGap)));
    // The conversation component owns its own scroll rate (one text line per unit
    // keeps anchor arithmetic exact), so the workspace must not overwrite it.
    m_guideScroll->SetScrollRate(0, std::max(1, Dip(this, S::ThreadGap)));
    m_supportBody->SetScrollRate(0, std::max(1, Dip(this, S::ThreadGap)));
    Arrange();
    Refresh(false);
}

int AgentWorkspace::ConversationMinimumHeight(int cardWidth)
{
    if (m_conversation == nullptr || m_head == nullptr || m_statusStrip == nullptr) return 0;
    const int width = std::max(0, cardWidth);
    const int headHeight = m_head->Arrange(0, 0, width);
    const int statusHeight = m_statusStrip->Arrange(width);
    const int line = tokens::LineHeightDip(this, T::BodyPx, T::LineHeightBody);
    const int gap = Dip(this, S::ComposeGap);
    // AGUI-T13: the composer measures itself (40 DIP input + send + optional feedback
    // line); the card adds its header and a radius inset. This is a minimum-height
    // estimate, so it must not move the live card (ArrangeConversation owns geometry).
    const int cardInset = Dip(this, M::CardInset);
    const int innerWidth = std::max(0, width - 2 * cardInset);
    const int viewHeight = m_composerView != nullptr
                               ? m_composerView->Arrange(0, 0, innerWidth)
                               : 0;
    const int composerHeight = m_composer->HeaderHeightPx() + viewHeight + 2 * cardInset;
    return headHeight + statusHeight + 5 * line + composerHeight + 6 * gap;
}

void AgentWorkspace::Arrange()
{
    if (m_destroying || m_arranging || !m_scroll || !m_sheet || m_topBar == nullptr) return;
    m_arranging = true;
    const wxSize viewport = m_scroll->GetClientSize();
    const int width = std::max(0, viewport.x);
    // AGUI-T11: every breakpoint is decided exactly once, from the page's DIP width.
    // The page width (not the scroll client, which loses ~17 DIP to a vertical
    // scrollbar) is the "available workspace" DEC-08 refers to; the scrollbar must
    // never push a 961-DIP page into the 960 tier.
    const layout::Metrics metrics = tokens::LayoutMetrics();
    const int dipWidth = tokens::DipToCssPx(this, std::max(0, GetClientSize().x));
    const layout::Breakpoint tier = layout::ForWidth(dipWidth, metrics);
    const int gap = tokens::CssPxToDip(this, metrics.columnGap);
    const int pad = std::min(tokens::CssPxToDip(this, metrics.pagePad), width / 2);
    m_compact = layout::RailIsHorizontal(dipWidth, metrics);

    // 1) In-page top bar spans the whole page width (DEC-14: the application's own
    //    title bar/menu/status bar/activity rail stay outside and untouched).
    const int topBarHeight = m_topBar->Arrange(width);
    m_topBar->SetSize(0, 0, width, topBarHeight);
    int y = topBarHeight;

    // 2) Page action row: return to the workbench, draft and history preserved.
    const int returnHeight = Dip(this, M::NarrowButtonHeight);
    const int returnWidth = std::min(std::max(0, width - 2 * pad),
                                     std::max(Dip(this, 64), m_return->GetBestSize().x));
    const int returnY = y + gap / 2;
    m_return->SetSize(std::max(0, width - pad - returnWidth), returnY, returnWidth,
                      returnHeight);
    if (m_report) {
        const int reportWidth = std::min(std::max(0, width - 2 * pad - returnWidth - gap),
                                         std::max(Dip(this, 64), m_report->GetBestSize().x));
        m_report->SetSize(pad, returnY, reportWidth, returnHeight);
    }
    y = returnY + returnHeight + gap / 2;

    // 3) Functional rail: vertical (58) on wide pages, wrapped horizontal row at
    //    <=960. Arrange() returns the natural height, so nothing is clipped.
    m_rail->SetCompact(m_compact);
    const int railWidth = m_compact ? width : tokens::CssPxToDip(this, metrics.railWidth);
    const int railHeight = m_rail->Arrange(railWidth);
    m_rail->SetSize(0, y, railWidth, railHeight);

    const int contentTop = m_compact ? y + railHeight + gap : y;
    const int contentX = m_compact ? 0 : railWidth;
    const int areaWidth = std::max(0, width - contentX);
    const int areaPad = std::min(tokens::CssPxToDip(this, metrics.pagePad), areaWidth / 2);
    const int gridWidth = std::max(0, areaWidth - 2 * areaPad);
    // Cards live to the right of the functional rail (the in-page rail column is
    // part of the page, not of the columns grid).
    const int areaLeft = contentX + areaPad;

    const bool guideVisible = m_model.Mode() == vm::AppMode::Learning;
    // Tier comes from the page width. The policy removes page padding itself,
    // so pass the area including that padding (only the rail was removed).
    const layout::Columns cols = layout::ColumnsForTier(
        tokens::DipToCssPx(this, areaWidth), tier, guideVisible, m_docked, metrics);
    const bool columns = !cols.stacked;

    int bottom = contentTop;
    if (!columns) {
        // Stacked reading order from the prototype <=960 rule:
        // conversation(1) -> guide(2) -> support(3).
        int stackY = contentTop;
        const int stackWidth = gridWidth > 0 ? gridWidth : areaWidth;
        const int stackX = areaLeft;
        const int conversationHeight =
            std::max(ConversationMinimumHeight(stackWidth),
                     tokens::CssPxToDip(this, metrics.mainMinHeight));
        m_conversation->SetSize(stackX, stackY, stackWidth, conversationHeight);
        stackY += conversationHeight + gap;
        // The six-step list scrolls inside the card; keep the stacked column
        // deliberately short so the path's own scroll position is meaningful.
        const int guideHeight = std::max(tokens::CssPxToDip(this, metrics.sideMinHeight) / 2,
                                         tokens::CssPxToDip(this, M::HeaderHeight));
        if (guideVisible) {
            ArrangeGuide(stackX, stackY, stackWidth, guideHeight);
            stackY += guideHeight + gap;
        } else if (m_guide != nullptr) {
            m_guide->Show(false);
        }
        const int supportHeight = std::max(tokens::CssPxToDip(this, metrics.sideMinHeight),
                                           viewport.y - contentTop);
        m_support->SetSize(stackX, stackY, stackWidth, supportHeight);
        bottom = stackY + supportHeight;
    } else {
        int conversationWidth = 0;
        int conversationX = 0;
        int supportX = 0;
        int supportWidth = 0;
        int guideWidth = 0;
        if (guideVisible) {
            guideWidth = std::min(tokens::CssPxToDip(this, cols.guide), gridWidth);
        }
        if (m_docked) {
            // dock: guide | support (wide) | conversation (narrow, right)
            conversationWidth = std::min(tokens::CssPxToDip(this, cols.main), gridWidth);
            supportWidth = std::max(0, gridWidth - guideWidth - conversationWidth -
                                           (guideVisible ? 2 * gap : gap));
            supportX = areaLeft + (guideVisible ? guideWidth + gap : 0);
            conversationX = areaLeft + gridWidth - conversationWidth;
        } else {
            // center: guide | conversation (wide) | support (narrow, right)
            supportWidth = std::min(tokens::CssPxToDip(this, cols.support), gridWidth);
            conversationWidth = std::max(0, gridWidth - guideWidth - supportWidth -
                                                (guideVisible ? 2 * gap : gap));
            conversationX = areaLeft + (guideVisible ? guideWidth + gap : 0);
            supportX = conversationX + conversationWidth + gap;
        }
        const int columnHeight = std::max(
            ConversationMinimumHeight(conversationWidth),
            std::max(tokens::CssPxToDip(this, metrics.sideMinHeight),
                     viewport.y - contentTop - gap));
        m_conversation->SetSize(conversationX, contentTop, conversationWidth, columnHeight);
        m_support->SetSize(supportX, contentTop, supportWidth, columnHeight);
        if (guideVisible) {
            ArrangeGuide(areaLeft, contentTop, guideWidth, columnHeight);
        } else if (m_guide != nullptr) {
            m_guide->Show(false);
        }
        bottom = contentTop + columnHeight;
    }

    ArrangeConversation();
    ArrangeSupport();

    const int virtualHeight = std::max(viewport.y, bottom + pad);
    int sheetX = 0;
    int sheetY = 0;
    m_scroll->CalcScrolledPosition(0, 0, &sheetX, &sheetY);
    m_sheet->SetSize(sheetX, sheetY, width, virtualHeight);
    m_scroll->SetVirtualSize(width, virtualHeight);

    // Float drag may only start on the brand band of the in-page top bar; the
    // buttons/sections keep their own mouse handling and can never drag.
    m_dragHandle = wxRect(pad, 0, std::max(0, m_topBar->BrandRectPx().GetRight()), topBarHeight);
    m_arranging = false;
}

void AgentWorkspace::ArrangeGuide(int x, int y, int width, int height)
{
    if (m_guide == nullptr || m_guideBody == nullptr || m_guideScroll == nullptr ||
        m_path == nullptr) {
        return;
    }
    m_guide->Show(true);
    m_guide->SetSize(x, y, std::max(0, width), std::max(0, height));
    const wxSize area = m_guideBody->GetClientSize();
    if (area.x <= 0 || area.y <= 0) return;
    const int pad = std::min(Dip(this, S::CardBodyPadX), std::max(0, area.x / 2));
    const int gap = Dip(this, S::AccBodyGap);
    const int textWidth = std::max(0, area.x - 2 * pad);

    // The footer note is pinned to the bottom (prototype .guide-note margin-top:auto);
    // only the step list scrolls, so the note stays readable while browsing steps.
    const int noteHeight = WrapForWidth(m_guideNote, m_guideNote->GetFont(),
                                        Text(GuideNoteText()), textWidth).height;
    const int noteZone = noteHeight + Dip(this, S::CardBodyPadY) + Dip(this, S::HintPadTop);
    const int scrollHeight = std::max(0, area.y - noteZone - gap);
    m_guideScroll->SetSize(pad, Dip(this, S::CardBodyPadY), textWidth, scrollHeight);

    // The vertical scrollbar narrows the client area; size the six-step panel to
    // the real client width so it can never overflow horizontally (no HSCROLL).
    int pathWidth = std::max(0, textWidth);
    int pathHeight = m_path->Arrange(pathWidth);
    m_guideScroll->SetVirtualSize(pathWidth, std::max(scrollHeight, pathHeight));
    const int clientWidth = m_guideScroll->GetClientSize().x;
    if (clientWidth > 0 && clientWidth < pathWidth) {
        pathWidth = clientWidth;
        pathHeight = m_path->Arrange(pathWidth);
        m_guideScroll->SetVirtualSize(pathWidth, std::max(scrollHeight, pathHeight));
    }
    int pathX = 0;
    int pathY = 0;
    m_guideScroll->CalcScrolledPosition(0, 0, &pathX, &pathY);
    m_path->SetSize(pathX, pathY, pathWidth, pathHeight);

    const int noteTop = std::max(0, area.y - noteHeight - Dip(this, S::CardBodyPadY));
    PlaceText(m_guideNote, Text(GuideNoteText()), pad, noteTop, textWidth);
    m_guideBody->SetMinSize(wxSize(area.x, std::max(area.y, noteTop + noteHeight)));
}

void AgentWorkspace::ArrangeSupport()
{
    if (m_supportSections == nullptr || m_supportBody == nullptr) return;
    const wxSize area = m_supportBody->GetClientSize();
    const int pad = std::min(Dip(this, S::CardBodyPadX), std::max(0, area.x / 2));
    const int width = std::max(0, area.x - 2 * pad);
    const int bodyPad = Dip(this, S::AccBodyPadX);
    const int innerPad = Dip(this, S::CardBodyPadX);
    const int textWidth = std::max(0, width - 2 * bodyPad - 2 * innerPad);

    int index = 0;
    for (auto section : nav::SectionCatalog()) {
        if (section == nav::Section::Guide) continue;
        const int current = index++;
        if (current >= static_cast<int>(m_sectionBody.size())) break;
        if (section == nav::Section::Issue || section == nav::Section::Evidence) {
            auto* body = section == nav::Section::Issue ? m_issueSection : m_evidenceSection;
            if (body) {
                const int bodyWidth = std::max(0, width - 2 * bodyPad);
                const int height = body->Arrange(bodyWidth);
                body->SetSize(0, 0, bodyWidth, height);
            }
            continue;
        }
        wxStaticText* label = m_sectionBody[static_cast<size_t>(current)];
        if (label == nullptr) continue;
        const nav::SectionProjection projection = nav::ProjectSection(m_model, section);
        const wxString text = projection.body;
        const int textHeight = PlaceText(label, text, innerPad, Dip(this, S::CardBodyPadY),
                                         textWidth);
        auto* body = dynamic_cast<SectionBody*>(label->GetParent());
        if (body != nullptr) {
            const int bodyWidth = std::max(0, width - 2 * bodyPad);
            const int bodyHeight = textHeight + 2 * Dip(this, S::CardBodyPadY);
            body->SetMinSize(wxSize(bodyWidth, bodyHeight));
            // Collapsed sections are not sized by the accordion; give them their
            // measured geometry so the hidden label never exceeds its parent.
            body->SetSize(0, 0, bodyWidth, bodyHeight);
        }
        label->SetToolTip(projection.reason.empty() ? text
                                                    : text + "\n" + projection.reason);
    }
    // Section titles carry the honest summary; the accordion owns no data.
    m_supportSections->SetSize(0, 0, width, 0);
    m_supportSections->Relayout();
    const int accordionHeight = m_supportSections->GetBestSize().y;
    // Children of a scrolled window are stored in client coordinates, so the
    // virtual position has to be translated by the current view start.
    int accordionX = pad;
    int accordionY = pad;
    m_supportBody->CalcScrolledPosition(pad, pad, &accordionX, &accordionY);
    m_supportSections->SetSize(accordionX, accordionY, width, accordionHeight);
    m_supportBody->SetVirtualSize(std::max(0, area.x),
                                  std::max(area.y, pad + accordionHeight + pad));
}

void AgentWorkspace::ArrangeConversation()
{
    if (m_destroying || !m_conversationBody || m_head == nullptr) return;
    const wxSize size = m_conversationBody->GetClientSize();
    const int pad = std::min(Dip(this, S::ComposePadX), std::max(0, size.x / 2));
    const int gap = Dip(this, S::ComposeGap);
    const int width = std::max(0, size.x - 2 * pad);
    const int line = tokens::LineHeightDip(this, T::BodyPx, T::LineHeightBody);
    // AGUI-T11: the message column is capped at 760 DIP and centred (prototype
    // .aw-body padding:14px max(16px,calc((100% - 760px)/2))). AGUI-T12 keeps the
    // same cap: long code lines and wide tables scroll inside their own row instead
    // of widening the column or breaking the page.
    const layout::ThreadColumn threadColumn = layout::ThreadColumnFor(
        tokens::DipToCssPx(this, size.x), tokens::LayoutMetrics());
    const int threadPad = std::min(tokens::CssPxToDip(this, threadColumn.pad),
                                   std::max(0, size.x / 2));
    const int threadWidth = std::max(0, size.x - 2 * threadPad);

    const int headHeight = m_head->Arrange(0, 0, size.x);
    const int statusHeight = m_statusStrip->Arrange(size.x);
    // Arrange() only measures/sizes; the strip must sit under the header instead
    // of painting over it at (0,0).
    m_statusStrip->SetSize(0, headHeight, size.x, statusHeight);
    const int threadTop = headHeight + statusHeight;

    // AGUI-T13: measure the composer at the width the card will really give its
    // content. ContentRectPx() is the card's own radius-based inset; guessing it with
    // M::CardInset would wrap the feedback line at a slightly wider width and clip it.
    const int cardInset = Dip(this, M::CardInset);
    m_composer->SetSize(pad, 0, width,
                        m_composer->HeaderHeightPx() + 64 + 2 * cardInset);
    const wxRect content = m_composer->ContentRectPx();
    const int composerViewHeight =
        m_composerView->Arrange(content.x, content.y, std::max(0, content.width));
    const int composerHeight = content.y + composerViewHeight + content.x;
    const int threadHeight = std::max(line, size.y - threadTop - composerHeight - 3 * gap);
    m_thread->SetSize(threadPad, threadTop + gap, threadWidth, threadHeight);
    m_composer->SetSize(pad, threadTop + gap + threadHeight + gap, width, composerHeight);

    // AGUI-T12: the thread area owns its row layout. Reflowing it must not move the
    // reader: a bottom-following view keeps following, an upward-reading view keeps
    // the same top row and the same intra-row offset (form switch, mode switch and
    // window resize all land here).
    const bool atBottom = m_thread->IsAtBottom();
    const wxString anchor = m_thread->TopAnchorId();
    const int anchorOffset = m_thread->TopAnchorOffset();
    m_thread->Relayout();
    if (atBottom) {
        m_thread->ScrollToBottom();
    } else if (!anchor.empty()) {
        m_thread->RestoreTopAnchor(anchor, anchorOffset);
    }
}

} // namespace agent::gui
