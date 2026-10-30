#include "agent/gui/AgentConversation.h"

#include "agent/gui/AgentControls.h"
#include "agent/gui/AgentTokens.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace agent::gui {
namespace {

namespace S = UiTheme::AgentSpacing;
namespace M = UiTheme::AgentMetrics;
namespace T = UiTheme::AgentTypography;

wxString Text(const char* utf8) { return wxString::FromUTF8(utf8); }

wxString Text(const std::string& utf8) { return wxString::FromUTF8(utf8.data(), utf8.size()); }

int Dip(const wxWindow* window, int value) { return tokens::CssPxToDip(window, value); }

wxString OriginLabel(vm::Origin origin)
{
    switch (origin)
    {
    case vm::Origin::Live:
        return Text("真实数据");
    case vm::Origin::Demo:
        return Text("演示数据");
    case vm::Origin::Unknown:
    default:
        return Text("来源未知");
    }
}

wxString LevelLabel(const vm::Level& level)
{
    if (!level.bound()) return Text("级别未知");
    return Text("级别 ") + Text(level.ToText());
}

wxString StageStatusLabel(vm::RunStatus status)
{
    switch (status) {
    case vm::RunStatus::Succeeded: return Text("成功");
    case vm::RunStatus::Completed: return Text("已完成");
    case vm::RunStatus::Failed: return Text("失败");
    case vm::RunStatus::BudgetExceeded: return Text("预算超限");
    case vm::RunStatus::Running: return Text("运行中");
    case vm::RunStatus::Cancelled: return Text("已取消");
    case vm::RunStatus::WaitingStudent: return Text("等待学生");
    case vm::RunStatus::WaitingEvidence: return Text("等待证据");
    case vm::RunStatus::WaitingApproval: return Text("等待批准");
    case vm::RunStatus::RecoveryRequired: return Text("需要恢复");
    case vm::RunStatus::Degraded: return Text("降级结果");
    default: return Text("状态未知");
    }
}

// Rounded row background. User messages get the prototype's blue bubble; cards get
// the pre-composited glass surface; meta strips use the sunken surface.
class RowPanel final : public wxPanel
{
public:
    RowPanel(wxWindow* parent, int kind)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          m_kind(kind)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &RowPanel::OnPaint, this);
    }

    void SetKind(int kind)
    {
        m_kind = kind;
        Refresh(false);
    }

    bool AcceptsFocus() const override { return false; }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0) return;
        const auto& colours = tokens::Colours();
        const wxColour surface = tokens::SurfaceColour(tokens::Component::Card);
        dc.SetBackground(wxBrush(surface));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;

        if (m_kind == 1) {  // user bubble
            int top = size.y, bottom = 0, left = size.x, right = 0;
            for (auto* child : GetChildren()) {
                const wxRect rect = child->GetRect();
                top = std::min(top, rect.GetTop());
                bottom = std::max(bottom, rect.GetBottom());
                left = std::min(left, rect.GetLeft());
                right = std::max(right, rect.GetRight());
            }
            if (bottom < top || right < left) return;
            const int pad = Dip(this, S::BubblePadY);
            const wxRect bubble(left - pad, top - pad, right - left + 2 * pad,
                                bottom - top + 2 * pad);
            gc->SetBrush(wxBrush(colours.userBubbleBottom));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(bubble.x, bubble.y, bubble.width, bubble.height,
                                     Dip(this, M::CardRadius));
            return;
        }

        if (m_kind == 3) {  // card
            gc->SetPen(wxPen(colours.hairline, 1));
            gc->SetBrush(wxBrush(tokens::SurfaceColour(tokens::Component::Card)));
            gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0,
                                     Dip(this, M::CardRadius));
            return;
        }

        if (m_kind == 4) {  // result meta strip
            gc->SetBrush(wxBrush(colours.sunkenSoft));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(0, 0, size.x, size.y);
            return;
        }
    }

    int m_kind = 0;
};

// Text control with wrapping and read-only selection (system Ctrl+C by user action).
wxTextCtrl* MakeTextControl(wxWindow* parent, const wxString& value, bool wrap,
                            const wxFont& font, const wxColour& background,
                            const wxColour& foreground)
{
    long style = wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE | wxTE_NO_VSCROLL;
#ifdef __WXMSW__
    // Plain EDIT controls cannot apply per-range fonts on Windows.
    style |= wxTE_RICH2;
#endif
    if (!wrap) style |= wxTE_DONTWRAP | wxHSCROLL;
    auto* control = new wxTextCtrl(parent, wxID_ANY, value, wxDefaultPosition,
                                   wxDefaultSize, style);
    control->SetFont(font);
    control->SetBackgroundColour(background);
    control->SetForegroundColour(foreground);
    control->SetEditable(false);
    control->SetName(wxString::FromUTF8("消息文本"));
    return control;
}

