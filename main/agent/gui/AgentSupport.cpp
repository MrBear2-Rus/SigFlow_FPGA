#include "agent/gui/AgentSupport.h"
#include "agent/gui/AgentControls.h"
#include "agent/gui/AgentTokens.h"
#include <wx/textctrl.h>
#include <algorithm>
#include <set>

namespace agent::gui::support {
namespace {
wxString U8(const std::string& s) { return wxString::FromUTF8(s.data(), s.size()); }
std::string Known(const std::string& s) { return s.empty() ? "未知" : s; }
std::string Origin(vm::Origin o)
{
    return o == vm::Origin::Demo ? "演示数据" : o == vm::Origin::Live ? "真实数据" : "来源未知";
}
std::string Freshness(const vm::CardView& card, const std::string& current)
{
    if (card.expired) return "后端标记已过期";
    if (current.empty() || card.receiptRevision.empty()) return "新鲜度未知（缺接收时绑定版本）";
    if (current != card.receiptRevision) return "本地版本已变化：需复核（非后端 expired 标记）";
    return "接收时绑定版本与当前一致；证据新鲜度未核验";
}
std::string Limits(const std::vector<std::string>& limits)
{
    std::string s;
    for (const auto& limit : limits) s += "\n限制：" + limit;
    return s;
}
}

Projection Project(const vm::AgentViewModel& model, const std::string& currentRevision)
{
    Projection out;
    const std::string revision = model.DemoMode() ? model.Status().revision :
        !currentRevision.empty() ? currentRevision : model.BoundRevision();
    std::set<std::string> ids;
    for (const auto& card : model.Cards()) if (!card.issueId.empty()) ids.insert(card.issueId);
    out.issueSummary = "当前问题 · 开放数未知";
    std::string overview = model.DemoMode() ? "演示数据：不是真实问题或诊断\n" : "";
    overview += "当前版本：" + Known(revision) + "\n开放数：未知（合同未提供问题聚合）";
    overview += "\n已提供问题 ID：";
    if (ids.empty()) overview += "未知（尚未接入问题信息）";
    else { for (const auto& id : ids) overview += "\n" + id; }
    overview += "\n问题类型：未知\n逻辑对象：未知\n快照：未知\n来源 Job：未知";
    out.issues.push_back({"当前问题", overview});
    for (const auto& card : model.Cards()) {
        std::string text = "问题 ID：" + Known(card.issueId) + "\n卡片 ID：" + Known(card.cardId);
        text += "\n来源：" + Origin(card.origin) + " · " + Known(card.source);
        text += "\n接收时版本：" + Known(card.receiptRevision) + "\n" + Freshness(card, revision);
        text += "\n卡片状态：" + Known(card.rawState) + "\n摘要：" + Known(card.body) + Limits(card.limitations);
        out.issues.push_back({card.title.empty() ? "未命名教学卡片" : card.title, text});
    }
    size_t count = 0;
    for (const auto& card : model.Cards()) count += card.evidence.size();
    out.evidenceSummary = "证据链 · " + std::to_string(count) + " 条";
    if (model.DemoMode()) out.evidence.push_back({"演示数据", "示例引用未读取文件，未执行工具。"});
    // U2 committed state 的验证进度：只读摘要，明确不等于已验证/不触发执行。
    if (model.Verification().present)
    {
        out.evidence.push_back({"验证进度 · 后端状态",
                                vm::VerificationSummaryText(model.Verification())});
    }
    // U2 prediction：只读摘要；不追加任何提交/执行入口。
    if (model.Prediction().present)
    {
        out.evidence.push_back({"预测摘要 · 后端状态",
                                vm::PredictionSummaryText(model.Prediction())});
    }
    for (int group = 0; group < 3; ++group) {
        const std::string label = group == 0 ? "观察" : group == 1 ? "假设" : "分类未知";
        out.evidence.push_back({label, group == 1 ? "假设仍待验证，不能当作观察或验收结论。" :
                                  group == 2 ? "未知 kind 保留原文，不归入观察。" : "观察表示来源标签，不自动等于已验证事实。"});
        size_t inGroup = 0;
        for (const auto& card : model.Cards()) for (const auto& ev : card.evidence) {
            const int evGroup = ev.IsObservation() ? 0 : ev.IsHypothesis() ? 1 : 2;
            if (evGroup != group) continue;
            ++inGroup;
            const bool eligible = ev.origin == vm::Origin::Live && card.origin == vm::Origin::Live &&
                !model.DemoMode() && !card.expired && !card.receiptRevision.empty() &&
                card.receiptRevision == revision && !ev.ref.empty() && !model.BoundProjectId().empty();
            const int location = static_cast<int>(out.locations.size());
            out.locations.push_back({ev.ref, card.cardId, model.BoundProjectId(), card.receiptRevision, ev.origin, eligible});
            // N4：来源/位置只显示受限引用（source_id/job_id 与已核验的行号范围），
            // 不解析文件、不拼路径；没有结构化元数据时退回原始 ref（旧行为）。
            std::string text = "内容：" + Known(ev.note) + "\n来源 / 位置：" + Known(ev.ReferenceText());
            text += "\n证据类型：" + Known(ev.kind) + "\n" + Origin(ev.origin);
            text += "\n卡片：" + Known(card.cardId) + "\n接收时版本：" + Known(card.receiptRevision);
            text += "\n" + Freshness(card, revision) + Limits(ev.limitations) + Limits(card.limitations);
            out.evidence.push_back({label + " · " + std::to_string(inGroup), text, location});
        }
        if (!inGroup) out.evidence.push_back({"暂无" + label, "未提供此类证据。"});
    }
    return out;
}

AgentSupportSection::AgentSupportSection(wxWindow* parent, bool evidence)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_evidence(evidence)
{
    SetName(U8(evidence ? "证据链只读内容" : "当前问题只读内容"));
    SetMinSize(wxSize(0, 0));
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        if (!m_arranging && GetClientSize().x != m_width) Arrange(GetClientSize().x);
        e.Skip();
    });
    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });
    ApplyTheme();
}

