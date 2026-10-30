#pragma once

#include "agent/gui/AgentComposer.h"
#include "agent/gui/AgentConversation.h"
#include "agent/gui/AgentLearningPath.h"
#include "agent/gui/AgentNavigation.h"
#include "agent/gui/AgentSubmit.h"
#include "agent/gui/AgentSupport.h"
#include "agent/gui/AgentViewModel.h"
#include "ui/UiTheme.h"

#include <wx/panel.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class wxScrolledWindow;
class wxStaticText;
class wxTextCtrl;

namespace agent::gui {
namespace controls {
class AgentAccordion;
class AgentButton;
class AgentCardHost;
}  // namespace controls

// T07's native host skeleton; AGUI-T09 adds the in-page top bar, functional rail,
// guide/support destinations and the two form-button groups. The model owns every
// business value; controls only project it. No request adapter, fixture, service,
// configuration write or MainFrame dependency lives here.
class AgentWorkspace final : public wxPanel
{
public:
    AgentWorkspace(wxWindow* parent, std::function<void()> onReturn);
    ~AgentWorkspace() override;

    vm::AgentViewModel& Model() { return m_model; }
    const vm::AgentViewModel& Model() const { return m_model; }
    wxTextCtrl* DraftControl() const { return m_draft; }
    void FocusComposer();
    // AGUI-T08: dock form puts the conversation in the narrow side column and the
    // support area in the wide main column (index.html setForm('dock')).
    void SetDocked(bool docked);
    bool IsDocked() const { return m_docked; }
    // Header band that may start a float drag; excludes the return action.
    wxRect DragHandleRect() const { return m_dragHandle; }
    // AGUI-T09: the in-page top bar carries the brand band, so it is the window
    // that receives the float drag gesture (see AgentFormCoordinator). Buttons
    // inside it keep their own mouse handling and never drag.
    wxWindow* DragSourceWindow() const { return m_topBar; }
    bool ContainsFocus() const;
    // Scroll position and focused child survive a reparent between forms.
    struct ViewState
    {
        wxPoint page;
        wxPoint thread;
        wxString threadAnchor;
        int threadOffset = 0;
        bool threadAtBottom = false;
        wxPoint support;
        wxPoint guide;
        wxWindow* focus = nullptr;
    };
    ViewState CaptureViewState() const;
    void RestoreViewState(const ViewState& state);

    // ----- AGUI-T09：页内顶部导航（模型是唯一真源，协调器是形态唯一真源）-----
    nav::AgentTopBar* TopBar() const { return m_topBar; }
    nav::AgentRail* NavigationRail() const { return m_rail; }
    nav::AgentConversationHeader* ConversationHeader() const { return m_head; }
    nav::AgentStatusStrip* StatusStrip() const { return m_statusStrip; }
    controls::AgentAccordion* SupportAccordion() const { return m_supportSections; }
    wxWindow* GuidePanel() const;

    // ----- AGUI-T10：六步学习路径（占位正文换成真实组件；live 六步显式未知）-----
    path::AgentLearningPath* LearningPath() const { return m_path; }
    // 学习路径的滚动区（名称见实现；模式切换时保留滚动位置）
    wxWindow* GuideScrollWindow() const;

    // ----- AGUI-T12：消息流投影（只读；内容来自模型，不发明数据）-----
    AgentConversation* Conversation() const { return m_thread; }

    // ----- AGUI-T13：统一提问提交入口（鼠标 / Enter / chips / 将来桌宠同一路径）-----
    AgentComposer* Composer() const { return m_composerView; }
    /// 提交适配器（T22 注入真实链路；未注入时门禁给 NotReady 并保留草稿）。
    void SetSubmitHandler(submit::Handler handler);
    /// 宿主注入的可信上下文（projectId/revision/sessionId 来自真实事实，不由本层猜）。
    void SetTeachingContext(submit::Context context);
    void SetExternalBusy(bool busy);
    void SetReportSelectionHandler(std::function<void()> handler);
    void SetReportJob(const std::string& jobId, bool enabled);
    void SetWorkflowActions(bool visible, bool hint, bool finish, bool cancel,
                            std::function<void(const std::string&)> handler);
    /// 恢复/刷新证据入口：只由宿主按后端 teaching_status 注入；
    /// recoverVisible 只有 teaching_status == RecoveryRequired 时为真。
    void SetRecoveryActions(bool refreshEvidenceVisible, bool recoverVisible,
                            const wxString& recoverReason);
    void SetWorkflowFeedback(const wxString& message);
    submit::Readiness Readiness() const;
    /// 统一提交：冻结文本与当前等级，走同一份校验/回执/清草稿规则。返回是否已受理。
    bool SubmitQuestion(const wxString& text);
    /// 完成一次已受理的提交（适配器/测试在回答结束时调用；不用定时器判完成）。
    void NotifySubmissionFinished(std::uint64_t submissionId, bool accepted,
                                  const wxString& explanation = {});
    std::uint64_t PendingSubmissionId() const;