const char* EmptyTitle()
{
    return "尚无对话";
}

const char* EmptyHint()
{
    return "可以先在下方整理你的问题。草稿会在本次窗口中保留，返回工作台也不会清空。";
}

}  // namespace

AgentConversation::AgentConversation(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxBORDER_NONE | wxVSCROLL | wxTAB_TRAVERSAL), m_stageTimer(this)
{
    SetName("agent-conversation");
    SetScrollRate(0, std::max(1, LineHeight()));
    SetBackgroundColour(tokens::SurfaceColour(tokens::Component::Card));
    wxFont hintFont = BodyFont();
    m_emptyTitle = new wxStaticText(this, wxID_ANY, Text(EmptyTitle()), wxDefaultPosition,
                                    wxDefaultSize, wxST_NO_AUTORESIZE);
    m_emptyTitle->SetName(Text("对话空状态"));
    m_emptyHint = new wxStaticText(this, wxID_ANY, Text(EmptyHint()), wxDefaultPosition,
                                   wxDefaultSize, wxST_NO_AUTORESIZE);
    m_emptyHint->SetName(Text("对话空状态说明"));
    m_emptyTitle->SetFont(tokens::UiFontPx(this, T::BrandPx, T::Semibold));
    m_emptyHint->SetFont(hintFont);
    m_emptyTitle->SetForegroundColour(tokens::Colours().text1);
    m_emptyHint->SetForegroundColour(tokens::Colours().text2);
    m_emptyTitle->SetBackgroundColour(GetBackgroundColour());
    m_emptyHint->SetBackgroundColour(GetBackgroundColour());

    Bind(wxEVT_SIZE, &AgentConversation::OnSize, this);
    Bind(wxEVT_TIMER, &AgentConversation::OnStagePulse, this, m_stageTimer.GetId());
    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
}

AgentConversation::~AgentConversation()
{
    m_destroying = true;
    m_stageTimer.Stop();
    m_theme.Reset();
    // Children are destroyed by wxWindow; only release the bookkeeping here (calling
    // Destroy() again for each row would schedule a second deletion).
    for (Row* row : m_rows) delete row;
    m_rows.clear();
}

wxFont AgentConversation::BodyFont() const
{
    return tokens::UiFontPx(this, T::BodyPx);
}

wxFont AgentConversation::BoldFont() const
{
    // wxMSW RichEdit maps range weights to a binary bold flag; weight 600 is
    // serialized as normal. Use native bold for actual emphasized text.
    return tokens::UiFontPx(this, T::BodyPx, T::Semibold).Bold();
}

wxFont AgentConversation::MonoFont() const
{
    return tokens::MonoFontPx(this, T::SmallBodyPx);
}

int AgentConversation::LineHeight() const
{
    return std::max(1, tokens::LineHeightDip(this, T::BodyPx, T::LineHeightBody));
}

void AgentConversation::ApplyTheme()
{
    if (m_destroying) return;
    SetBackgroundColour(tokens::SurfaceColour(tokens::Component::Card));
    if (m_emptyTitle != nullptr) {
        m_emptyTitle->SetFont(tokens::UiFontPx(this, T::BrandPx, T::Semibold));
        m_emptyTitle->SetForegroundColour(tokens::Colours().text1);
        m_emptyTitle->SetBackgroundColour(GetBackgroundColour());
    }
    if (m_emptyHint != nullptr) {
        m_emptyHint->SetFont(BodyFont());
        m_emptyHint->SetForegroundColour(tokens::Colours().text2);
        m_emptyHint->SetBackgroundColour(GetBackgroundColour());
    }
    RebuildRows();
    RelayoutRows(std::max(0, GetClientSize().x));
    Refresh(false);
}

void AgentConversation::SetSuggestionHandler(std::function<void(const wxString&)> handler)
{
    m_suggestion = std::move(handler);
}

AgentConversation::Row* AgentConversation::NewRow(RowKind kind, const wxString& id)
{
    auto* row = new Row();
    row->kind = kind;
    row->id = id;
    row->panel = new RowPanel(this, static_cast<int>(kind));
    row->panel->SetName(Text("消息行"));
    m_rows.push_back(row);
    return row;
}