bool AgentSupportSection::SetProjection(const Projection& data)
{
    if (m_loaded && data == m_data) return false;
    m_loaded = true;
    m_data = data;
    Rebuild();
    return true;
}

void AgentSupportSection::Rebuild()
{
    for (auto& item : m_items) { if (item.action) item.action->Destroy(); item.text->Destroy(); }
    m_items.clear();
    ++m_rebuilds;
    const auto& blocks = m_evidence ? m_data.evidence : m_data.issues;
    for (const auto& block : blocks) {
        for (bool heading : {true, false}) {
            long style = wxTE_MULTILINE | wxTE_READONLY | wxTE_NO_VSCROLL | wxBORDER_NONE;
#ifdef __WXMSW__
            style |= wxTE_RICH2;
#endif
            auto* text = new wxTextCtrl(this, wxID_ANY, U8(heading ? block.title : block.text),
                                       wxDefaultPosition, wxDefaultSize, style);
            text->SetName(U8(heading ? "分区小标题" : "只读来源与摘要"));
            m_items.push_back({text, heading});
        }
        if (block.location >= 0) {
            auto* button = new controls::AgentButton(this, wxID_ANY, U8("定位来源"), controls::Variant::Ghost);
            const auto index = static_cast<size_t>(block.location);
            const bool eligible = index < m_data.locations.size() && m_data.locations[index].eligible;
            // 默认边界：没有受信 NavigationBridge 时保持禁用并给出固定原因；
            // 这里绝不安装假 handler，也不访问文件。
            const vm::LocationActionView gate = vm::ProjectLocationAction(m_locate != nullptr, eligible);
            if (!gate.available) button->SetDisabledReason(U8(gate.reason));
            button->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { RequestLocation(index); });
            m_items.back().action = button;
        }
    }
    ApplyTheme();
}

void AgentSupportSection::ApplyTheme()
{
    SetBackgroundColour(tokens::SurfaceColour(tokens::Component::Card));
    for (auto& item : m_items) {
        item.text->SetBackgroundColour(GetBackgroundColour());
        item.text->SetForegroundColour(tokens::Colours().text1);
        auto font = tokens::UiFontPx(this, UiTheme::AgentTypography::SmallBodyPx);
        item.text->SetFont(item.heading ? font.Bold() : font);
    }
    Arrange(GetClientSize().x);
}

int AgentSupportSection::Arrange(int width)
{
    if (m_arranging) return GetMinSize().y;
    m_arranging = true;
    m_width = std::max(0, width);
    const int pad = std::min(tokens::CssPxToDip(this, 12), m_width / 2);
    const int inner = std::max(0, m_width - 2 * pad);
    const int line = std::max(1, tokens::LineHeightDip(this, UiTheme::AgentTypography::SmallBodyPx,
                                                   UiTheme::AgentTypography::LineHeightBody));
    const int gap = tokens::CssPxToDip(this, 8);
    int y = gap;
    for (auto& item : m_items) {
        item.text->SetSize(pad, y, inner, line * 400);
        const int height = std::max(1, item.text->GetNumberOfLines()) * line;
        item.text->SetSize(pad, y, inner, height);
        y += height + gap;
        if (item.action) {
            const int buttonHeight = tokens::CssPxToDip(this, 34);
            item.action->SetSize(pad, y, inner, buttonHeight);
            y += buttonHeight + gap;
        }
    }
    SetMinSize(wxSize(0, y));
    m_arranging = false;
    return y;
}

wxString AgentSupportSection::TextContent() const
{
    wxString text;
    for (const auto& item : m_items) text += item.text->GetValue() + "\n";
    return text;
}

void AgentSupportSection::SetLocateHandler(std::function<bool(const LocationTarget&)> handler)
{
    m_locate = std::move(handler);
    Rebuild();
}

bool AgentSupportSection::RequestLocation(size_t index)
{
    if (!m_locate || index >= m_data.locations.size() || !m_data.locations[index].eligible) return false;
    const auto handler = m_locate;
    const auto target = m_data.locations[index];
    return handler(target); // Trusted T25 resolver must validate the opaque ref; no access here.
}
} // namespace agent::gui::support
