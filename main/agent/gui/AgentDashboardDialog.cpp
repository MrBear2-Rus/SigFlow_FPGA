#include "agent/gui/AgentDashboardDialog.h"

#include "agent/gui/AgentControls.h"
#include "agent/gui/AgentTokens.h"
#include "ui/UiTheme.h"

#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <utility>

namespace agent::gui {
namespace {

namespace T = UiTheme::AgentTypography;

/// wx 3.2.11 on MinGW builds wxString from narrow literals using the current
/// ANSI code page, so every UTF-8 source literal must be converted explicitly.
wxString U8(const char* text) {
    return wxString::FromUTF8(text);
}

wxString SourceText(const ModelSettings& settings) {
    switch (settings.source) {
        case ModelConfigSource::Environment: return U8("环境变量");
        case ModelConfigSource::Stored: return U8("本机加密存储");
        case ModelConfigSource::EmbeddedDev: return U8("开发构建嵌入");
        case ModelConfigSource::None: break;
    }
    return U8("未配置");
}

wxString ErrorText(const std::string& code) {
    if (code.empty()) return U8("未知错误");
    if (code == "invalid_api_key") return U8("API Key 为空或包含非法字符");
    if (code == "invalid_model") return U8("模型名为空或包含非法字符");
    if (code == "invalid_base_url") return U8("端点必须是 DeepSeek HTTPS 地址");
    if (code == "invalid_environment_config") return U8("环境变量配置不完整或非法");
    if (code == "unsupported_platform") return U8("当前平台不支持本机保存，请使用环境变量");
    if (code == "key_store_unavailable") return U8("本机配置存储不可用");
    if (code == "key_store_write_failed") return U8("本机配置写入失败，旧配置已保留");
    if (code == "key_store_invalid") return U8("本机配置无法解密或格式已损坏");
    if (code == "model_unconfigured") return U8("未配置模型 Key");
    if (code == "balance_unsupported_provider") return U8("当前模型提供方不支持余额查询");
    if (code == "balance_unauthorized") return U8("余额查询被拒绝（Key 无效或无权限）");
    if (code == "balance_unreachable") return U8("余额服务不可达");
    if (code == "balance_timeout") return U8("余额查询超时");
    if (code == "balance_unavailable") return U8("账户当前不可用");
    if (code == "balance_invalid_response") return U8("余额服务返回了无效响应");
    return wxString::FromUTF8(code.c_str());
}

wxString ErrorText(const std::string& code, const std::string& message) {
    const wxString mapped = ErrorText(code);
    if (message.empty()) return mapped;
    return mapped + U8("（") + wxString::FromUTF8(message.c_str()) + U8("）");
}

wxString JsonString(const Json::Value& value) {
    return value.isString() ? wxString::FromUTF8(value.asString().c_str()) : wxString();
}

wxString JsonNumber(const Json::Value& value) {
    if (value.isInt64()) {
        return wxString::Format("%lld", static_cast<long long>(value.asInt64()));
    }
    if (value.isUInt64()) {
        return wxString::Format("%llu", static_cast<unsigned long long>(value.asUInt64()));
    }
    if (value.isDouble()) return wxString::Format("%.0f", value.asDouble());
    return "0";
}

int JsonInt(const Json::Value& value, int fallback = 0) {
    return value.isInt() ? value.asInt() : fallback;
}

wxStaticText* MakeLabel(wxWindow* parent, const wxString& text, double px,
                        UiTheme::Weight weight, const wxColour& colour) {
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetFont(tokens::UiFontPx(parent, px, weight));
    label->SetForegroundColour(colour);
    label->SetBackgroundColour(tokens::SurfaceColour(tokens::Component::Button));
    return label;
}

void StyleInput(wxTextCtrl* control, const wxColour& background, const wxColour& text) {
    control->SetFont(tokens::UiFontPx(control, T::BodyPx));
    control->SetForegroundColour(text);
    control->SetBackgroundColour(background);
}

}  // namespace

AgentDashboardDialog::AgentDashboardDialog(wxWindow* parent, Host host)
    : wxDialog(parent, wxID_ANY, U8("模型与 API 仪表盘"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      host_(std::move(host)),
      m_pollTimer(this, wxID_HIGHEST + 701)
{
    BuildUi();
    ApplyTheme();
    Bind(wxEVT_TIMER, &AgentDashboardDialog::OnTimer, this, m_pollTimer.GetId());
    Bind(wxEVT_SIZE, &AgentDashboardDialog::OnSize, this);
    m_save->Bind(wxEVT_BUTTON, &AgentDashboardDialog::OnSave, this);
    m_clear->Bind(wxEVT_BUTTON, &AgentDashboardDialog::OnClear, this);
    m_refresh->Bind(wxEVT_BUTTON, &AgentDashboardDialog::OnRefresh, this);
    m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });
    SetEscapeId(wxID_CLOSE);
    m_theme = tokens::SubscribeTheme([this] { ApplyTheme(); });

    const ModelSettings settings = host_.settings ? host_.settings() : ModelSettings{};
    m_model->SetValue(wxString::FromUTF8(
        (settings.model.empty() ? "deepseek-chat" : settings.model).c_str()));
    m_baseUrl->SetValue(wxString::FromUTF8(
        (settings.baseUrl.empty() ? "https://api.deepseek.com" : settings.baseUrl).c_str()));
    RefreshStatus();
    if (host_.ready && host_.ready()) {
        RequestQuery();
    } else {
        SetQueryStatus(U8("Agent 服务未就绪；保存配置后会在重启完成时自动查询。"));
    }
}

AgentDashboardDialog::~AgentDashboardDialog() {
    // Unsubscribe before any wx child is destroyed.
    m_theme.Reset();
}

void AgentDashboardDialog::BuildUi() {
    const auto& colours = tokens::Colours();
    const int pad = tokens::CssPxToDip(this, 16);
    const int gap = tokens::CssPxToDip(this, 12);
    const int inner = tokens::CssPxToDip(this, 12);
    const wxColour cardSurface = tokens::SurfaceColour(tokens::Component::Button);

    SetSize(wxSize(820, 860));
    SetMinSize(wxSize(620, 540));
    SetBackgroundColour(colours.pageBg);

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    header->SetBackgroundColour(colours.pageBg);
    auto* headerSizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(header, wxID_ANY, U8("模型与 API 仪表盘"));
    title->SetFont(tokens::UiFontPx(header, T::BrandPx, T::Semibold));
    title->SetForegroundColour(colours.text1);
    title->SetBackgroundColour(colours.pageBg);
    headerSizer->Add(title, 0, wxEXPAND);
    auto* subtitle = new wxStaticText(
        header, wxID_ANY, U8("配置模型凭据，查看账户余额与本机 token 用量。"));
    subtitle->SetFont(tokens::UiFontPx(header, T::SmallBodyPx));
    subtitle->SetForegroundColour(colours.text2);
    subtitle->SetBackgroundColour(colours.pageBg);
    headerSizer->Add(subtitle, 0, wxEXPAND | wxTOP, tokens::CssPxToDip(this, 4));
    header->SetSizer(headerSizer);
    root->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    root->AddSpacer(gap);

    // The cards live in a scrolled area: the dialog is resizable, and a short
    // window must scroll instead of clipping the balance/usage cards.
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxBORDER_NONE | wxVSCROLL);
    m_scroll->SetName("agent-dashboard-scroll");
    m_scroll->SetBackgroundColour(colours.pageBg);
    m_scroll->SetScrollRate(0, tokens::CssPxToDip(this, 12));
    auto* body = new wxBoxSizer(wxVERTICAL);