wxTextCtrl* AgentConversation::AddText(Row& row, const wxString& text, bool wrap, bool mono,
                                       bool bold)
{
    const bool user = row.kind == RowKind::UserMessage;
    const wxColour background =
        user ? tokens::Colours().userBubbleBottom
             : tokens::SurfaceColour(tokens::Component::Card);
    const wxColour foreground = user ? tokens::Colours().textOnAccent : tokens::Colours().text1;
    wxFont font = mono ? MonoFont() : (bold ? BoldFont() : BodyFont());
    auto* control = MakeTextControl(row.panel, text, wrap, font, background, foreground);
    row.texts.push_back(RowText{control, wrap, mono});
    return control;
}

void AgentConversation::AddRichBlocks(Row& row, const std::string& body)
{
    const rich::Document document = rich::Parse(body);
    if (document.blocks.empty()) {
        if (!body.empty()) AddText(row, Text(body), true, false, false);
        return;
    }
    for (const rich::Block& block : document.blocks) {
        switch (block.kind) {
        case rich::BlockKind::Paragraph: {
            wxString value;
            std::vector<std::pair<int, int>> boldRanges;
            std::vector<std::pair<int, int>> codeRanges;
            for (const rich::Run& run : block.runs) {
                const int start = static_cast<int>(value.length());
                value += Text(run.text);
                const int length = static_cast<int>(value.length()) - start;
                if (run.bold && length > 0) boldRanges.push_back({start, length});
                if (run.code && length > 0) codeRanges.push_back({start, length});
            }
            if (value.empty()) break;
            wxTextCtrl* control = AddText(row, value, true, false, false);
            const auto applyFont = [](wxTextCtrl* target, int start, int length,
                                      const wxFont& font) {
                wxTextAttr attribute;
                attribute.SetFont(font);
                target->SetStyle(start, start + length, attribute);
            };
            for (const auto& range : boldRanges) {
                applyFont(control, range.first, range.second, BoldFont());
            }
            for (const auto& range : codeRanges) {
                applyFont(control, range.first, range.second, MonoFont());
            }
            break;
        }
        case rich::BlockKind::BulletList:
        case rich::BlockKind::OrderedList: {
            // One line per item, joined without a trailing newline: a trailing "\n"
            // would leave an empty last line and add a phantom row of height.
            wxString value;
            int index = 1;
            for (const rich::ListItem& item : block.items) {
                if (!value.empty()) value += "\n";
                if (block.kind == rich::BlockKind::BulletList) value += Text("- ");
                if (block.kind == rich::BlockKind::OrderedList) {
                    value += wxString::Format("%d. ", index++);
                }
                for (const rich::Run& run : item.runs) value += Text(run.text);
            }
            if (!value.empty()) AddText(row, value, true, false, false);
            break;
        }
        case rich::BlockKind::CodeBlock:
            AddText(row, Text(block.text), false, true, false);
            break;
        case rich::BlockKind::Table: {
            wxString value;
            const auto join = [&](const rich::TableRow& tableRow) {
                wxString line;
                for (size_t i = 0; i < tableRow.cells.size(); ++i) {
                    if (i > 0) line += Text(" | ");
                    for (const rich::Run& run : tableRow.cells[i].runs) line += Text(run.text);
                }
                return line;
            };
            const auto appendLine = [&](const wxString& line) {
                if (!value.empty()) value += "\n";
                value += line;
            };
            if (block.hasHeader) appendLine(join(block.header));
            for (const rich::TableRow& tableRow : block.rows) appendLine(join(tableRow));
            if (!value.empty()) AddText(row, value, false, true, false);
            break;
        }
        }
    }
}

AgentConversation::Row* AgentConversation::BuildMessageRow(const vm::MessageView& message,
                                                           size_t ordinal)
{
    const bool user = message.role == "user";
    const wxString id = message.messageId.empty()
                            ? wxString::Format("message:%zu", ordinal)
                            : Text(message.messageId);
    Row* row = NewRow(user ? RowKind::UserMessage : RowKind::AgentMessage, id);
    AddRichBlocks(*row, message.text);

    if (!message.limitations.empty()) {
        wxString limits = Text("限制：");
        for (size_t i = 0; i < message.limitations.size(); ++i) {
            if (i > 0) limits += Text("；");
            limits += Text(message.limitations[i]);
        }
        AddText(*row, limits, true, false, false);
    }

    wxString meta = Text("来源：") + OriginLabel(message.origin);
    if (!message.role.empty() && message.role != "user" && message.role != "assistant") {
        meta += Text(" · 角色：") + Text(message.role);
    }
    if (!message.rawState.empty()) meta += Text(" · 状态：") + Text(message.rawState);
    if (!message.runId.empty()) meta += Text(" · run：") + Text(message.runId);
    AddText(*row, meta, true, false, false);
    return row;
}

