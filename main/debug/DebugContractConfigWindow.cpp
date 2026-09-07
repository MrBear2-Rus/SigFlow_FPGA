#include "DebugContractConfigWindow.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tokenzr.h>

namespace {

std::string ToUtf8(const wxString& value)
{
    const wxScopedCharBuffer buffer = value.ToUTF8();
    return buffer.data() ? std::string(buffer.data()) : std::string();
}

wxString FromUtf8(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

wxString NormalizeProjectDirectory(const wxString& value)
{
    if (value.IsEmpty()) return value;
    wxFileName path(value);
    if (path.GetFullName().Lower() == wxT("sigflow.project")) return path.GetPath();
    return value;
}

bool ParseUnsigned(const wxString& text, std::uint64_t& value)
{
    const std::string input = ToUtf8(text);
    if (input.empty()) return false;
    try {
        std::size_t parsed = 0;
        const unsigned long long result = std::stoull(input, &parsed, 10);
        if (parsed != input.size()) return false;
        value = static_cast<std::uint64_t>(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadProjectMetadata(const wxString& projectPath, wxString& topModule,
                         wxString& targetProfile)
{
    if (projectPath.IsEmpty()) return false;
    const wxString projectDirectory = NormalizeProjectDirectory(projectPath);
    if (projectDirectory.IsEmpty()) return false;
    const wxString configPath = projectDirectory + wxFileName::GetPathSeparator() +
                                wxT("sigflow.project");
    if (!wxFileExists(configPath)) return false;
    wxFile file(configPath, wxFile::read);
    if (!file.IsOpened()) return false;
    wxString text;
    if (!file.ReadAll(&text)) return false;
    const std::string json = ToUtf8(text);
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json.data(), json.data() + json.size(), &root, &errors)) return false;
    if (root["build"]["top_module"].isString()) {
        topModule = FromUtf8(root["build"]["top_module"].asString());
    }
    if (root["fpga"]["target_profile"].isString()) {
        targetProfile = FromUtf8(root["fpga"]["target_profile"].asString());
    }
    return true;
}

std::vector<sigflow::debug::DebugProbe> ParseProbes(const wxString& text, wxString& error)
{
    std::vector<sigflow::debug::DebugProbe> probes;
    wxStringTokenizer lines(text, wxT("\n"), wxTOKEN_RET_EMPTY_ALL);
    unsigned lineNumber = 0;
    std::set<std::string> ids;
    while (lines.HasMoreTokens()) {
        ++lineNumber;
        wxString line = lines.GetNextToken();
        line.Trim(true).Trim(false);
        if (line.IsEmpty() || line.StartsWith(wxT("#"))) continue;
        wxStringTokenizer tokens(line, wxT(" \t"));
        if (!tokens.HasMoreTokens()) continue;
        const wxString path = tokens.GetNextToken();
        unsigned width = 1;
        wxString clockDomain;
        if (tokens.HasMoreTokens()) {
            std::uint64_t parsedWidth = 0;
            if (!ParseUnsigned(tokens.GetNextToken(), parsedWidth) || parsedWidth == 0 ||
                parsedWidth > 32) {
                error = wxString::Format(wxT("第 %u 行的位宽无效"), lineNumber);
                return {};
            }
            width = static_cast<unsigned>(parsedWidth);
        }
        if (tokens.HasMoreTokens()) clockDomain = tokens.GetNextToken();
        if (tokens.HasMoreTokens()) {
            error = wxString::Format(wxT("第 %u 行格式错误：路径 [位宽] [时钟域]"), lineNumber);
            return {};
        }
        std::string id = ToUtf8(path);
        const std::size_t separator = id.find_last_of("./");
        if (separator != std::string::npos) id = id.substr(separator + 1);
        for (char& character : id) {
            if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') {
                character = '_';
            }
        }
        if (id.empty() || std::isdigit(static_cast<unsigned char>(id.front()))) id = "probe";
        const std::string baseId = id;
        unsigned suffix = 2;
        while (!ids.insert(id).second) id = baseId + "_" + std::to_string(suffix++);
        sigflow::debug::DebugProbe probe;
        probe.id = id;
        probe.path = ToUtf8(path);
        probe.width = width;
        probe.clockDomain = ToUtf8(clockDomain);
        probes.push_back(std::move(probe));
    }
    return probes;
}

int ChoiceIndex(wxChoice* choice, const wxString& label)
{
    const int index = choice->FindString(label);
    return index == wxNOT_FOUND ? 0 : index;
}

}

DebugContractConfigWindow::DebugContractConfigWindow(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, wxT("FPGA · debug_contract"), wxDefaultPosition,
              wxSize(900, 760), wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
{
    BuildUi();
    LoadDefaultContract();
}

void DebugContractConfigWindow::BuildUi()
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(new wxStaticText(this, wxID_ANY, wxT("Debug Contract")), 0,
                wxALL | wxALIGN_CENTER_VERTICAL, 8);
    pathLabel_ = new wxStaticText(this, wxID_ANY, wxT("未打开工程"));
    header->Add(pathLabel_, 1, wxALL | wxALIGN_CENTER_VERTICAL, 8);
    root->Add(header, 0, wxEXPAND);

    auto* notebook = new wxNotebook(this, wxID_ANY);
    auto* general = new wxPanel(notebook);
    auto* generalSizer = new wxBoxSizer(wxVERTICAL);
    auto* generalGrid = new wxFlexGridSizer(0, 2, 7, 10);
    generalGrid->AddGrowableCol(1, 1);
    auto addText = [&](const wxString& label, wxTextCtrl*& control, const wxString& value) {
        generalGrid->Add(new wxStaticText(general, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        control = new wxTextCtrl(general, wxID_ANY, value);
        generalGrid->Add(control, 1, wxEXPAND);
    };
    addText(wxT("目标 Profile"), targetProfileCtrl_, wxT("tang-nano-9k@1.0.0"));
    addText(wxT("顶层模块"), topModuleCtrl_, wxEmptyString);
    addText(wxT("采样时钟信号"), sampleClockCtrl_, wxT("clk"));
    addText(wxT("时钟频率 Hz"), sampleFrequencyCtrl_, wxT("27000000"));
    generalSizer->Add(generalGrid, 0, wxALL | wxEXPAND, 10);
    auto* probeBox = new wxStaticBoxSizer(wxVERTICAL, general, wxT("探针列表"));
    probeEditor_ = new wxTextCtrl(general, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxTE_MULTILINE | wxTE_DONTWRAP);
    probeEditor_->SetHint(wxT("每行：信号路径 [位宽] [时钟域]\n例如：dut.state 4 clk_a"));
    probeBox->Add(probeEditor_, 1, wxALL | wxEXPAND, 6);
    generalSizer->Add(probeBox, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    general->SetSizer(generalSizer);
    notebook->AddPage(general, wxT("基本与探针"), true);

    auto* triggerPage = new wxPanel(notebook);
    auto* triggerSizer = new wxBoxSizer(wxVERTICAL);
    auto* triggerGrid = new wxFlexGridSizer(0, 2, 8, 10);
    triggerGrid->AddGrowableCol(1, 1);
    triggerGrid->Add(new wxStaticText(triggerPage, wxID_ANY, wxT("触发模式")), 0, wxALIGN_CENTER_VERTICAL);
    triggerChoice_ = new wxChoice(triggerPage, wxID_ANY);
    triggerChoice_->Append(wxT("none"));
    triggerChoice_->Append(wxT("mask_equal"));
    triggerChoice_->Append(wxT("edge_rising"));
    triggerChoice_->Append(wxT("edge_falling"));
    triggerChoice_->Append(wxT("state_stall"));
    triggerChoice_->Append(wxT("handshake_timeout"));
    triggerGrid->Add(triggerChoice_, 1, wxEXPAND);
    auto addTriggerText = [&](const wxString& label, wxTextCtrl*& control) {
        triggerGrid->Add(new wxStaticText(triggerPage, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        control = new wxTextCtrl(triggerPage, wxID_ANY);
        triggerGrid->Add(control, 1, wxEXPAND);
    };
    addTriggerText(wxT("Mask（十六进制）"), triggerMaskCtrl_);
    addTriggerText(wxT("Value（十六进制）"), triggerValueCtrl_);
    triggerGrid->Add(new wxStaticText(triggerPage, wxID_ANY, wxT("N / 连续周期")), 0, wxALIGN_CENTER_VERTICAL);
    triggerCyclesCtrl_ = new wxSpinCtrl(triggerPage, wxID_ANY, wxT("8"), wxDefaultPosition,
                                        wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 8);
    triggerGrid->Add(triggerCyclesCtrl_, 1, wxEXPAND);
    addTriggerText(wxT("hs_valid 信号路径"), hsValidCtrl_);
    addTriggerText(wxT("hs_ready 信号路径"), hsReadyCtrl_);
    triggerSizer->Add(triggerGrid, 0, wxALL | wxEXPAND, 10);
    triggerSizer->Add(new wxStaticText(triggerPage, wxID_ANY,
        wxT("state_stall / handshake_timeout 会写入 trigger.intent；mask_equal 与边沿模式使用 mask/value。")),
        0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
    triggerPage->SetSizer(triggerSizer);
    notebook->AddPage(triggerPage, wxT("触发"), false);

    auto* capturePage = new wxPanel(notebook);
    auto* captureSizer = new wxBoxSizer(wxVERTICAL);
    auto* captureGrid = new wxFlexGridSizer(0, 2, 8, 10);
    captureGrid->AddGrowableCol(1, 1);
    auto addSpin = [&](const wxString& label, wxSpinCtrl*& control, int value, int maximum) {
        captureGrid->Add(new wxStaticText(capturePage, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        control = new wxSpinCtrl(capturePage, wxID_ANY, wxString::Format(wxT("%d"), value),
                                 wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, maximum, value);
        captureGrid->Add(control, 1, wxEXPAND);
    };
    addSpin(wxT("采样深度"), depthCtrl_, 1024, 10000000);
    addSpin(wxT("Pretrigger samples"), pretriggerCtrl_, 512, 10000000);
    addSpin(wxT("Decimation"), decimationCtrl_, 1, 1000000);
    captureSizer->Add(captureGrid, 0, wxALL | wxEXPAND, 10);
    capturePage->SetSizer(captureSizer);
    notebook->AddPage(capturePage, wxT("采样"), false);

    auto* transportPage = new wxPanel(notebook);
    auto* transportSizer = new wxBoxSizer(wxVERTICAL);
    auto* transportGrid = new wxFlexGridSizer(0, 2, 8, 10);
    transportGrid->AddGrowableCol(1, 1);
    transportGrid->Add(new wxStaticText(transportPage, wxID_ANY, wxT("UART 协议")), 0, wxALIGN_CENTER_VERTICAL);
    protocolChoice_ = new wxChoice(transportPage, wxID_ANY);
    protocolChoice_->Append(wxT("minimal"));
    protocolChoice_->Append(wxT("full"));
    transportGrid->Add(protocolChoice_, 1, wxEXPAND);
    transportGrid->Add(new wxStaticText(transportPage, wxID_ANY, wxT("波特率")), 0, wxALIGN_CENTER_VERTICAL);
    baudChoice_ = new wxChoice(transportPage, wxID_ANY);
    for (const wxString& baud : {wxT("115200"), wxT("460800"), wxT("921600"),
                                 wxT("1500000"), wxT("3000000")}) baudChoice_->Append(baud);
    transportGrid->Add(baudChoice_, 1, wxEXPAND);
    auto addTransportText = [&](const wxString& label, wxTextCtrl*& control) {
        transportGrid->Add(new wxStaticText(transportPage, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        control = new wxTextCtrl(transportPage, wxID_ANY);
        transportGrid->Add(control, 1, wxEXPAND);
    };
    addTransportText(wxT("逻辑 TX 端口"), txPortCtrl_);
    addTransportText(wxT("逻辑 RX 端口"), rxPortCtrl_);
    auto addPin = [&](const wxString& label, wxSpinCtrl*& control) {
        transportGrid->Add(new wxStaticText(transportPage, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        control = new wxSpinCtrl(transportPage, wxID_ANY, wxT("0"), wxDefaultPosition,
                                 wxDefaultSize, wxSP_ARROW_KEYS, 0, 200, 0);
        transportGrid->Add(control, 1, wxEXPAND);
    };
    addPin(wxT("TX 管脚"), txPinCtrl_);
    addPin(wxT("RX 管脚"), rxPinCtrl_);
    addPin(wxT("复位管脚"), rstPinCtrl_);
    transportGrid->Add(new wxStaticText(transportPage, wxID_ANY, wxT("同步校准")), 0, wxALIGN_CENTER_VERTICAL);
    syncEnabledCtrl_ = new wxCheckBox(transportPage, wxID_ANY, wxT("启用 55 AA 同步校准"));
    transportGrid->Add(syncEnabledCtrl_, 1, wxEXPAND);
    transportSizer->Add(transportGrid, 0, wxALL | wxEXPAND, 10);
    transportPage->SetSizer(transportSizer);
    notebook->AddPage(transportPage, wxT("UART"), false);
    root->Add(notebook, 1, wxLEFT | wxRIGHT | wxEXPAND, 8);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* open = new wxButton(this, wxID_ANY, wxT("打开"));
    auto* reload = new wxButton(this, wxID_ANY, wxT("重新加载"));
    auto* validate = new wxButton(this, wxID_ANY, wxT("校验"));
    auto* saveAs = new wxButton(this, wxID_ANY, wxT("保存为..."));
    auto* save = new wxButton(this, wxID_SAVE, wxT("保存"));
    buttons->Add(open, 0, wxALL, 6);
    buttons->Add(reload, 0, wxALL, 6);
    buttons->Add(validate, 0, wxALL, 6);
    buttons->AddStretchSpacer();
    buttons->Add(saveAs, 0, wxALL, 6);
    buttons->Add(save, 0, wxALL, 6);
    root->Add(buttons, 0, wxEXPAND);
    statusLabel_ = new wxStaticText(this, wxID_ANY, wxT(""));
    root->Add(statusLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);
    SetSizer(root);
    open->Bind(wxEVT_BUTTON, &DebugContractConfigWindow::OnOpen, this);
    reload->Bind(wxEVT_BUTTON, &DebugContractConfigWindow::OnReload, this);
    validate->Bind(wxEVT_BUTTON, &DebugContractConfigWindow::OnValidate, this);
    saveAs->Bind(wxEVT_BUTTON, &DebugContractConfigWindow::OnSaveAs, this);
    save->Bind(wxEVT_BUTTON, &DebugContractConfigWindow::OnSave, this);
}

void DebugContractConfigWindow::SetProjectContext(const wxString& projectPath)
{
    const wxString normalizedPath = NormalizeProjectDirectory(projectPath);
    if (projectPath_ == normalizedPath && !normalizedPath.IsEmpty()) return;
    projectPath_ = normalizedPath;
    LoadDefaultContract();
}

void DebugContractConfigWindow::Reload()
{
    LoadDefaultContract();
}

void DebugContractConfigWindow::SetSavedHandler(std::function<void(const wxString&)> handler)
{
    savedHandler_ = std::move(handler);
}

wxString DebugContractConfigWindow::DefaultContractPath() const
{
    return projectPath_.IsEmpty() ? wxString() :
        wxFileName(projectPath_, wxT("debug-contract.json")).GetFullPath();
}

void DebugContractConfigWindow::LoadDefaultContract()
{
    contractFilePath_.clear();
    contract_ = sigflow::debug::DebugContract();
    contract_.sessionId = sigflow::debug::NewDebugSessionId();
    contract_.sampleClock.signal = "clk";
    contract_.sampleClock.frequencyHz = 27000000;
    contract_.capture.depth = 1024;
    contract_.capture.pretriggerSamples = 512;
    contract_.capture.decimation = 1;
    contract_.transport.protocol = "minimal";
    contract_.transport.baud = 921600;
    contract_.transport.txPort = "dbg_tx";
    contract_.transport.rxPort = "dbg_rx";
    contract_.transport.txPin = 17;
    contract_.transport.rxPin = 18;
    contract_.transport.syncEnabled = true;
    wxString topModule;
    wxString targetProfile;
    if (ReadProjectMetadata(projectPath_, topModule, targetProfile)) {
        if (!topModule.IsEmpty()) contract_.topModule = ToUtf8(topModule);
        if (!targetProfile.IsEmpty()) contract_.targetProfile = ToUtf8(targetProfile);
    }
    if (contract_.targetProfile.empty()) contract_.targetProfile = "tang-nano-9k@1.0.0";
    if (!projectPath_.IsEmpty() && wxFileExists(DefaultContractPath())) {
        LoadFromPath(DefaultContractPath());
        return;
    }
    ApplyContractToUi();
    SetStatus(projectPath_.IsEmpty() ? wxT("请先打开 SigFlow 工程") :
              wxT("尚未找到契约，请填写探针后保存"));
}

void DebugContractConfigWindow::LoadFromPath(const wxString& path)
{
    if (path.IsEmpty()) {
        SetStatus(wxT("未指定 Debug Contract 文件。"), true);
        return;
    }
    if (!wxFileExists(path)) {
        SetStatus(wxT("文件不存在：") + path, true);
        return;
    }
    wxFile file(path, wxFile::read);
    wxString content;
    if (!file.IsOpened() || !file.ReadAll(&content)) {
        SetStatus(wxT("无法读取：") + path, true);
        return;
    }
    sigflow::debug::DebugContract parsed;
    std::string error;
    if (!parsed.ParseJson(ToUtf8(content), error)) {
        SetStatus(wxT("JSON 解析失败：") + FromUtf8(error), true);
        return;
    }
    contract_ = std::move(parsed);
    contractFilePath_ = path;
    ApplyContractToUi();
    SetStatus(wxT("已加载：") + path);
}

void DebugContractConfigWindow::ApplyContractToUi()
{
    targetProfileCtrl_->SetValue(FromUtf8(contract_.targetProfile));
    topModuleCtrl_->SetValue(FromUtf8(contract_.topModule));
    sampleClockCtrl_->SetValue(FromUtf8(contract_.sampleClock.signal));
    sampleFrequencyCtrl_->SetValue(wxString::Format(wxT("%llu"),
        static_cast<unsigned long long>(contract_.sampleClock.frequencyHz)));
    wxString probes;
    for (const auto& probe : contract_.probes) {
        probes += FromUtf8(probe.path);
        if (probe.width != 1) probes += wxString::Format(wxT(" %u"), probe.width);
        if (!probe.clockDomain.empty()) probes += wxT(" ") + FromUtf8(probe.clockDomain);
        probes += wxT("\n");
    }
    probeEditor_->SetValue(probes);
    int triggerIndex = 0;
    if (contract_.trigger.intentKind == "state_stall") triggerIndex = 4;
    else if (contract_.trigger.intentKind == "handshake_timeout") triggerIndex = 5;
    else triggerIndex = triggerChoice_->FindString(FromUtf8(contract_.trigger.kind));
    triggerChoice_->SetSelection(triggerIndex == wxNOT_FOUND ? 0 : triggerIndex);
    triggerMaskCtrl_->SetValue(FromUtf8(contract_.trigger.mask));
    triggerValueCtrl_->SetValue(FromUtf8(contract_.trigger.value));
    triggerCyclesCtrl_->SetValue(8);
    hsValidCtrl_->SetValue(FromUtf8(contract_.trigger.hsValidPath));
    hsReadyCtrl_->SetValue(FromUtf8(contract_.trigger.hsReadyPath));
    depthCtrl_->SetValue(static_cast<int>(contract_.capture.depth));
    pretriggerCtrl_->SetValue(static_cast<int>(contract_.capture.pretriggerSamples));
    decimationCtrl_->SetValue(static_cast<int>(contract_.capture.decimation));
    protocolChoice_->SetSelection(ChoiceIndex(protocolChoice_, FromUtf8(contract_.transport.protocol)));
    baudChoice_->SetSelection(ChoiceIndex(baudChoice_, wxString::Format(wxT("%u"), contract_.transport.baud)));
    txPortCtrl_->SetValue(FromUtf8(contract_.transport.txPort));
    rxPortCtrl_->SetValue(FromUtf8(contract_.transport.rxPort));
    txPinCtrl_->SetValue(contract_.transport.txPin);
    rxPinCtrl_->SetValue(contract_.transport.rxPin);
    rstPinCtrl_->SetValue(contract_.transport.rstPin);
    syncEnabledCtrl_->SetValue(contract_.transport.syncEnabled);
    const wxString displayedPath = contractFilePath_.IsEmpty()
        ? (projectPath_.IsEmpty() ? wxString(wxT("未打开工程")) : DefaultContractPath())
        : contractFilePath_;
    pathLabel_->SetLabel(displayedPath);
    Layout();
}

bool DebugContractConfigWindow::BuildContract(sigflow::debug::DebugContract& contract,
                                              wxString& error) const
{
    if (projectPath_.IsEmpty()) {
        error = wxT("请先打开 SigFlow 工程。");
        return false;
    }
    contract = contract_;
    contract.schemaVersion = "1.0";
    contract.sessionId = contract.sessionId.empty() ? sigflow::debug::NewDebugSessionId() : contract.sessionId;
    contract.targetProfile = ToUtf8(targetProfileCtrl_->GetValue());
    contract.topModule = ToUtf8(topModuleCtrl_->GetValue());
    contract.sampleClock.signal = ToUtf8(sampleClockCtrl_->GetValue());
    std::uint64_t frequency = 0;
    if (!ParseUnsigned(sampleFrequencyCtrl_->GetValue(), frequency) || frequency == 0) {
        error = wxT("时钟频率必须是正整数 Hz。");
        return false;
    }
    contract.sampleClock.frequencyHz = frequency;
    contract.probes = ParseProbes(probeEditor_->GetValue(), error);
    if (!error.IsEmpty()) return false;
    const int triggerIndex = triggerChoice_->GetSelection();
    if (triggerIndex == wxNOT_FOUND) {
        error = wxT("请选择触发模式。");
        return false;
    }
    contract.trigger = sigflow::debug::DebugTrigger();
    contract.trigger.mask = ToUtf8(triggerMaskCtrl_->GetValue());
    contract.trigger.value = ToUtf8(triggerValueCtrl_->GetValue());
    contract.trigger.hsValidPath = ToUtf8(hsValidCtrl_->GetValue());
    contract.trigger.hsReadyPath = ToUtf8(hsReadyCtrl_->GetValue());
    switch (triggerIndex) {
        case 0: contract.trigger.kind = "none"; break;
        case 1: contract.trigger.kind = "mask_equal"; break;
        case 2: contract.trigger.kind = "edge_rising"; break;
        case 3: contract.trigger.kind = "edge_falling"; break;
        case 4:
            contract.trigger.kind = "none";
            contract.trigger.intentKind = "state_stall";
            break;
        case 5:
            contract.trigger.kind = "none";
            contract.trigger.intentKind = "handshake_timeout";
            if (contract.trigger.hsValidPath.empty() || contract.trigger.hsReadyPath.empty()) {
                error = wxT("handshake_timeout 需要填写 hs_valid 和 hs_ready 路径。");
                return false;
            }
            break;
        default: error = wxT("未知触发模式。"); return false;
    }
    if (triggerIndex >= 4) {
        contract.trigger.intentParams = wxString::Format(wxT("{\"cycles\":%d}"),
                                                         triggerCyclesCtrl_->GetValue()).ToStdString();
    }
    contract.capture.depth = static_cast<std::uint32_t>(depthCtrl_->GetValue());
    contract.capture.pretriggerSamples = static_cast<std::uint32_t>(pretriggerCtrl_->GetValue());
    contract.capture.decimation = static_cast<std::uint32_t>(decimationCtrl_->GetValue());
    contract.transport.kind = "uart";
    contract.transport.protocol = ToUtf8(protocolChoice_->GetStringSelection());
    contract.transport.txPort = ToUtf8(txPortCtrl_->GetValue());
    contract.transport.rxPort = ToUtf8(rxPortCtrl_->GetValue());
    std::uint64_t baud = 0;
    if (!ParseUnsigned(baudChoice_->GetStringSelection(), baud)) {
        error = wxT("请选择有效波特率。");
        return false;
    }
    contract.transport.baud = static_cast<std::uint32_t>(baud);
    contract.transport.txPin = txPinCtrl_->GetValue();
    contract.transport.rxPin = rxPinCtrl_->GetValue();
    contract.transport.rstPin = rstPinCtrl_->GetValue();
    contract.transport.syncEnabled = syncEnabledCtrl_->GetValue();
    contract.ApplyDefaults();
    std::string contractError;
    if (!contract.AssignProbeBitOffsets(contractError) || !contract.Validate(contractError)) {
        error = FromUtf8(contractError);
        return false;
    }
    return true;
}

bool DebugContractConfigWindow::SaveToPath(const wxString& path, wxString& error)
{
    sigflow::debug::DebugContract candidate;
    if (!BuildContract(candidate, error)) return false;
    wxFileName fileName(path);
    if (!fileName.DirExists() &&
        !wxFileName::Mkdir(fileName.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        error = wxT("无法创建目录：") + fileName.GetPath();
        return false;
    }
    wxFile file(path, wxFile::write);
    if (!file.IsOpened()) {
        error = wxT("无法写入：") + path;
        return false;
    }
    const wxString jsonText = wxString::FromUTF8(candidate.ToJson());
    const wxScopedCharBuffer data = jsonText.ToUTF8();
    const std::size_t length = data.data() ? std::strlen(data.data()) : 0;
    const bool written = file.Write(data.data(), length) == static_cast<wxFileOffset>(length);
    file.Close();
    if (!written) {
        error = wxT("写入失败：") + path;
        return false;
    }
    contract_ = std::move(candidate);
    contractFilePath_ = path;
    ApplyContractToUi();
    SetStatus(wxT("已保存：") + path);
    if (savedHandler_) savedHandler_(path);
    return true;
}

void DebugContractConfigWindow::SetStatus(const wxString& message, bool error)
{
    statusLabel_->SetLabel(message);
    statusLabel_->SetForegroundColour(error ? wxColour(0xB9, 0x1C, 0x1C) :
                                      wxColour(0x05, 0x96, 0x69));
    Layout();
}

void DebugContractConfigWindow::OnOpen(wxCommandEvent&)
{
    wxFileDialog dialog(this, wxT("打开 Debug Contract"), projectPath_, wxEmptyString,
                        wxT("JSON files (*.json)|*.json|All files (*.*)|*.*"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) LoadFromPath(dialog.GetPath());
}

void DebugContractConfigWindow::OnReload(wxCommandEvent&)
{
    LoadDefaultContract();
}

void DebugContractConfigWindow::OnValidate(wxCommandEvent&)
{
    sigflow::debug::DebugContract candidate;
    wxString error;
    if (!BuildContract(candidate, error)) SetStatus(error, true);
    else SetStatus(wxT("校验通过：探针总宽度不超过 32 bit。"));
}

void DebugContractConfigWindow::OnSave(wxCommandEvent&)
{
    const wxString path = contractFilePath_.IsEmpty() ? DefaultContractPath() : contractFilePath_;
    wxString error;
    if (path.IsEmpty() || !SaveToPath(path, error)) SetStatus(error, true);
}

void DebugContractConfigWindow::OnSaveAs(wxCommandEvent&)
{
    const wxString defaultPath = contractFilePath_.IsEmpty() ? DefaultContractPath() : contractFilePath_;
    wxFileDialog dialog(this, wxT("保存 Debug Contract"),
                        defaultPath.IsEmpty() ? projectPath_ : wxFileName(defaultPath).GetPath(),
                        wxT("debug-contract.json"),
                        wxT("JSON files (*.json)|*.json|All files (*.*)|*.*"),
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) return;
    wxString error;
    if (!SaveToPath(dialog.GetPath(), error)) SetStatus(error, true);
}