    // ---- service status card ----
    auto* statusCard = new controls::AgentCardHost(m_scroll, wxID_ANY, U8("服务状态"));
    auto* statusBody = new wxPanel(statusCard, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxBORDER_NONE);
    auto* statusSizer = new wxBoxSizer(wxVERTICAL);
    m_status = new wxStaticText(statusBody, wxID_ANY, wxEmptyString);
    m_status->SetName("agent-dashboard-status");
    statusSizer->Add(m_status, 0, wxALL | wxEXPAND, inner);
    statusBody->SetSizer(statusSizer);
    statusCard->SetContent(statusBody);
    body->Add(statusCard, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    body->AddSpacer(gap);

    // ---- API key card ----
    auto* keyCard = new controls::AgentCardHost(m_scroll, wxID_ANY, U8("API Key"));
    auto* keyBody = new wxPanel(keyCard, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxBORDER_NONE);
    auto* keySizer = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, tokens::CssPxToDip(this, 8),
                                     tokens::CssPxToDip(this, 10));
    grid->AddGrowableCol(1, 1);

    const wxColour labelColour = colours.text2;
    grid->Add(MakeLabel(keyBody, U8("API Key"), T::LabelPx, T::Medium, labelColour), 0,
              wxALIGN_CENTER_VERTICAL);
    m_key = new wxTextCtrl(keyBody, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                           wxTE_PASSWORD | wxBORDER_NONE);
    m_key->SetName("agent-dashboard-key");
    m_key->SetHint(U8("留空则保留已保存的 Key"));
    m_key->SetMinSize(wxSize(-1, tokens::CssPxToDip(this, 34)));
    grid->Add(m_key, 1, wxEXPAND);