AgentConversation::Row* AgentConversation::BuildCardRow(const vm::CardView& card,
                                                        size_t ordinal)
{
    const wxString id = card.cardId.empty() ? wxString::Format("card:%zu", ordinal)
                                            : Text(card.cardId);
    Row* row = NewRow(RowKind::Card, id);
    const wxString title = card.title.empty() ? Text("（无标题卡片）") : Text(card.title);
    AddText(*row, title, true, false, true);

    if (!card.body.empty()) AddRichBlocks(*row, card.body);

    wxString meta = Text("类型：") +
                    (card.kind.empty() ? Text("未知类型") : Text(card.kind));
    meta += Text(" · 来源：") +
            (card.source.empty() ? Text("未知") : Text(card.source));
    meta += Text(" · ") + OriginLabel(card.origin);
    if (card.level.bound()) meta += Text(" · ") + LevelLabel(card.level);
    if (card.stateVersion.known()) meta += Text(" · 版本 ") + Text(card.stateVersion.ToText());
    if (card.expired) meta += Text(" · 已过期");
    if (!card.issueId.empty()) meta += Text(" · 问题 ") + Text(card.issueId);
    if (!card.cardId.empty()) meta += Text(" · 卡片 ") + Text(card.cardId);
    if (!card.rawState.empty()) meta += Text(" · 状态：") + Text(card.rawState);
    AddText(*row, meta, true, false, false);

    for (const vm::EvidenceView& evidence : card.evidence) {
        wxString line = Text("证据 · ") + Text(evidence.KindLabel());
        const std::string reference = evidence.ReferenceText();
        if (!reference.empty()) line += Text(" · ") + Text(reference);
        if (!evidence.note.empty()) line += Text("：") + Text(evidence.note);
        line += Text(" · ") + OriginLabel(evidence.origin);
        AddText(*row, line, true, false, false);
        for (const std::string& limitation : evidence.limitations) {
            AddText(*row, Text("证据限制：") + Text(limitation), true, false, false);
        }
    }
    for (const std::string& courseRef : card.courseRefs) {
        AddText(*row, Text("课程引用：") + Text(courseRef), true, false, false);
    }
    for (const std::string& limitation : card.limitations) {
        AddText(*row, Text("限制：") + Text(limitation), true, false, false);
    }
    return row;
}

AgentConversation::Row* AgentConversation::BuildResultMetaRow()
{
    Row* row = NewRow(RowKind::ResultMeta, Text("result-meta"));
    wxString text = Text("本轮结果");
    text += Text(" · model_used=") +
            (m_content.modelUsed.empty() ? Text("未知") : Text(m_content.modelUsed));
    if (!m_content.statusText.empty()) text += Text(" · 状态：") + Text(m_content.statusText);
    if (m_content.demo) text += Text(" · 演示数据");
    AddText(*row, text, true, false, false);
    if (!m_content.reason.empty())
        AddText(*row, Text("原因：") + Text(m_content.reason), true, false, false);
    for (const auto& limitation : m_content.limitations)
        AddText(*row, Text("本轮限制：") + Text(limitation), true, false, false);
    return row;
}

AgentConversation::Row* AgentConversation::BuildSuggestionsRow()
{
    Row* row = NewRow(RowKind::Suggestions, Text("suggestions"));
    for (const std::string& suggestion : m_content.suggestions) {
        if (suggestion.empty()) continue;
        auto* button = new controls::AgentButton(row->panel, wxID_ANY, Text(suggestion),
                                                 controls::Variant::Ghost);
        button->Bind(wxEVT_BUTTON, [this, suggestion](wxCommandEvent&) {
            // Explicit user action only: hand the trusted text to the host, never send.
            if (m_suggestion) m_suggestion(Text(suggestion));
        });
        row->buttons.push_back(button);
    }
    return row;
}

