#pragma once
//
// AgentComposer — AGUI-T13 统一提问输入区（.aw-compose：40 DIP 输入 + 渐变发送）
//
// 设计源：GUI/agent-gui/index.html H254–261（.aw-compose / .send#askGo）、H650–651；
//         spec.md §4.6（40px 输入、r-sm、placeholder、focus 蓝边+3px 环、40×40 渐变发送、
//         disabled 50%）、§5.3（提交 / busy / chips）；implementation.md §AGUI-T13。
// 追踪：LAY-007、CMP-017、CMP-018、INT-005、INT-007、INT-008、STA-002。
//
// 职责与边界：
//   * 只负责输入与"用户发起提交"的事件：鼠标发送、Enter、chips（经宿主）与将来的桌宠入口
//     都落到同一个宿主回调；本组件不持有模型、不发请求、不判任务完成、不用定时器。
//   * 草稿与模型双向同步：程序设置草稿不发用户事件；用户编辑只回调宿主。
//   * 键盘语义：Enter 发送、Shift+Enter 换行、Esc 一律不消费（留给 Coordinator 的
//     Esc 语义）；中文输入法合成确认期间不得误发（真实 wx/Win32 IME 状态保护 + 可测判定）。
//   * busy 时仍可编辑下一份草稿，只禁用发送并给出本地明确说明；空白/超长/未就绪都在本地
//     说明，不联网、不弹全局 toast（全局反馈属 T21/T34）。
#include "agent/gui/AgentSubmit.h"
#include "agent/gui/AgentTokens.h"
#include "ui/UiTheme.h"

#include <wx/panel.h>
#include <wx/string.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

class wxStaticText;
class wxTextCtrl;

namespace agent::gui {

namespace controls {
class AgentButton;
}  // namespace controls

class AgentComposer final : public wxPanel
{
public:
    explicit AgentComposer(wxWindow* parent);
    ~AgentComposer() override;

    // ---- 提交事件（宿主统一入口；返回 true = 已开始一次提交）----
    using SubmitCallback = std::function<bool(const wxString& text)>;
    void SetSubmitCallback(SubmitCallback callback);

    /// 用当前草稿发起提交（发送钮 / Enter / 将来桌宠入口）。
    /// 本地先做空白与门禁检查：不满足时只写反馈、不调用宿主回调，返回 false。
    bool RequestSubmit();

    /// 已由用户明确确认的外部文本提交（如未来桌宠）。建议按钮应先填草稿，不能调用此接口自动发送。
    bool SubmitTrustedText(const wxString& text);

    /// 宿主回调被调用且返回 true 的次数（断言"只发一次"）。
    size_t SubmitCount() const { return m_submitCount; }
    /// 用户发起次数（含被本地拒绝的；Enter/鼠标/chips 各算一次）。
    size_t SubmitAttemptCount() const { return m_attemptCount; }

    // ---- 草稿（与模型双向同步）----
    void SetDraftText(const wxString& text);   ///< 程序设置，不发用户事件
    wxString DraftText() const;
    void SetDraftEditedCallback(std::function<void(const wxString&)> callback);

    // ---- 状态 ----
    void SetBusy(bool busy);                    ///< 只禁用发送/改反馈，不清草稿
    bool IsBusy() const { return m_busy; }

    void SetReady(const submit::Readiness& readiness);
    const submit::Readiness& Readiness() const { return m_readiness; }

    void SetFeedback(const wxString& text);     ///< 本地明确说明（空 = 隐藏）
    void SetWorkflowActions(bool visible, bool hint, bool finish, bool cancel,
                            std::function<void(const std::string&)> handler);
    /// 恢复/刷新证据入口：只由工作台按后端状态注入。
    ///   * refreshEvidenceVisible：有活跃教学 run 时显示“刷新证据”；
    ///   * recoverVisible：仅 teaching_status == RecoveryRequired 时显示“恢复教学”；
    ///   * recoverReason：未显示/不可用时的固定原因（capability-gated 文案）。
    void SetRecoveryActions(bool refreshEvidenceVisible, bool recoverVisible,
                            const wxString& recoverReason);
    wxString FeedbackText() const;

    bool IsSendEnabled() const;
    wxString SendDisabledReason() const;

    // ---- 布局（工作台调用；返回占用高度）----
    int Arrange(int x, int y, int width);
    void ApplyTheme();

    // ---- 焦点 ----
    void FocusInput();
    bool HasInputFocus() const;
    bool HasFocusRing() const { return m_focusRing; }

    // ---- 键盘 / IME ----
    /// 纯判定：Shift 换行、IME 合成中不提交。其余门禁（busy/未就绪/空白）由调用方判断。
    static bool EnterCommits(bool shiftDown, bool imeComposing);
    /// 真实 wx/Win32 IME 合成状态（非 Windows 恒 false）。
    bool ImeComposing() const;
    /// 仅测试：覆盖 IME 合成状态；std::nullopt = 使用真实状态。
    void SetImeComposingOverrideForTest(std::optional<bool> composing);

    // ---- 诊断（测试/夹具用；不是样式 API）----
    wxTextCtrl* DraftControl() const { return m_input; }
    wxWindow* SendButtonWindow() const;
    int InputHeightPx() const { return m_inputHeight; }
    wxString PlaceholderText() const;
    size_t FeedbackLineCount() const { return m_feedbackLines; }

private:
    void OnSize(wxSizeEvent& event);
    void UpdateRecoveryButtons();
    void OnPaint(wxPaintEvent& event);
    void OnInputFocus(wxFocusEvent& event);
    void OnInputKeyDown(wxKeyEvent& event);
    void OnInputText(wxCommandEvent& event);
    void OnSendPressed();
    void UpdateSendState();
    void ShowFeedback(const wxString& text);
    wxFont InputFont() const;
    wxFont FeedbackFont() const;
    int GapPx() const;
    int SendSizePx() const;

    submit::Readiness m_readiness;
    SubmitCallback m_submit;
    std::function<void(const wxString&)> m_draftEdited;
    wxTextCtrl* m_input = nullptr;
    controls::AgentButton* m_send = nullptr;
    controls::AgentButton* m_hint = nullptr;
    controls::AgentButton* m_finish = nullptr;
    controls::AgentButton* m_cancel = nullptr;
    controls::AgentButton* m_refresh = nullptr;
    controls::AgentButton* m_refreshEvidence = nullptr;
    controls::AgentButton* m_recover = nullptr;
    bool m_workflowVisible = false;
    bool m_refreshEvidenceRequested = false;
    bool m_recoverRequested = false;
    wxString m_recoverReason;
    std::function<void(const std::string&)> m_workflowAction;
    wxStaticText* m_feedback = nullptr;
    wxString m_feedbackText;
    size_t m_feedbackLines = 0;
    size_t m_submitCount = 0;
    size_t m_attemptCount = 0;
    bool m_busy = false;
    bool m_focusRing = false;
    bool m_syncingDraft = false;
    int m_inputHeight = 0;
    int m_sendSize = 0;
    int m_width = 0;
    int m_height = 0;
    std::optional<bool> m_imeOverride;
    // 宿主回调（提交入口）可能在同步栈里销毁整个页面：回调返回后先看令牌，
    // 已析构就只做 return，不再碰任何成员。
    std::shared_ptr<bool> m_alive;
    UiTheme::Subscription m_theme;
};

}  // namespace agent::gui