    grid->Add(MakeLabel(keyBody, U8("模型"), T::LabelPx, T::Medium, labelColour), 0,
              wxALIGN_CENTER_VERTICAL);
    m_model = new wxTextCtrl(keyBody, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                             wxBORDER_NONE);
    m_model->SetName("agent-dashboard-model");
    m_model->SetHint(U8("deepseek-chat"));
    m_model->SetMinSize(wxSize(-1, tokens::CssPxToDip(this, 34)));
    grid->Add(m_model, 1, wxEXPAND);

    grid->Add(MakeLabel(keyBody, U8("端点"), T::LabelPx, T::Medium, labelColour), 0,
              wxALIGN_CENTER_VERTICAL);
    m_baseUrl = new wxTextCtrl(keyBody, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxTE_READONLY | wxBORDER_NONE);
    m_baseUrl->SetName("agent-dashboard-endpoint");
    m_baseUrl->SetMinSize(wxSize(-1, tokens::CssPxToDip(this, 34)));
    grid->Add(m_baseUrl, 1, wxEXPAND);
    keySizer->Add(grid, 0, wxALL | wxEXPAND, inner);

    m_keyNote = new wxStaticText(
        keyBody, wxID_ANY, U8("Key 只以当前用户 DPAPI 密文保存；保存后重启 Agent 服务生效。"));
    m_keyNote->SetName("agent-dashboard-key-note");
    m_keyNote->SetFont(tokens::UiFontPx(keyBody, T::SmallBodyPx));
    m_keyNote->SetForegroundColour(colours.text2);
    m_keyNote->SetBackgroundColour(cardSurface);
    keySizer->Add(m_keyNote, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, inner);

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_save = new controls::AgentButton(keyBody, wxID_ANY, U8("保存并重启"),
                                       controls::Variant::Primary);
    m_save->SetAccessibleName(U8("保存 API Key 并重启 Agent 服务"));
    m_clear = new controls::AgentButton(keyBody, wxID_ANY, U8("清除本机 Key"),
                                        controls::Variant::Secondary);
    m_refresh = new controls::AgentButton(keyBody, wxID_ANY, U8("刷新"),
                                          controls::Variant::Ghost);
    actions->Add(m_save, 0, wxRIGHT, gap);
    actions->Add(m_clear, 0, wxRIGHT, gap);
    actions->AddStretchSpacer(1);
    actions->Add(m_refresh, 0);
    keySizer->Add(actions, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, inner);
    keyBody->SetSizer(keySizer);
    keyCard->SetContent(keyBody);
    body->Add(keyCard, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    body->AddSpacer(gap);

    // ---- balance card ----
    auto* balanceCard = new controls::AgentCardHost(m_scroll, wxID_ANY, U8("账户余额"));
    auto* balanceBody = new wxPanel(balanceCard, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxBORDER_NONE);
    auto* balanceSizer = new wxBoxSizer(wxVERTICAL);
    m_balance = new wxTextCtrl(balanceBody, wxID_ANY, U8("尚未查询。"), wxDefaultPosition,
                               wxDefaultSize,
                               wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
    m_balance->SetName("agent-dashboard-balance");
    m_balance->SetMinSize(wxSize(-1, tokens::CssPxToDip(this, 86)));
    balanceSizer->Add(m_balance, 1, wxALL | wxEXPAND, inner);
    balanceBody->SetSizer(balanceSizer);
    balanceCard->SetContent(balanceBody);
    body->Add(balanceCard, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    body->AddSpacer(gap);

    // ---- local usage card ----
    auto* usageCard = new controls::AgentCardHost(m_scroll, wxID_ANY, U8("本机用量"));
    auto* usageBody = new wxPanel(usageCard, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxBORDER_NONE);
    auto* usageSizer = new wxBoxSizer(wxVERTICAL);
    m_usage = new wxTextCtrl(usageBody, wxID_ANY, U8("尚未查询。"), wxDefaultPosition,
                             wxDefaultSize,
                             wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
    m_usage->SetName("agent-dashboard-usage");
    m_usage->SetMinSize(wxSize(-1, tokens::CssPxToDip(this, 150)));
    usageSizer->Add(m_usage, 1, wxALL | wxEXPAND, inner);
    usageBody->SetSizer(usageSizer);
    usageCard->SetContent(usageBody);
    body->Add(usageCard, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    body->AddSpacer(gap);

    m_scroll->SetSizer(body);
    m_scroll->FitInside();
    root->Add(m_scroll, 1, wxEXPAND);

    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    m_queryStatus = new wxStaticText(this, wxID_ANY, U8("打开仪表盘后会自动查询一次。"));
    m_queryStatus->SetName("agent-dashboard-query-status");
    m_queryStatus->SetFont(tokens::UiFontPx(this, T::SmallBodyPx));
    m_queryStatus->SetForegroundColour(colours.text2);
    m_queryStatus->SetBackgroundColour(colours.pageBg);
    footer->Add(m_queryStatus, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    m_close = new controls::AgentButton(this, wxID_CLOSE, U8("关闭"),
                                        controls::Variant::Ghost);
    footer->Add(m_close, 0);
    root->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

    SetSizer(root);
    Layout();
}

void AgentDashboardDialog::ApplyTheme() {
    const auto& colours = tokens::Colours();
    const wxColour cardSurface = tokens::SurfaceColour(tokens::Component::Button);
    SetBackgroundColour(colours.pageBg);

    if (m_status != nullptr) {
        m_status->SetFont(tokens::UiFontPx(this, T::BodyPx));
        m_status->SetForegroundColour(colours.text1);
        m_status->SetBackgroundColour(cardSurface);
    }
    if (m_keyNote != nullptr) {
        m_keyNote->SetFont(tokens::UiFontPx(this, T::SmallBodyPx));
        m_keyNote->SetForegroundColour(colours.text2);
        m_keyNote->SetBackgroundColour(cardSurface);
    }
    if (m_queryStatus != nullptr) {
        m_queryStatus->SetFont(tokens::UiFontPx(this, T::SmallBodyPx));
        m_queryStatus->SetForegroundColour(colours.text2);
        m_queryStatus->SetBackgroundColour(colours.pageBg);
    }
    for (auto* control : {m_key, m_model}) {
        if (control != nullptr) StyleInput(control, colours.glassInput, colours.text1);
    }
    if (m_baseUrl != nullptr) StyleInput(m_baseUrl, colours.sunken, colours.text2);
    if (m_balance != nullptr) {
        m_balance->SetFont(tokens::UiFontPx(this, T::BodyPx));
        m_balance->SetForegroundColour(colours.text1);
        m_balance->SetBackgroundColour(cardSurface);
    }
    if (m_usage != nullptr) {
        m_usage->SetFont(tokens::UiFontPx(this, T::SmallBodyPx));
        m_usage->SetForegroundColour(colours.text1);
        m_usage->SetBackgroundColour(cardSurface);
    }
    Layout();
    Refresh(false);
}

void AgentDashboardDialog::RefreshStatus() {
    const ModelSettings settings = host_.settings ? host_.settings() : ModelSettings{};
    const bool externallyProvided =
        settings.source == ModelConfigSource::None && !settings.configured() &&
        host_.modelAvailable && host_.modelAvailable();
    wxString text = host_.serviceText ? host_.serviceText() : U8("服务状态未知");
    text += U8("\nKey 来源：") +
            (externallyProvided ? U8("外部/未知") : SourceText(settings));
    if (!externallyProvided) text += settings.configured() ? U8("（已配置）") : U8("（未配置）");
    if (!settings.errorCode.empty()) text += U8(" · ") + ErrorText(settings.errorCode);
    if (settings.source == ModelConfigSource::Environment) {
        text += U8("\n注意：环境变量优先，本机保存的 Key 不会覆盖它。");
    }
    const int wrapWidth = std::max(240, GetClientSize().x - 100);
    if (text != m_lastStatusText || wrapWidth != m_lastWrapWidth) {
        m_lastStatusText = text;
        m_lastWrapWidth = wrapWidth;
        m_status->SetLabel(text);
        m_status->Wrap(wrapWidth);
        if (m_scroll != nullptr) m_scroll->FitInside();
        Layout();
    }
    if (!AgentModelSettings::Supported()) {
        m_keyNote->SetLabel(
            U8("当前平台不支持本机保存；请使用 SIGFLOW_EDU_MODEL_API_KEY 环境变量。"));
    }
    m_save->Enable(AgentModelSettings::Supported());
    m_clear->Enable(AgentModelSettings::Supported() &&
                    settings.source == ModelConfigSource::Stored);
}

void AgentDashboardDialog::SetQueryStatus(const wxString& text, bool force) {
    if (!force && text == m_lastQueryStatus) return;
    m_lastQueryStatus = text;
    m_queryStatus->SetLabel(text);
    m_queryStatus->Wrap(std::max(240, GetClientSize().x - 160));
    m_queryStatus->Refresh(false);
    Layout();
}

void AgentDashboardDialog::RequestQuery() {
    if (m_queryPending) return;
    if (!host_.ready || !host_.ready()) {
        SetQueryStatus(U8("Agent 服务未就绪，暂时无法查询用量与余额。"));
        return;
    }
    if (!host_.refresh) return;
    m_queryPending = true;
    m_queryUsageOk = false;
    m_queryBalanceOk = false;
    m_queryUsageError.clear();
    m_queryBalanceError.clear();
    m_refresh->Disable();
    SetQueryStatus(U8("正在查询用量与余额…"), true);
    if (!host_.refresh()) {
        m_queryPending = false;
        m_refresh->Enable(true);
        SetQueryStatus(U8("Agent 服务尚未就绪，请稍后重试。"), true);
    }
}

void AgentDashboardDialog::EndQuery() {
    m_queryPending = false;
    if (m_refresh != nullptr) m_refresh->Enable(true);
    // The capability probe usually lands while the query is in flight; refresh
    // the service card so it does not keep the pre-restart wording.
    RefreshStatus();
    if (m_queryUsageOk && m_queryBalanceOk) {
        SetQueryStatus(U8("用量与余额已更新。"), true);
    } else if (m_queryUsageOk) {
        SetQueryStatus(U8("用量已更新；余额查询失败：") + m_queryBalanceError, true);
    } else if (m_queryBalanceOk) {
        SetQueryStatus(U8("余额已更新；用量查询失败：") + m_queryUsageError, true);
    } else {
        SetQueryStatus(U8("查询失败：") + m_queryUsageError + U8(" · ") +
                           m_queryBalanceError, true);
    }
}

void AgentDashboardDialog::CancelQuery(const wxString& message) {
    m_queryPending = false;
    m_queryUsageOk = false;
    m_queryBalanceOk = false;
    if (m_refresh != nullptr) m_refresh->Enable(true);
    SetQueryStatus(message, true);
}

void AgentDashboardDialog::ShowUsage(const Json::Value& usage) {
    if (!usage.isObject()) {
        ShowUsageFailure("INVALID_RESPONSE", "用量响应格式不正确");
        return;
    }
    m_queryUsageOk = true;
    m_queryUsageError.clear();
    const Json::Value& totals = usage["totals"];
    wxString text;
    text += U8("累计调用：") + JsonNumber(totals["calls"]) + U8(" 次（失败 ") +
            JsonNumber(totals["failed_calls"]) + U8(" 次）\n");
    text += U8("累计 tokens：prompt ") + JsonNumber(totals["prompt_tokens"]) +
            U8(" · completion ") + JsonNumber(totals["completion_tokens"]) +
            U8(" · total ") + JsonNumber(totals["total_tokens"]) + "\n";

    long long windowCalls = 0;
    long long windowTokens = 0;
    if (usage["daily"].isArray()) {
        for (const auto& day : usage["daily"]) {
            windowCalls += day["calls"].isInt() ? day["calls"].asInt64() : 0;
            windowTokens += day["total_tokens"].isInt() ? day["total_tokens"].asInt64() : 0;
        }
    }
    text += U8("近 ") + JsonNumber(usage["days"]) + U8(" 天：") +
            wxString::Format("%lld", windowCalls) + U8(" 次，tokens ") +
            wxString::Format("%lld", windowTokens) + "\n";
    text += U8("统计起点：") +
            (usage["since"].isString() ? JsonString(usage["since"]) : U8("暂无记录"));
    if (JsonInt(usage["skipped"]) > 0) {
        text += U8("\n损坏记录已跳过：") + JsonNumber(usage["skipped"]);
    }
    if (JsonInt(totals["tokens_unavailable_calls"]) > 0) {
        text += U8("\n未返回 token 用量的调用：") +
                JsonNumber(totals["tokens_unavailable_calls"]);
    }
    text += U8("\n说明：仅统计本机自本版本上线后的模型调用，DeepSeek 不提供历史用量 API。");
    m_usage->SetValue(text);
    m_usage->Refresh(false);
}

void AgentDashboardDialog::ShowUsageFailure(const std::string& code,
                                            const std::string& message) {
    m_queryUsageOk = false;
    m_queryUsageError = ErrorText(code, message);
    m_usage->SetValue(U8("用量不可用：") + m_queryUsageError);
    m_usage->Refresh(false);
}

void AgentDashboardDialog::ShowBalance(const Json::Value& balance) {
    if (!balance.isObject()) {
        ShowBalanceFailure("INVALID_RESPONSE", "余额响应格式不正确");
        return;
    }
    m_queryBalanceOk = true;
    m_queryBalanceError.clear();
    wxString text;
    if (balance["available"].isBool() && balance["available"].asBool()) {
        text = U8("可用余额：") + JsonString(balance["total_balance"]) + " " +
               JsonString(balance["currency"]);
        text += U8("\n赠金：") + JsonString(balance["granted_balance"]) + U8(" · 充值：") +
                JsonString(balance["topped_up_balance"]);
    } else {
        text = U8("余额不可用：") +
               ErrorText(std::string(JsonString(balance["reason"]).utf8_str().data()));
    }
    text += U8("\n查询时间：") + JsonString(balance["checked_at"]);
    m_balance->SetValue(text);
    m_balance->Refresh(false);
}

void AgentDashboardDialog::ShowBalanceFailure(const std::string& code,
                                              const std::string& message) {
    m_queryBalanceOk = false;
    m_queryBalanceError = ErrorText(code, message);
    m_balance->SetValue(U8("余额不可用：") + m_queryBalanceError);
    m_balance->Refresh(false);
}

void AgentDashboardDialog::ShowSaveResult(bool ok, const wxString& message) {
    m_keyNote->SetLabel(message);
    m_keyNote->SetForegroundColour(ok ? tokens::Colours().brand : tokens::Colours().danger);
    m_keyNote->Refresh(false);
    if (ok) {
        if (host_.restartPossible && !host_.restartPossible()) {
            SetQueryStatus(U8("已保存；Agent 服务当前未启用，将在下次启动生效。"), true);
        } else {
            BeginRestartPolling();
        }
    }
    Layout();
}

void AgentDashboardDialog::BeginRestartPolling() {
    m_pollTicks = 0;
    m_restarting = true;
    m_pollTimer.Start(500);
    SetQueryStatus(U8("正在重启 Agent 服务…"), true);
}

void AgentDashboardDialog::OnSave(wxCommandEvent&) {
    const ModelSettings current = host_.settings ? host_.settings() : ModelSettings{};
    ModelSettings next;
    next.apiKey = std::string(m_key->GetValue().ToUTF8().data());
    if (next.apiKey.empty() && current.source == ModelConfigSource::Stored) {
        next.apiKey = current.apiKey;
    }
    next.model = std::string(m_model->GetValue().ToUTF8().data());
    next.baseUrl = std::string(m_baseUrl->GetValue().ToUTF8().data());
    std::string error;
    if (!host_.save || !host_.save(next, &error)) {
        ShowSaveResult(false, ErrorText(error));
        return;
    }
    m_key->Clear();
    ShowSaveResult(true, U8("已保存，正在重启 Agent 服务…"));
}

void AgentDashboardDialog::OnClear(wxCommandEvent&) {
    if (wxMessageBox(U8("清除后将以环境变量或未配置状态重启 Agent 服务。继续？"),
                     U8("清除本机 Key"), wxYES_NO | wxICON_QUESTION, this) != wxYES) {
        return;
    }
    std::string error;
    if (!host_.clear || !host_.clear(&error)) {
        ShowSaveResult(false, ErrorText(error));
        return;
    }
    m_key->Clear();
    ShowSaveResult(true, U8("已清除本机 Key，正在重启 Agent 服务…"));
}

void AgentDashboardDialog::OnRefresh(wxCommandEvent&) {
    RequestQuery();
}

void AgentDashboardDialog::OnTimer(wxTimerEvent&) {
    RefreshStatus();
    if (host_.ready && host_.ready()) {
        if (m_restarting) {
            m_restarting = false;
            m_keyNote->SetLabel(U8("已保存，Agent 服务已重启。"));
            m_keyNote->SetForegroundColour(tokens::Colours().brand);
            m_keyNote->Refresh(false);
        }
        m_pollTimer.Stop();
        RequestQuery();
        return;
    }
    const wxString state = host_.serviceState ? host_.serviceState() : wxString();
    if (state == "failed" || state == "disabled") {
        m_restarting = false;
        m_pollTimer.Stop();
        const bool failed = state == "failed";
        m_keyNote->SetLabel(failed ? U8("已保存，但 Agent 服务重启失败，请查看终端日志。")
                                   : U8("已保存；Agent 服务当前未启用。"));
        m_keyNote->SetForegroundColour(failed ? tokens::Colours().danger
                                              : tokens::Colours().text2);
        m_keyNote->Refresh(false);
        SetQueryStatus(failed ? U8("Agent 服务重启失败，请查看终端日志后重试。")
                              : U8("Agent 服务未启用；配置会在下次启动时生效。"),
                       true);
        return;
    }
    ++m_pollTicks;
    if (m_pollTicks > 40) {
        m_restarting = false;
        m_pollTimer.Stop();
        m_keyNote->SetLabel(U8("已保存，但 Agent 服务重启超时，请查看终端日志。"));
        m_keyNote->SetForegroundColour(tokens::Colours().danger);
        m_keyNote->Refresh(false);
        SetQueryStatus(U8("Agent 服务重启超时，请查看终端日志。"), true);
        RefreshStatus();
    }
}

void AgentDashboardDialog::OnSize(wxSizeEvent& event) {
    // The cards are custom-painted and the footer labels re-wrap: invalidate the
    // whole dialog so a live resize cannot leave stale pixels behind.
    Layout();
    if (m_scroll != nullptr) {
        m_scroll->FitInside();
        m_scroll->Refresh(false);
    }
    Refresh(false);
    event.Skip();
}

}  // namespace agent::gui