AgentConversation::Row* AgentConversation::BuildStagesRow()
{
    Row* row = NewRow(RowKind::Stages, Text("public-stages"));
    auto* header = new controls::AgentButton(row->panel, wxID_ANY, wxEmptyString,
                                            controls::Variant::Ghost);
    row->header = header;
    header->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleStages(); });
    if (m_content.demo) AddText(*row, Text("演示数据：阶段未实际执行"), true, false, true);
    if (m_content.stages.empty()) {
        AddText(*row, Text(m_content.progressWaiting ?
            "正在等待运行结果；服务未提供公开阶段明细。" :
            "服务未提供公开阶段明细；请以运行结果和限制为准。"), true, false, false);
    }
    for (const auto& stage : m_content.stages) {
        AddText(*row, Text("事件状态：") + StageStatusLabel(stage.status) + Text(" · ") + Text(stage.title),
                true, false, true);
        AddText(*row, Text(stage.summary), true, false, false);
        AddText(*row, OriginLabel(stage.origin) + Text(" · 事件 ") + Text(stage.id), true, false, false);
    }
    if (m_content.omittedStages > 0)
        AddText(*row, Text("仅保留最近 64 条公开事件；更早未展示条数：") +
                      wxString::Format("%llu", static_cast<unsigned long long>(m_content.omittedStages)),
                true, false, false);
    UpdateStageHeader();
    return row;
}

wxWindow* AgentConversation::StageToggleWindow() const
{
    for (const auto* row : m_rows) if (row->kind == RowKind::Stages) return row->header;
    return nullptr;
}

void AgentConversation::UpdateStageHeader()
{
    auto* header = StageToggleWindow();
    if (!header) return;
    wxString label = Text(m_stagesExpanded ? "收起执行摘要" : "展开执行摘要");
    // Put the status before the count so a narrow dock does not hide a failure.
    label += Text(" · ") + StageStatusLabel(m_content.runStatus);
    label += wxString::Format(" · %zu", m_content.stages.size());
    label += Text(" 条公开事件");
    if (m_content.demo) label += Text(" · 演示");
    if (m_content.progressWaiting) {
        label += Text(" · 等待");
        for (unsigned i = 0; i <= m_stagePulse % 3; ++i) label += ".";
    }
    header->SetLabel(label);
    static_cast<controls::AgentButton*>(header)->SetAccessibleName(label);
    header->SetToolTip(Text("仅公开事件摘要，不展示内部推理。可用回车或空格展开、收起。"));
}

void AgentConversation::OnStagePulse(wxTimerEvent&)
{
    // Typing dots are view-only. Never synthesize stages, status or percentages.
    if (m_destroying || !m_content.progressWaiting || !IsShownOnScreen()) return;
    ++m_stagePulse;
    UpdateStageHeader();
}

void AgentConversation::ToggleStages()
{
    if (m_destroying || !StageToggleWindow()) return;
    const int scrollY = CurrentScrollY();
    int offset = 0;
    const auto anchor = AnchorIdAt(scrollY, &offset);
    m_stagesExpanded = !m_stagesExpanded;
    m_stageIntent = m_stagesExpanded;
    UpdateStageHeader();
    RelayoutRows(std::max(0, GetClientSize().x));
    if (!anchor.empty()) ScrollToAnchor(anchor, scrollY, offset);
}

void AgentConversation::ClearRows()
{
    for (Row* row : m_rows) {
        if (row->panel != nullptr) {
            row->panel->Destroy();
        }
        delete row;
    }
    m_rows.clear();
}

void AgentConversation::RebuildRows()
{
    ClearRows();
    ++m_rebuilds;
    size_t ordinal = 0;
    for (const vm::MessageView& message : m_content.messages) {
        BuildMessageRow(message, ordinal++);
    }
    if (m_content.progressVisible) BuildStagesRow();
    if (!m_content.cards.empty() || !m_content.reason.empty() ||
        !m_content.limitations.empty()) {
        BuildResultMetaRow();
        size_t cardOrdinal = 0;
        for (const vm::CardView& card : m_content.cards) {
            BuildCardRow(card, cardOrdinal++);
        }
    }
    if (!m_content.suggestions.empty()) BuildSuggestionsRow();
}

void AgentConversation::AppendMessageRows(size_t firstMessage)
{
    ++m_appends;
    for (size_t index = firstMessage; index < m_content.messages.size(); ++index) {
        BuildMessageRow(m_content.messages[index], index);
    }
}

