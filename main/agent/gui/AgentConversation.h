#pragma once
//
// AgentConversation — AGUI-T12 消息流 / 受限富文本 / 结果完整展示
//
// 设计源：GUI/agent-gui/index.html H191–265（.aw-body/.msg/.resp/.card/.evid/.followups）、
//         spec.md §6.1/§12.2；CMP-016/CMP-018/INT-008/INT-014/VIS-005/VIS-006/LAY-007。
//
// 职责与边界：
//   * 只负责**投影**：宿主把 AgentViewModel 的 Messages()/Cards()/状态拷进 ConversationContent，
//     本组件不持有模型、不发请求、不读写配置、不执行 HTML/JS、不打开链接、不调用 shell。
//   * 正文按 AgentRichText 的受限 Markdown 子集渲染：粗体/行内代码/列表/围栏代码/管道表格；
//     HTML、脚本、URL、路径一律按纯文本显示。未识别语法原样保留，不静默截断。
//   * 消息与卡片是两组独立数据：消息按顺序显示，卡片作为"结果"分组显示；不把同一份内容
//     既当消息又当卡片重复渲染，也不为旧消息借当前 run 身份伪造来源。
//   * 用户/Agent 文本、卡片正文、证据、限制、元数据都用只读原生文本控件承载，支持选择与复制
//     （系统 Ctrl+C，用户主动动作）；表格/代码用等宽 + 横向滚动，不撑破页面。
//   * 滚动：内容变化前记录"是否贴底"和"顶部可见行锚点"；贴底则跟随新内容，向上阅读则保持
//     原锚点/视口。仅当内容签名变化时才重建/追加，草稿输入等无关通知不会重建消息。
//   * chips 只有调用方提供可信建议文字时才显示，点击只回调、不自动发送（T13 未接）。
#include "agent/gui/AgentRichText.h"
#include "agent/gui/AgentViewModel.h"
#include "ui/UiTheme.h"

#include <wx/font.h>
#include <wx/scrolwin.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class wxPanel;
class wxStaticText;
class wxTextCtrl;

namespace agent::gui {

/// 宿主导入的只读投影。默认构造 = 空会话（显示空状态）。
struct ConversationContent
{
    std::vector<vm::MessageView> messages;
    std::vector<vm::CardView> cards;
    /// 本轮结果的会话级字段（来自 Status()）；空字符串表示后端未返回。
    std::string modelUsed;
    std::string statusText;
    std::string reason;
    std::vector<std::string> limitations;
    /// 可信的建议文字（当前 v1 合同没有该字段，宿主默认留空；T13/T22 接通后再填）。
    std::vector<std::string> suggestions;
    vm::RequestState request = vm::RequestState::Idle;
    bool demo = false;
    std::vector<vm::StageView> stages;
    std::vector<std::string> stageScope;  ///< project/revision/generation/session/run/origin
    std::uint64_t omittedStages = 0;
    bool progressVisible = false;
    bool progressWaiting = false;
    vm::RunStatus runStatus = vm::RunStatus::Unknown;

    bool operator==(const ConversationContent&) const = default;
};

class AgentConversation final : public wxScrolledWindow
{
public:
    explicit AgentConversation(wxWindow* parent);
    ~AgentConversation() override;

    /// 返回 true 表示内容确实变了（重建或追加），false 表示完全一致（不触碰滚动）。
    bool SetContent(const ConversationContent& content);
    const ConversationContent& ContentData() const { return m_content; }

    /// chips 点击：只把可信建议文字交给回调，不发送、不改模型。
    void SetSuggestionHandler(std::function<void(const wxString&)> handler);
    /// View-only disclosure intent; survives notifications, resizing and reparenting.
    void ToggleStages();
    bool StagesExpanded() const { return m_stagesExpanded; }
    wxWindow* StageToggleWindow() const;
    int StageAnimationTimerId() const { return m_stageTimer.GetId(); }
    bool StageAnimationRunning() const { return m_stageTimer.IsRunning(); }

    // ---- 滚动契约（I02 / INT-014）----
    bool IsAtBottom() const;
    void ScrollToBottom();
    wxString TopAnchorId() const;   ///< 顶部可见行的 id（消息/卡片），空会话返回空串
    int TopAnchorOffset() const;
    void RestoreTopAnchor(const wxString& id, int offset = 0);
    /// 宽度变化后重排（保持顶部锚点与当前滚动位置）。
    void Relayout();

    // ---- 诊断（测试/夹具用；不是样式 API）----
    size_t RowCount() const { return m_rows.size(); }
    wxWindow* RowWindow(size_t index) const;
    wxString RowId(size_t index) const;
    size_t RebuildCount() const { return m_rebuilds; }
    size_t AppendCount() const { return m_appends; }
    bool IsEmptyState() const { return m_rows.empty(); }
    wxString TextOfRows() const;  ///< 所有行文本按顺序拼接（含空状态），用于"没有丢内容"核对
    static bool SameContent(const ConversationContent& a, const ConversationContent& b)
    {
        return a == b;
    }

private:
    enum class RowKind
    {
        Empty = 0,
        UserMessage,
        AgentMessage,
        Card,
        ResultMeta,
        Suggestions,
        Stages
    };

    struct RowText
    {
        wxTextCtrl* control = nullptr;
        bool wrap = true;
        bool mono = false;
    };

    struct Row
    {
        RowKind kind = RowKind::Empty;
        wxString id;
        wxPanel* panel = nullptr;
        std::vector<RowText> texts;
        std::vector<wxWindow*> buttons;
        wxWindow* header = nullptr;
        int y = 0;
        int height = 0;
    };

    void OnSize(wxSizeEvent& event);
    void OnPaint(wxPaintEvent& event);
    void ApplyTheme();
    void RebuildRows();
    void AppendMessageRows(size_t firstMessage);
    void ClearRows();
    Row* BuildMessageRow(const vm::MessageView& message, size_t ordinal);
    Row* BuildCardRow(const vm::CardView& card, size_t ordinal);
    Row* BuildResultMetaRow();
    Row* BuildSuggestionsRow();
    Row* BuildStagesRow();
    void UpdateStageHeader();
    void OnStagePulse(wxTimerEvent& event);
    Row* NewRow(RowKind kind, const wxString& id);
    wxTextCtrl* AddText(Row& row, const wxString& text, bool wrap, bool mono, bool bold);
    void AddRichBlocks(Row& row, const std::string& body);
    int MeasureText(const RowText& text, int width) const;
    void RelayoutRows(int width);
    void LayoutRow(Row& row, int width, int& y);
    void PlaceContent();
    int RowTop(int index) const;
    int CurrentScrollY() const;
    wxString AnchorIdAt(int scrollY, int* offset) const;
    void ScrollToAnchor(const wxString& id, int fallbackY, int offset = 0);
    void UpdateEmptyState();
    wxFont BodyFont() const;
    wxFont BoldFont() const;
    wxFont MonoFont() const;
    int LineHeight() const;

    ConversationContent m_content;
    std::vector<Row*> m_rows;
    wxPanel* m_canvas = nullptr;
    wxStaticText* m_emptyTitle = nullptr;
    wxStaticText* m_emptyHint = nullptr;
    std::function<void(const wxString&)> m_suggestion;
    size_t m_rebuilds = 0;
    size_t m_appends = 0;
    int m_width = 0;
    bool m_arranging = false;
    bool m_destroying = false;
    UiTheme::Subscription m_theme;
    std::optional<bool> m_stageIntent;
    bool m_stagesExpanded = true;
    unsigned m_stagePulse = 0;
    wxTimer m_stageTimer;
};

}  // namespace agent::gui