    // 单一动作分发：rail / aw-head / 夹具都走同一条路径。返回 false = 未受理。
    bool Trigger(nav::Action action);
    // MainFrame 注入真实协调器入口（不新建协调器）。
    void SetFormRequestHandler(std::function<bool(AgentForm)> handler);
    // 协调器观察者回写：刷新两组 pressed，不改变业务状态。
    void SetFormState(AgentForm form);
    AgentForm CurrentForm() const { return m_form; }

    bool IsGuideVisible() const;
    // 导航目标：先让目标区可见，再展开并定位（INT-006）。
    bool RevealSection(nav::Section section);
    int SupportSectionIndex(nav::Section section) const;   ///< Guide/越界 → -1
    bool IsSupportSectionExpanded(nav::Section section) const;
    wxString SupportSectionBody(nav::Section section) const;
    support::AgentSupportSection* IssueSection() const { return m_issueSection; }
    support::AgentSupportSection* EvidenceSection() const { return m_evidenceSection; }
    wxString StatusLineText() const;
    int LevelSelection() const;   ///< 1..3；未知（含 U2/空）→ 0

private:
    void ApplyTheme();
    void Arrange();
    void ArrangeConversation();
    void ArrangeSupport();
    void ArrangeGuide(int x, int y, int width, int height);
    int ConversationMinimumHeight(int cardWidth);
    void SyncDraft();
    void SyncNavigation();
    void SyncConversation();
    void SyncComposer();
    bool SyncSupport();
    bool InFlight() const;
    void PushSubmitFeedback(const wxString& text);
    void EnsureSupportSectionBuilt();
    bool NewAsk();
    bool JumpToSection(nav::Section section);
    void ScrollPageTo(const wxPoint& target);
    void ScrollSectionIntoView(int sectionIndex);

    std::function<void()> m_onReturn;
    std::function<bool(AgentForm)> m_formRequest;
    // Subscriptions are destroyed/reset before both the model and wx children.
    vm::AgentViewModel m_model;
    vm::Subscription m_modelSubscription;
    UiTheme::Subscription m_theme;
    bool m_arranging = false;
    bool m_syncingDraft = false;
    bool m_destroying = false;
    bool m_docked = false;
    bool m_columns = true;
    bool m_compact = false;
    AgentForm m_form = AgentForm::None;
    wxRect m_dragHandle;

    wxScrolledWindow* m_scroll = nullptr;
    wxPanel* m_sheet = nullptr;
    nav::AgentTopBar* m_topBar = nullptr;
    nav::AgentRail* m_rail = nullptr;
    controls::AgentButton* m_return = nullptr;
    controls::AgentButton* m_report = nullptr;
    std::function<void()> m_reportSelection;

    controls::AgentCardHost* m_guide = nullptr;
    wxPanel* m_guideBody = nullptr;
    wxScrolledWindow* m_guideScroll = nullptr;
    path::AgentLearningPath* m_path = nullptr;
    wxStaticText* m_guideNote = nullptr;

    controls::AgentCardHost* m_conversation = nullptr;
    wxPanel* m_conversationBody = nullptr;
    nav::AgentConversationHeader* m_head = nullptr;
    nav::AgentStatusStrip* m_statusStrip = nullptr;
    AgentConversation* m_thread = nullptr;
    controls::AgentCardHost* m_composer = nullptr;
    AgentComposer* m_composerView = nullptr;
    wxTextCtrl* m_draft = nullptr;

    // AGUI-T13：提交状态/门禁/回执都在这里，视图只显示。
    submit::ComposerState m_submit;
    submit::Handler m_submitHandler;
    submit::Context m_submitContext;
    std::uint64_t m_submissionId = 0;
    std::uint64_t m_lastAcceptedSubmission = 0;
    bool m_submitting = false;
    bool m_externalBusy = false;
    // 宿主回调可能在同步栈里销毁本工作台；用存活令牌避免回写已析构成员。
    std::shared_ptr<bool> m_alive;

    controls::AgentCardHost* m_support = nullptr;
    wxScrolledWindow* m_supportBody = nullptr;
    controls::AgentAccordion* m_supportSections = nullptr;
    std::vector<wxStaticText*> m_sectionBody;
    support::AgentSupportSection* m_issueSection = nullptr;
    support::AgentSupportSection* m_evidenceSection = nullptr;
    bool m_sectionsBuilt = false;
    // 投影变化检测：只有影响几何的字段变化才重排，避免每次按键都整页重排。
    bool m_lastDemo = false;
    bool m_lastGuideShown = true;
    vm::AppMode m_lastMode = vm::AppMode::Learning;
    wxString m_lastStatusLine;
    wxString m_lastPlanSummary;   // 验证计划折叠标题摘要（变化时才重排）
    wxString m_lastCourseSummary; // 课程引用折叠标题摘要（变化时才重排）
    path::Projection m_lastPath;
    wxPoint m_guideScrollPos;   // 学习/工作切换与形态切换时保留路径滚动
    bool m_guideScrollSaved = false;
};

}  // namespace agent::gui