bool AgentConversation::SetContent(const ConversationContent& content)
{
    if (m_destroying || content == m_content) return false;
    const bool wasEmpty = m_content.messages.empty() && m_content.cards.empty();
    const int scrollY = CurrentScrollY();
    int anchorOffset = 0;
    const wxString anchor = AnchorIdAt(scrollY, &anchorOffset);
    const bool atBottom = IsAtBottom();
    const size_t oldMessages = m_content.messages.size();
    const size_t oldCards = m_content.cards.size();

    // Appending at the end is valid only while the rows contain messages alone.
    // Result and suggestion groups must stay after every message.
    bool appendOnly = !wasEmpty && m_content.cards.empty() &&
                      !m_content.progressVisible && !content.progressVisible &&
                      m_content.suggestions.empty() && m_content.reason.empty() &&
                      m_content.limitations.empty() &&
                      oldCards == content.cards.size() &&
                      content.messages.size() > oldMessages &&
                      content.cards == m_content.cards &&
                      content.suggestions == m_content.suggestions;
    if (appendOnly) {
        for (size_t i = 0; i < oldMessages; ++i) {
            if (!(content.messages[i] == m_content.messages[i])) {
                appendOnly = false;
                break;
            }
        }
    }
    const bool metaChanged = content.modelUsed != m_content.modelUsed ||
                             content.statusText != m_content.statusText ||
                             content.reason != m_content.reason ||
                             content.limitations != m_content.limitations ||
                             content.demo != m_content.demo;
    if (appendOnly && metaChanged) appendOnly = false;

    if (content.stageScope != m_content.stageScope) m_stageIntent.reset();
    m_stagesExpanded = m_stageIntent.value_or(!vm::IsTerminal(content.runStatus));
    m_content = content;
    if (content.progressVisible && content.progressWaiting) {
        if (!m_stageTimer.IsRunning()) m_stageTimer.Start(400);
    } else {
        m_stageTimer.Stop();
        m_stagePulse = 0;
    }
    if (appendOnly) {
        AppendMessageRows(oldMessages);
    } else {
        RebuildRows();
    }
    UpdateEmptyState();
    RelayoutRows(std::max(0, GetClientSize().x));

    if (atBottom) {
        ScrollToBottom();
    } else if (!anchor.empty()) {
        ScrollToAnchor(anchor, scrollY, anchorOffset);
    } else {
        int unitX = 0;
        int unitY = 0;
        GetScrollPixelsPerUnit(&unitX, &unitY);
        Scroll(0, std::max(0, scrollY) / std::max(1, unitY));
    }
    return true;
}

void AgentConversation::UpdateEmptyState()
{
    const bool empty = m_rows.empty();
    if (m_emptyTitle != nullptr) m_emptyTitle->Show(empty);
    if (m_emptyHint != nullptr) m_emptyHint->Show(empty);
}

int AgentConversation::MeasureText(const RowText& text, int width) const
{
    if (text.control == nullptr) return 0;
    const int contentWidth = std::max(10, width);
    if (!text.wrap) {
        int lines = 1;
        const wxString value = text.control->GetValue();
        for (wxString::const_iterator it = value.begin(); it != value.end(); ++it) {
            if (*it == '\n') ++lines;
        }
        const int lineHeight = std::max(1, tokens::LineHeightDip(this, T::SmallBodyPx,
                                                                 T::LineHeightHint));
        return lines * lineHeight;
    }
    // Force the control to wrap at the target width, then ask it for the wrapped count.
    text.control->SetSize(0, 0, contentWidth, std::max(1, LineHeight()) * 400);
    int lines = text.control->GetNumberOfLines();
    if (lines < 1) lines = 1;
    return lines * LineHeight();
}

void AgentConversation::LayoutRow(Row& row, int width, int& y)
{
    const int padX = Dip(this, S::ThreadPadX);
    const int padY = Dip(this, S::ThreadPadY);
    const int gap = Dip(this, S::ThreadGap);
    row.y = y;
    int cursor = padY;
    const bool user = row.kind == RowKind::UserMessage;
    int textWidth = std::max(0, width - 2 * padX);
    if (user) textWidth = std::min(textWidth, std::max(Dip(this, 120), (width * 4) / 5));
    if (row.header != nullptr) {
        row.header->SetSize(padX, cursor, textWidth, Dip(this, M::NarrowButtonHeight));
        cursor += Dip(this, M::NarrowButtonHeight) + gap;
    }
    for (RowText& text : row.texts) {
        if (text.control == nullptr) continue;
        if (row.kind == RowKind::Stages && !m_stagesExpanded) {
            text.control->Hide();
            continue;
        }
        const int height = MeasureText(text, textWidth);
        const int x = user ? std::max(padX, width - padX - textWidth) : padX;
        text.control->SetSize(x, cursor, textWidth, height);
        text.control->Show(true);
        cursor += height + gap;
    }
    if (!row.buttons.empty()) {
        int x = padX;
        int lineHeight = 0;
        const int buttonHeight = Dip(this, M::NarrowButtonHeight);
        for (wxWindow* button : row.buttons) {
            const int buttonWidth = std::min(std::max(Dip(this, 64), button->GetBestSize().x),
                                             std::max(0, width - 2 * padX));
            if (x + buttonWidth > width - padX && x > padX) {
                x = padX;
                cursor += lineHeight + Dip(this, S::ToolsGap);
                lineHeight = 0;
            }
            button->SetSize(x, cursor, buttonWidth, buttonHeight);
            button->Show(true);
            x += buttonWidth + Dip(this, S::ToolsGap);
            lineHeight = std::max(lineHeight, buttonHeight);
        }
        cursor += lineHeight + gap;
    }
    row.height = std::max(Dip(this, M::HeaderHeight), cursor + padY);
    // Rows are laid out in virtual coordinates, but a child window is positioned in
    // client coordinates and the scrolled window shifts the existing children when
    // the view moves. Translating by the current view start keeps a re-layout at a
    // non-zero scroll position from displacing every row by that offset.
    int panelX = 0;
    int panelY = row.y;
    CalcScrolledPosition(0, row.y, &panelX, &panelY);
    row.panel->SetSize(panelX, panelY, width, row.height);
    // Advance the virtual cursor: rows must stack in document order, otherwise the
    // next row would be laid over this one and the virtual height would never grow.
    y = row.y + row.height + gap;
}

void AgentConversation::RelayoutRows(int width)
{
    if (m_destroying) return;
    m_width = std::max(0, width);
    m_arranging = true;
    int y = Dip(this, S::ThreadPadY);
    for (Row* row : m_rows) {
        LayoutRow(*row, m_width, y);
    }
    for (size_t index = 0; index < m_rows.size(); ++index) {
        m_rows[index]->panel->Show(true);
    }
    // Empty state labels are placed on the scrolled window itself.
    if (m_rows.empty() && m_emptyTitle != nullptr && m_emptyHint != nullptr) {
        const int pad = std::min(Dip(this, S::ThreadPadX), m_width / 2);
        const int textWidth = std::max(0, m_width - 2 * pad);
        // Wrap with real font metrics: the copy is a Chinese sentence, and a native
        // static control would either clip it or report a one-line height for the
        // several lines it actually draws.
        const tokens::WrappedText titleWrap = tokens::WrapTextForWidth(
            this, m_emptyTitle->GetFont(), Text(EmptyTitle()), textWidth);
        const tokens::WrappedText hintWrap = tokens::WrapTextForWidth(
            this, m_emptyHint->GetFont(), Text(EmptyHint()), textWidth);
        m_emptyTitle->SetLabel(titleWrap.text);
        m_emptyHint->SetLabel(hintWrap.text);
        const int titleHeight = titleWrap.height;
        const int hintHeight = hintWrap.height;
        // Same virtual→client translation as the rows; the empty state normally sits
        // at view start 0, but a themed relayout must not depend on that.
        int titleX = pad;
        int titleY = Dip(this, S::ThreadPadY);
        CalcScrolledPosition(titleX, titleY, &titleX, &titleY);
        int hintX = pad;
        int hintY = Dip(this, S::ThreadPadY) + titleHeight + Dip(this, S::ThreadGap);
        CalcScrolledPosition(hintX, hintY, &hintX, &hintY);
        m_emptyTitle->SetSize(titleX, titleY, textWidth, titleHeight);
        m_emptyHint->SetSize(hintX, hintY, textWidth, hintHeight);
        y = Dip(this, S::ThreadPadY) + titleHeight + Dip(this, S::ThreadGap) + hintHeight +
            Dip(this, S::ThreadPadY);
    }
    const int virtualHeight = std::max(GetClientSize().y, y);
    SetVirtualSize(m_width, virtualHeight);
    for (Row* row : m_rows) {
        row->panel->Refresh(false);
    }
    m_arranging = false;
}

void AgentConversation::OnSize(wxSizeEvent& event)
{
    if (!m_arranging && !m_destroying) {
        const int width = std::max(0, GetClientSize().x);
        if (width != m_width) {
            const int scrollY = CurrentScrollY();
            int offset = 0;
            const wxString anchor = AnchorIdAt(scrollY, &offset);
            RelayoutRows(width);
            if (!anchor.empty()) {
                ScrollToAnchor(anchor, scrollY, offset);
            } else {
                int unitX = 0;
                int unitY = 0;
                GetScrollPixelsPerUnit(&unitX, &unitY);
                Scroll(0, std::max(0, scrollY) / std::max(1, unitY));
            }
        }
    }
    event.Skip();
}

void AgentConversation::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(tokens::SurfaceColour(tokens::Component::Card)));
    dc.Clear();
}

void AgentConversation::Relayout()
{
    const int scrollY = CurrentScrollY();
    int offset = 0;
    const wxString anchor = AnchorIdAt(scrollY, &offset);
    const bool atBottom = IsAtBottom();
    RelayoutRows(std::max(0, GetClientSize().x));
    if (atBottom) ScrollToBottom();
    else if (!anchor.empty()) ScrollToAnchor(anchor, scrollY, offset);
}

int AgentConversation::CurrentScrollY() const
{
    int x = 0;
    int y = 0;
    const_cast<AgentConversation*>(this)->GetViewStart(&x, &y);
    int unitX = 0;
    int unitY = 0;
    const_cast<AgentConversation*>(this)->GetScrollPixelsPerUnit(&unitX, &unitY);
    return y * std::max(1, unitY);
}

bool AgentConversation::IsAtBottom() const
{
    const int virtualHeight = GetVirtualSize().y;
    if (virtualHeight <= 0) return true;
    return CurrentScrollY() + GetClientSize().y >= virtualHeight - 2;
}

void AgentConversation::ScrollToBottom()
{
    const int virtualHeight = GetVirtualSize().y;
    int unitX = 0;
    int unitY = 0;
    GetScrollPixelsPerUnit(&unitX, &unitY);
    const int unit = std::max(1, unitY);
    const int target = std::max(0, virtualHeight - GetClientSize().y);
    Scroll(0, (target + unit - 1) / unit);
}

wxString AgentConversation::AnchorIdAt(int scrollY, int* offset) const
{
    if (offset != nullptr) *offset = 0;
    for (const Row* row : m_rows) {
        if (row->y + row->height > scrollY) {
            if (offset != nullptr) *offset = std::max(0, scrollY - row->y);
            return row->id;
        }
    }
    return wxString();
}

wxString AgentConversation::TopAnchorId() const
{
    return AnchorIdAt(CurrentScrollY(), nullptr);
}

int AgentConversation::TopAnchorOffset() const
{
    int offset = 0;
    AnchorIdAt(CurrentScrollY(), &offset);
    return offset;
}

void AgentConversation::RestoreTopAnchor(const wxString& id, int offset)
{
    if (m_destroying || id.empty()) return;
    // The anchor id names the row that was at the top before a reflow; scrolling it
    // back to the top restores the reading position instead of jumping to the end.
    ScrollToAnchor(id, CurrentScrollY(), offset);
}

void AgentConversation::ScrollToAnchor(const wxString& id, int fallbackY, int offset)
{
    int unitX = 0;
    int unitY = 0;
    GetScrollPixelsPerUnit(&unitX, &unitY);
    const int unit = std::max(1, unitY);
    for (const Row* row : m_rows) {
        if (row->id == id) {
            // Round the scroll position up: scroll positions are multiples of the
            // unit, so flooring would leave the view inside the previous row and the
            // anchor id would change on the next read.
            const int within = std::clamp(offset, 0, std::max(0, row->height - 1));
            Scroll(0, std::max(0, row->y + within + unit - 1) / unit);
            return;
        }
    }
    Scroll(0, std::max(0, fallbackY) / unit);
}

wxWindow* AgentConversation::RowWindow(size_t index) const
{
    if (index >= m_rows.size()) return nullptr;
    return m_rows[index]->panel;
}

wxString AgentConversation::RowId(size_t index) const
{
    if (index >= m_rows.size()) return wxString();
    return m_rows[index]->id;
}

wxString AgentConversation::TextOfRows() const
{
    wxString text;
    for (const Row* row : m_rows) {
        if (row->header) text += row->header->GetLabel() + "\n";
        for (const RowText& item : row->texts) {
            if (item.control != nullptr) text += item.control->GetValue() + "\n";
        }
        for (wxWindow* button : row->buttons) {
            if (auto* agentButton = dynamic_cast<controls::AgentButton*>(button)) {
                text += agentButton->GetLabel() + "\n";
            }
        }
    }
    if (m_rows.empty()) {
        if (m_emptyTitle != nullptr) text += m_emptyTitle->GetLabel() + "\n";
        if (m_emptyHint != nullptr) text += m_emptyHint->GetLabel();
    }
    return text;
}

}  // namespace agent::gui
