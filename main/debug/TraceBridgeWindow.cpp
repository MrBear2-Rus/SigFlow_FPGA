#include "TraceBridgeWindow.h"

#include "DebugSession.h"
#include "DebugFingerprint.h"
#include "SerialPortEnumerator.h"
#include "SerialTransport.h"
#include "FtdiTransport.h"
#include "WaveformAligner.h"
#include "WaveformComparator.h"
#include "RootCauseGraph.h"
#include "ReplayScenario.h"
#include "DebugMappingBuilder.h"
#include "TransportBackend.h"
#include "TransportSelfTest.h"
#include "TraceBridgePreflight.h"
#include "TraceBridgeCaptureWizard.h"
#include "TraceBridgeRecovery.h"
#include "TraceBridgeTemplateCatalog.h"
#include "MonitorSummary.h"
#include "DeviceControl.h"
#include "ProbePlanningService.h"
#include "../fpga/FpgaConstraint.h"
#include "../fpga/FpgaPinData.h"
#include "../platform/PlatformPaths.h"

using sigflow::platform::JoinPath;

#include <json/json.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/panel.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/colour.h>
#include <wx/dcclient.h>
#include <wx/dir.h>
#include <wx/dnd.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stdpaths.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tokenzr.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/zipstrm.h>
#include <wx/wfstream.h>
#include <wx/weakref.h>
#include <wx/app.h>
#include <wx/scrolwin.h>
#include <wx/dialog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "json/json.h"

// ================== 自定义事件定义 ==================
wxDEFINE_EVENT(wxEVT_TB_STEP,     wxCommandEvent);
wxDEFINE_EVENT(wxEVT_TB_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_TB_LOG,      wxCommandEvent);
wxDEFINE_EVENT(wxEVT_TB_RESULT,   wxCommandEvent);

// ================== 工具 & 标签色 ==================
namespace {

// ---------------------------------------------------------------------------
// T-140 / PLT-012：未显式取色的"文字面"（wxStaticText / wxTextCtrl，系统默认黑）
// 在主题刷新时补为当期文字 token —— 深色档下默认黑不可读，浅色档下也避免出现
// 一个脱离 token 的"题外色"。已显式取色的控件保持不动。
// ---------------------------------------------------------------------------
bool NeedsDefaultTextColour(const wxColour& colour)
{
    return !colour.IsOk() || (colour.Red() == 0 && colour.Green() == 0 && colour.Blue() == 0);
}

void ApplyTextForegroundDefaults(wxWindow* window, const wxColour& colour)
{
    for (wxWindow* child : window->GetChildren()) {
        const bool textSurface = dynamic_cast<wxStaticText*>(child) != nullptr ||
                                 dynamic_cast<wxTextCtrl*>(child) != nullptr;
        if (textSurface && NeedsDefaultTextColour(child->GetForegroundColour())) {
            child->SetForegroundColour(colour);
        }
        ApplyTextForegroundDefaults(child, colour);
    }
}

std::string ToUtf8(const wxString& value) { return std::string(value.ToUTF8().data()); }
wxString ToWxString(const std::string& v)   { return wxString::FromUTF8(v.c_str()); }

void ShowTraceBridgeDetails(wxWindow* parent, const wxString& title,
                            const wxString& details)
{
    wxDialog dialog(parent, wxID_ANY, title, wxDefaultPosition,
                    parent->FromDIP(wxSize(760, 560)),
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* text = new wxTextCtrl(&dialog, wxID_ANY, details, wxDefaultPosition,
                                wxDefaultSize,
                                wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP |
                                    wxHSCROLL);
    text->SetMinSize(dialog.FromDIP(wxSize(700, 460)));
    root->Add(text, 1, wxALL | wxEXPAND, 8);
    auto* buttons = new wxStdDialogButtonSizer();
    buttons->AddButton(new wxButton(&dialog, wxID_OK, wxT("关闭")));
    buttons->Realize();
    root->Add(buttons, 0, wxALL | wxALIGN_RIGHT, 8);
    dialog.SetSizerAndFit(root);
    dialog.CentreOnParent();
    dialog.ShowModal();
}

std::string NormalizeArchiveEntryName(const wxString& value)
{
    std::string name = ToUtf8(value);
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

bool IsSafeArchiveEntryName(const std::string& name)
{
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos) return false;
    std::size_t begin = 0;
    while (begin <= name.size()) {
        const std::size_t end = name.find('/', begin);
        const std::string component = name.substr(begin, end == std::string::npos
                                                             ? std::string::npos
                                                             : end - begin);
        if (component.empty() || component == "." || component == "..") return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool ReadJsonFile(const std::filesystem::path& path, Json::Value& root, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to read " + path.string();
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), {});
    Json::CharReaderBuilder builder;
    std::string parseError;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(content.data(), content.data() + content.size(), &root, &parseError)) {
        error = "invalid JSON in " + path.string() + ": " + parseError;
        return false;
    }
    return true;
}

std::string QuoteProcessArg(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
        if (c == '\"') result += "\\\"";
        else result.push_back(c);
    }
    result += "\"";
    return result;
}

std::string FindBundledVerilator() {
    wxString configured;
    if (wxGetEnv("VERILATOR_BIN", &configured) && wxFileExists(configured)) {
        return ToUtf8(configured);
    }

    // 随包工具链统一布局：<repo>/external/fpga-tools/runtime/verilator/bin/...
    const wxString bundled = sigflow::platform::FindBundledVerilatorBinary();
    if (!bundled.IsEmpty()) {
        return ToUtf8(bundled);
    }

    wxFileName executable(wxStandardPaths::Get().GetExecutablePath());
    // 向上三级定位仓库根（例如 <repo>/build/bin -> <repo>）。
    // 必须用带边界判断的版本：直接在较浅的路径（exe 靠近盘根）上连调三次
    // RemoveLastDir() 会越过根目录并触发 wxArrayString 越界断言。
    for (int up = 0; up < 3; ++up) {
        if (!sigflow::platform::TryRemoveLastDir(executable)) {
            break;
        }
    }
    const wxString root = executable.GetPath();
    const wxUniChar pathSeparator = sigflow::platform::PathSeparator();
    const wxString verilatorBinDbg = sigflow::platform::WithExecutableSuffix("verilator_bin_dbg");
    const wxString candidates[] = {
        root + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "verilator-install" + pathSeparator + "bin" + pathSeparator + verilatorBinDbg,
        root + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "bin" + pathSeparator + verilatorBinDbg,
        wxGetCwd() + pathSeparator + "tools" + pathSeparator + "verilator" + pathSeparator + "verilator-install" + pathSeparator + "bin" + pathSeparator + verilatorBinDbg
    };
    for (const auto& candidate : candidates) {
        if (wxFileExists(candidate)) return ToUtf8(candidate);
    }
    return "verilator";
}

bool LoadReplayBuildConfig(const std::string& projectPath, std::string& topModule,
                           std::vector<std::string>& sourceFiles, std::string& error) {
    std::filesystem::path projectDirectory(projectPath);
    if (projectDirectory.filename() == "sigflow.project") projectDirectory = projectDirectory.parent_path();
    std::ifstream input(projectDirectory / "sigflow.project");
    if (!input) {
        error = "cannot open sigflow.project";
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), {});
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseError;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(content.data(), content.data() + content.size(), &root, &parseError)) {
        error = "invalid sigflow.project: " + parseError;
        return false;
    }
    const Json::Value& top = root["build"]["top_module"];
    if (top.isArray() && !top.empty()) topModule = top[0].asString();
    else if (top.isString()) topModule = top.asString();
    const Json::Value& sources = root["paths"]["source_files"];
    if (sources.isArray()) {
        for (const auto& item : sources) {
            if (!item.isString()) continue;
            std::filesystem::path source = item.asString();
            if (source.is_relative()) source = projectDirectory / source;
            sourceFiles.push_back(source.lexically_normal().string());
        }
    }
    if (topModule.empty() || sourceFiles.empty()) {
        error = "sigflow.project must define build.top_module and paths.source_files";
        return false;
    }
    return true;
}

std::uint64_t ParseHexU64OrDefault(const std::string& s, std::uint64_t def = 0) {
    if (s.empty()) return def;
    try {
        return std::stoull(s, nullptr, 16);
    } catch (...) {
        return def;
    }
}

wxColour Shade(const wxColour& c, double k) {
    int r = std::clamp(int(c.Red() * k), 0, 255);
    int g = std::clamp(int(c.Green() * k), 0, 255);
    int b = std::clamp(int(c.Blue() * k), 0, 255);
    return wxColour(static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b));
}

wxColour OkBg()    { return wxColour(0xEC, 0xDF, 0x5A); }  // green-ish
wxColour WarnBg()  { return wxColour(0xFE, 0xF3, 0xC7); }  // yellow
wxColour ErrBg()   { return wxColour(0xFE, 0xE2, 0xE2); }  // red
wxColour InfoBg()  { wxColour c = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW); return Shade(c, 0.97); }

wxColour StepDoneBg()   { return wxColour(0x22, 0xC5, 0x5E); }   // green-500
wxColour StepActiveBg() { return wxColour(0x63, 0x66, 0xF1); }   // indigo-500
wxColour StepTodoBg()   { return wxColour(0xE5, 0xE7, 0xEB); }   // gray-200

wxString Hex0x(uint32_t v) {
    if (v == 0) return "0x0";
    wxChar buf[16] = {};
    ::swprintf(buf, 16, L"0x%08X", v);
    return buf;
}

std::string Strip0xPrefix(std::string s) {
    if (s.size() >= 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
}

bool IsValidHex32(const std::string& s) {
    std::string t = Strip0xPrefix(s);
    if (t.empty() || t.size() > 8) return false;
    for (char c : t) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool ParseProbeEditor(const wxString& text,
                      std::vector<sigflow::debug::DebugProbe>& probes,
                      wxString& error) {
    probes.clear();
    wxStringTokenizer lines(text, wxT("\r\n"), wxTOKEN_RET_EMPTY_ALL);
    unsigned lineNumber = 0;
    while (lines.HasMoreTokens()) {
        ++lineNumber;
        wxString line = lines.GetNextToken().Trim(true).Trim(false);
        if (line.IsEmpty() || line.StartsWith(wxT("#"))) continue;
        wxStringTokenizer fields(line, wxT(" \t"), wxTOKEN_STRTOK);
        if (!fields.HasMoreTokens()) continue;
        const wxString path = fields.GetNextToken();
        unsigned width = 1;
        if (fields.HasMoreTokens()) {
            const wxString widthText = fields.GetNextToken();
            unsigned long parsed = 0;
            if (!widthText.ToULong(&parsed) || parsed == 0 || parsed > 32) {
                error = wxString::Format(wxT("探针第 %u 行位宽无效：%s"), lineNumber, widthText);
                return false;
            }
            width = static_cast<unsigned>(parsed);
        }
        wxString clockDomain;
        if (fields.HasMoreTokens()) clockDomain = fields.GetNextToken();
        if (fields.HasMoreTokens()) {
            error = wxString::Format(wxT("探针第 %u 行格式应为：信号路径 [位宽] [时钟域]"), lineNumber);
            return false;
        }
        if (path.IsEmpty()) {
            error = wxString::Format(wxT("探针第 %u 行缺少信号路径"), lineNumber);
            return false;
        }
        wxString id = path.AfterLast(wxT('.'));
        if (id.IsEmpty()) id = path;
        for (size_t index = 0; index < id.Length(); ++index) {
            const wxUniChar c = id[index];
            if (!(wxIsalnum(c) || c == wxT('_'))) id[index] = wxT('_');
        }
        if (id.IsEmpty() || wxIsdigit(id[0])) id = wxT("probe_") + id;
        const wxString baseId = id;
        unsigned suffix = 2;
        auto idExists = [&probes](const wxString& candidate) {
            const std::string value = std::string(candidate.ToUTF8().data());
            for (const auto& probe : probes) if (probe.id == value) return true;
            return false;
        };
        while (idExists(id)) id = baseId + wxString::Format(wxT("_%u"), suffix++);
        sigflow::debug::DebugProbe probe;
        probe.id = std::string(id.ToUTF8().data());
        probe.path = std::string(path.ToUTF8().data());
        probe.width = width;
        probe.clockDomain = std::string(clockDomain.ToUTF8().data());
        probes.push_back(std::move(probe));
    }
    if (probes.empty()) {
        error = wxT("至少需要一个探针，格式示例：dut.state 4");
        return false;
    }
    return true;
}

bool ValidateDebugTransportPins(const sigflow::debug::DebugContract& contract,
                                const wxString& projectPath, wxString& error,
                                wxString& guidance) {
    const FpgaPinDatabase& database = GetQFN88PinDatabase();
    const std::pair<const char*, int> pins[] = {
        {"tx", contract.transport.txPin},
        {"rx", contract.transport.rxPin},
        {"reset", contract.transport.rstPin}
    };
    std::set<int> used;
    for (const auto& item : pins) {
        if (item.second <= 0) continue;
        const PackagePin* pin = database.FindPin(item.second);
        if (!pin || !pin->available) {
            error = wxString::Format(wxT("TraceBridge UART %hs 引脚 %d 不可用或为保留引脚"),
                                     item.first, item.second);
            return false;
        }
        if (!used.insert(item.second).second) {
            error = wxString::Format(wxT("TraceBridge UART 引脚冲突：引脚 %d 被重复分配"), item.second);
            return false;
        }
    }
    const wxString bindingsPath = JoinPath(JoinPath(JoinPath(JoinPath(projectPath, ".sigflow"), "fpga"), "constraints"), "pin-bindings.json");
    if (wxFileExists(bindingsPath)) {
        ConstraintSheet sheet;
        wxString loadError;
        if (!LoadConstraintSheet(bindingsPath, sheet, loadError)) {
            guidance = wxT("无法读取现有引脚绑定表：") + loadError;
        } else {
            for (const auto& item : pins) {
                if (item.second <= 0) continue;
                for (const auto& binding : sheet.bindings) {
                    if (binding.packagePin == item.second) {
                        error = wxString::Format(wxT("TraceBridge UART %hs 引脚 %d 与用户端口 %s 冲突"),
                                                 item.first, item.second, binding.GetFullPortName());
                        return false;
                    }
                }
            }
        }
    }
    if (contract.transport.txPin <= 0 || contract.transport.rxPin <= 0) {
        guidance = wxT("UART 引脚尚未写入契约：若使用外接 USB-TTL，请连接 FPGA dbg_tx→TTL RX、"
                       "FPGA dbg_rx→TTL TX，并共地；实际引脚必须在 CST/契约中分配。");
    }
    return true;
}

wxString NowStamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tt = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    wxChar buf[32] = {};
    ::swprintf(buf, 32, L"%02d:%02d:%02d.%03d",
               tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return wxString(buf);
}

// worker 线程辅助函数：通过 QueueEvent marshal 回 UI 线程。
void PostLog(wxEvtHandler* h, TbLogTag tag, const wxString& msg) {
    auto* e = new wxCommandEvent(wxEVT_TB_LOG);
    e->SetString(wxString(TbLogTagText(tag)) + wxT("|") + msg);
    h->QueueEvent(e);
}
void PostResult(wxEvtHandler* h, int code, const wxString& msg) {
    auto* e = new wxCommandEvent(wxEVT_TB_RESULT);
    e->SetInt(code);
    e->SetString(msg);
    h->QueueEvent(e);
}

} // namespace

const wxChar* TbLogTagText(TbLogTag tag) {
    switch (tag) {
        case TbLogTag::INFO:   return wxT("INFO");
        case TbLogTag::CFG:    return wxT("CFG");
        case TbLogTag::ARM:    return wxT("ARM");
        case TbLogTag::POL:    return wxT("POL");
        case TbLogTag::RD:     return wxT("READ");
        case TbLogTag::OK:     return wxT("DONE");
        case TbLogTag::WR:     return wxT("WARN");
        case TbLogTag::ERR:    return wxT("ERR");
        case TbLogTag::CANCEL: return wxT("CANCEL");
    }
    return wxT("?");
}

wxColour TbLogTagColour(TbLogTag tag) {
    switch (tag) {
        case TbLogTag::INFO:   return wxColour(0x6B, 0x72, 0x80);
        case TbLogTag::CFG:    return wxColour(0x03, 0x69, 0xAA);
        case TbLogTag::ARM:    return wxColour(0x4F, 0x46, 0xE5);
        case TbLogTag::POL:    return wxColour(0x08, 0x91, 0xB2);
        case TbLogTag::RD:     return wxColour(0x0E, 0x74, 0x9B);
        case TbLogTag::OK:     return wxColour(0x05, 0x96, 0x69);
        case TbLogTag::WR:     return wxColour(0xD9, 0x77, 0x06);
        case TbLogTag::ERR:    return wxColour(0xDC, 0x26, 0x26);
        case TbLogTag::CANCEL: return wxColour(0xB9, 0x1C, 0x1C);
    }
    return *wxBLACK;
}

// ================== TraceBridgeStepStepper（7 步横条）==================
class TraceBridgeStepStepper : public wxPanel {
public:
    TraceBridgeStepStepper(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
        SetMinSize(wxSize(-1, 52));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&){ OnPaint(); });
        Bind(wxEVT_SIZE,  [this](wxSizeEvent&)  { Refresh(); });
        steps_ = { wxT("1 契约"), wxT("2 构建"), wxT("3 指纹"),
                   wxT("4 配置"), wxT("5 Arm"),  wxT("6 采集"), wxT("7 波形") };
        active_ = 1;
    }
    void SetActive(int oneBased, const wxString& sub = wxEmptyString) {
        active_ = std::max(0, std::min((int)steps_.size(), oneBased));
        sub_ = sub;
        Refresh();
    }
    void SetAllDone() { active_ = (int)steps_.size() + 1; Refresh(); }
    void Reset()      { active_ = 1; sub_.clear(); Refresh(); }

private:
    void OnPaint() {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(*wxWHITE_BRUSH);
        dc.Clear();
        const int pad = 16;
        const int circleR = 12;
        const int n = (int)steps_.size();
        if (n == 0) return;
        const int totalW = sz.GetWidth() - 2 * pad;
        const int gap = totalW / n;
        const int cy = 20;
        // connector line
        dc.SetPen(wxPen(StepTodoBg(), 3));
        dc.DrawLine(pad + gap/2, cy, pad + gap/2 + gap*(n-1), cy);
        for (int i = 0; i < n; ++i) {
            const int cx = pad + gap/2 + gap*i;
            // state
            int st; // 0 done, 1 active, 2 todo
            if (active_ > i + 1) st = 0;
            else if (active_ == i + 1) st = 1;
            else st = 2;
            wxColour bg = (st == 0) ? StepDoneBg() : (st == 1) ? StepActiveBg() : StepTodoBg();
            wxColour fg = (st == 2) ? wxColour(0x4B, 0x55, 0x63) : *wxWHITE;
            dc.SetBrush(wxBrush(bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawCircle(cx, cy, circleR);
            dc.SetTextForeground(fg);
            wxFont fnt = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Bold();
            dc.SetFont(fnt);
            wxString num = wxString::Format(wxT("%d"), i + 1);
            wxSize ext = dc.GetTextExtent(num);
            dc.DrawText(num, cx - ext.x/2, cy - ext.y/2);
            // label
            dc.SetTextForeground(wxColour(0x1F, 0x29, 0x37));
            wxFont lbl = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
            dc.SetFont(lbl);
            wxSize lsz = dc.GetTextExtent(steps_[i]);
            dc.DrawText(steps_[i], cx - lsz.x/2, cy + circleR + 4);
        }
        // substatus
        if (!sub_.empty()) {
            dc.SetTextForeground(wxColour(0x37, 0x41, 0x51));
            dc.SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Smaller());
            dc.DrawText(sub_, pad, sz.GetHeight() - 16);
        }
    }
    std::vector<wxString> steps_;
    int active_ = 1;
    wxString sub_;
};

// ================== TraceBridgeContractSummaryCard（KPI 卡片）==================
class TraceBridgeContractSummaryCard : public wxPanel {
public:
    TraceBridgeContractSummaryCard(wxWindow* parent)
        : wxPanel(parent, wxID_ANY) {
        SetBackgroundColour(*wxWHITE);
        auto* v = new wxBoxSizer(wxVERTICAL);
        v->AddSpacer(6);
        // path row
        auto* pathRow = new wxBoxSizer(wxVERTICAL);
        pathLabel_ = new wxStaticText(this, wxID_ANY, wxT("（未选择契约）"));
        pathLabel_->Wrap(FromDIP(380));
        pathLabel_->SetForegroundColour(UiTheme::Palette().text2);   // T-140：接 token
        pathRow->Add(pathLabel_, 0, wxLEFT | wxRIGHT | wxEXPAND, 10);
        v->Add(pathRow, 0, wxBOTTOM | wxEXPAND, 8);
        // KPI tile 2x2
        auto* grid = new wxGridSizer(2, FromDIP(8), FromDIP(8));
        tileProbe_ = MakeTile(grid, wxT("Probes"), wxT("-"), wxColour(0xE0, 0xE7, 0xFF));
        tileDepth_ = MakeTile(grid, wxT("Depth"), wxT("-"), wxColour(0xDC, 0xF6, 0xE5));
        tileTrigger_ = MakeTile(grid, wxT("Trigger"), wxT("-"), wxColour(0xFE, 0xF3, 0xC7));
        tileFingerprint_ = MakeTile(grid, wxT("Fingerprint"), wxT("-"), wxColour(0xF3, 0xE8, 0xFF));
        v->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        // ribbons
        auto r1 = MakeRibbon(v, wxT("源文件未修改（Git clean）"), UiTheme::Palette().brand, true);
        ribbonClean_ = r1.first; ribbonTextClean_ = r1.second;
        auto r2 = MakeRibbon(v, wxT("资源占用：LUT 估算待接入"), UiTheme::Palette().text2, false);
        ribbonResource_ = r2.first; ribbonTextResource_ = r2.second;
        // banner
        banner_ = new wxPanel(this, wxID_ANY);
        banner_->SetBackgroundColour(*wxWHITE);
        banner_->Hide();
        auto* bv = new wxBoxSizer(wxVERTICAL);
        bannerText_ = new wxStaticText(banner_, wxID_ANY, wxEmptyString);
        // 原 *wxWHITE：banner 底为 *wxWHITE 时不可读；改接当期主文字 token（T-140）
    bannerText_->SetForegroundColour(UiTheme::Palette().text1);
        bannerText_->Wrap(FromDIP(380));
        bv->AddSpacer(6);
        bv->Add(bannerText_, 0, wxLEFT | wxRIGHT, 10);
        bv->AddSpacer(6);
        banner_->SetSizer(bv);
        v->Add(banner_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        SetSizer(v);
    }
    void UpdateFrom(const sigflow::debug::DebugContract& contract,
                    const wxString& contractPath,
                    const wxString& validateError = wxEmptyString) {
        // path
        if (contractPath.IsEmpty()) {
            pathLabel_->SetLabel(wxT("（未选择契约）"));
        } else {
            pathLabel_->SetLabel(wxT("📄 ") + contractPath);
        }
        // tile values
        wxString probeInfo;
        if (contract.probes.empty()) {
            probeInfo = wxT("0 probes / 0 bits");
        } else {
            uint32_t totalBits = 0;
            for (auto& p : contract.probes) totalBits += p.width;
            probeInfo.Printf(wxT("%u probes / %u bits / 32-bit"),
                             static_cast<unsigned>(contract.probes.size()),
                             static_cast<unsigned>(totalBits));
        }
        SetTileValue(tileProbe_, probeInfo);

        wxString depthInfo;
        depthInfo.Printf(wxT("%u samples\npre-trigger: %u samples"),
                         static_cast<unsigned>(contract.capture.depth),
                         static_cast<unsigned>(contract.capture.pretriggerSamples));
        SetTileValue(tileDepth_, depthInfo);

        wxString trigInfo;
        {
            std::uint32_t mask = 0, value = 0;
            std::uint8_t mode = 0;
            std::uint16_t count = 1;
            std::string err;
            bool ok = sigflow::debug::DebugAcquisition::TriggerParamsFromContract(
                contract, mask, value, mode, count, err);
            if (!ok) {
                trigInfo.Printf(wxT("⚠️ 配置错误"));
            } else if (contract.trigger.intentKind == "state_stall") {
                trigInfo.Printf(wxT("状态停滞 N=%u"), static_cast<unsigned>(count));
            } else if (contract.trigger.intentKind == "handshake_timeout") {
                trigInfo.Printf(wxT("握手超时 N=%u"), static_cast<unsigned>(count));
            } else if (contract.trigger.kind.empty() || contract.trigger.kind == "none") {
                trigInfo = wxT("立即触发");
            } else {
                trigInfo.Printf(wxT("掩码相等 N=%u"), static_cast<unsigned>(count));
            }
        }
        SetTileValue(tileTrigger_, trigInfo);

        wxString fpInfo;
        fpInfo.Printf(wxT("…%04X"), static_cast<unsigned>(contract.fingerprints.fingerprint64 & 0xFFFFu));
        SetTileValue(tileFingerprint_, fpInfo);

        // ribbons
        ribbonClean_->SetBackgroundColour(wxColour(0xD1, 0xFA, 0xAE)); // green-100
        ribbonTextClean_->SetLabel(wxT("✓ 源文件未修改（Debug 构建为 Shadow Build，Git 工作区保持干净）"));
        ribbonTextClean_->SetForegroundColour(UiTheme::Palette().codeType);   // T-140：接 token
        ribbonClean_->Show(true);

        // 资源 Ribbon：先用阈值估算（>10% 黄，>20% 红），后续可接入 Yosys manifest 真值
        {
            const uint32_t budget = 6000;  // Tang Nano 9K 大概 LUT 预算（保守估计 6K 给 debug）
            // 估算：每探针 1 个 FFD + 触发 N 拍比较 + BSRAM 外，每 bit 2 LUT 粗估
            uint32_t totalBits = 0;
            for (auto& p : contract.probes) totalBits += p.width;
            const uint32_t est = totalBits * 2 + 350;  // + BRAM 开销 + 协议栈
            const int pct = budget == 0 ? 0 : std::min(100u, 100u * est / budget);
            wxColour col = wxColour(0xE5, 0xE7, 0xEB);
            wxColour fg = UiTheme::Palette().text2;
            if (pct >= 20) { col = ErrBg();   fg = UiTheme::Palette().danger; }
            else if (pct >= 10) { col = WarnBg(); fg = UiTheme::Palette().warn; }
            ribbonResource_->SetBackgroundColour(col);
            ribbonTextResource_->SetForegroundColour(fg);
            ribbonTextResource_->SetLabel(wxString::Format(
                wxT("LUT 粗估 %u（约 %d%%），后续接入 Yosys 统计。"), est, pct));
            ribbonResource_->Show(true);
        }

        // banner
        if (!validateError.IsEmpty()) {
            banner_->SetBackgroundColour(wxColour(0xDC, 0x26, 0x26));
            bannerText_->SetLabel(wxT("❌ 契约校验失败：") + validateError);
            banner_->Show(true);
        } else {
            banner_->Hide();
        }
        Layout();
    }
    void SetCleanRibbonError(const wxString& msg) {
        ribbonClean_->SetBackgroundColour(ErrBg());
        ribbonTextClean_->SetLabel(msg);
        ribbonTextClean_->SetForegroundColour(UiTheme::Palette().danger);
        Layout();
    }
    wxButton* browseBtn = nullptr;
    wxButton* reloadBtn = nullptr;

private:
    struct Tile { wxPanel* panel; wxStaticText* title; wxStaticText* value; };
    Tile* MakeTile(wxGridSizer* grid, const wxString& title, const wxString& value, const wxColour& bg) {
        Tile* t = new Tile();
        t->panel = new wxPanel(this, wxID_ANY);
        t->panel->SetBackgroundColour(bg);
        auto* v = new wxBoxSizer(wxVERTICAL);
        v->AddSpacer(6);
        t->title = new wxStaticText(t->panel, wxID_ANY, title);
        t->title->SetForegroundColour(UiTheme::Palette().text2);
        t->title->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Smaller());
        t->value = new wxStaticText(t->panel, wxID_ANY, value);
        t->value->SetForegroundColour(UiTheme::Palette().text1);
        t->value->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Bold());
        t->value->Wrap(FromDIP(160));
        v->Add(t->title, 0, wxLEFT | wxRIGHT, 10);
        v->AddSpacer(4);
        v->Add(t->value, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        t->panel->SetSizer(v);
        grid->Add(t->panel, 1, wxEXPAND);
        return t;
    }
    void SetTileValue(Tile* t, const wxString& v) {
        t->value->SetLabel(v); t->value->Wrap(FromDIP(160)); t->panel->Layout();
    }
    std::pair<wxPanel*, wxStaticText*> MakeRibbon(wxBoxSizer* v, const wxString& text,
                                                  const wxColour& fg, bool good) {
        wxPanel* p = new wxPanel(this, wxID_ANY);
        p->SetBackgroundColour(good ? wxColour(0xD1, 0xFA, 0xAE) : InfoBg());
        auto* rv = new wxBoxSizer(wxVERTICAL);
        rv->AddSpacer(5);
        wxStaticText* label = new wxStaticText(p, wxID_ANY, text);
        label->Wrap(FromDIP(380));
        label->SetForegroundColour(good ? UiTheme::Palette().codeType : fg);
        label->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Smaller());
        rv->Add(label, 0, wxLEFT | wxRIGHT, 10);
        rv->AddSpacer(5);
        p->SetSizer(rv);
        v->Add(p, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        return { p, label };
    }

    wxStaticText* pathLabel_ = nullptr;
    Tile* tileProbe_ = nullptr;
    Tile* tileDepth_ = nullptr;
    Tile* tileTrigger_ = nullptr;
    Tile* tileFingerprint_ = nullptr;
    wxPanel*    ribbonClean_      = nullptr; wxStaticText* ribbonTextClean_    = nullptr;
    wxPanel*    ribbonResource_   = nullptr; wxStaticText* ribbonTextResource_ = nullptr;
    wxPanel*    banner_ = nullptr;
    wxStaticText* bannerText_ = nullptr;
};

// ================== TraceBridgeRuntimePanel（Tab 2 Runtime）==================
class TraceBridgeRuntimePanel : public wxScrolledWindow {
public:
    TraceBridgeRuntimePanel(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL) {
        SetScrollRate(0, FromDIP(12));
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        auto* v = new wxBoxSizer(wxVERTICAL);
        v->AddSpacer(6);

        // Override toggle
        auto* overrideRow = new wxStaticBoxSizer(wxHORIZONTAL, this, wxT("Runtime Override"));
        overrideCk_ = new wxCheckBox(this, wxID_ANY, wxT("覆盖契约参数（⚠️可能与当前 bitstream 不匹配）"));
        overrideCk_->SetValue(false);
        overrideRow->Add(overrideCk_, 0, wxALL, 8);
        v->Add(overrideRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        // transport
        auto* transBox = new wxStaticBoxSizer(wxVERTICAL, this, wxT("Transport"));
        auto* g = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
        g->AddGrowableCol(1, 1);
        g->Add(new wxStaticText(this, wxID_ANY, wxT("串口")), 0, wxALIGN_CENTER_VERTICAL);
        auto* portRow = new wxBoxSizer(wxHORIZONTAL);
        serialChoice_ = new wxChoice(this, wxID_ANY);
        refreshBtn_ = new wxButton(this, wxID_ANY, wxT("刷新"));
        portRow->Add(serialChoice_, 1, wxRIGHT | wxEXPAND, 6);
        portRow->Add(refreshBtn_);
        g->Add(portRow, 1, wxEXPAND);
        g->Add(new wxStaticText(this, wxID_ANY, wxT("波特率")), 0, wxALIGN_CENTER_VERTICAL);
        baudSpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("921600"));
        baudSpin_->SetRange(9600, 3000000);
        baudSpin_->SetIncrement(1200);
        g->Add(baudSpin_, 1, wxEXPAND);
        g->Add(new wxStaticText(this, wxID_ANY, wxT("协议")), 0, wxALIGN_CENTER_VERTICAL);
        protoChoice_ = new wxChoice(this, wxID_ANY);
        protoChoice_->Append(wxT("minimal"));
        protoChoice_->Append(wxT("full"));
        protoChoice_->SetSelection(0);
        g->Add(protoChoice_, 1, wxEXPAND);
        g->Add(new wxStaticText(this, wxID_ANY, wxT("采集超时(ms)")), 0, wxALIGN_CENTER_VERTICAL);
        timeoutSpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("5000"));
        timeoutSpin_->SetRange(100, 600000);
        timeoutSpin_->SetIncrement(500);
        g->Add(timeoutSpin_, 1, wxEXPAND);
        g->Add(new wxStaticText(this, wxID_ANY, wxT("指纹校验")), 0, wxALIGN_CENTER_VERTICAL);
        checkFpCk_ = new wxCheckBox(this, wxID_ANY, wxT("校验板载指纹（推荐）"));
        checkFpCk_->SetValue(true);
        g->Add(checkFpCk_, 1, wxEXPAND);
        transBox->Add(g, 1, wxALL | wxEXPAND, 10);
        v->Add(transBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        auto* selfTestRow = new wxBoxSizer(wxHORIZONTAL);
        selfTestButton_ = new wxButton(this, wxID_ANY, wxT("用户主动链路自测"));
        selfTestButton_->SetToolTip(wxT("只读 PING/STATUS 握手；不会复位、配置或布防 DUT"));
        selfTestRow->Add(selfTestButton_, 0, wxRIGHT, 8);
        linkStatus_ = new wxStaticText(this, wxID_ANY,
            wxT("后端：未自测 · 目标：未验证 · 验证级别：未执行"));
        linkStatus_->Wrap(FromDIP(360));
        selfTestRow->Add(linkStatus_, 1, wxALIGN_CENTER_VERTICAL);
        selfTestRow->Add(new wxStaticText(this, wxID_ANY,
            wxT("仅在没有采集任务时可用；明细写入运行日志")),
            0, wxALIGN_CENTER_VERTICAL);
        v->Add(selfTestRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        // Host-side repeated capture plan. This only schedules existing
        // single-window acquisitions; it does not change the FPGA contract.
        auto* repeatBox = new wxStaticBoxSizer(wxVERTICAL, this, wxT("重复采集（板外计划）"));
        auto* rg = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
        rg->AddGrowableCol(1, 1);
        rg->Add(new wxStaticText(this, wxID_ANY, wxT("完整窗口数")), 0, wxALIGN_CENTER_VERTICAL);
        captureCountSpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("1"));
        captureCountSpin_->SetRange(1, 10000);
        rg->Add(captureCountSpin_, 1, wxEXPAND);
        rg->Add(new wxStaticText(this, wxID_ANY, wxT("最大尝试数（0=自动）")), 0, wxALIGN_CENTER_VERTICAL);
        maxAttemptsSpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("0"));
        maxAttemptsSpin_->SetRange(0, 100000);
        rg->Add(maxAttemptsSpin_, 1, wxEXPAND);
        rg->Add(new wxStaticText(this, wxID_ANY, wxT("连续失败上限（0=不限制）")), 0, wxALIGN_CENTER_VERTICAL);
        failureLimitSpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("3"));
        failureLimitSpin_->SetRange(0, 1000);
        rg->Add(failureLimitSpin_, 1, wxEXPAND);
        rg->Add(new wxStaticText(this, wxID_ANY, wxT("总期限(ms)")), 0, wxALIGN_CENTER_VERTICAL);
        totalTimeoutSpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("60000"));
        totalTimeoutSpin_->SetRange(1000, 3600000);
        totalTimeoutSpin_->SetIncrement(1000);
        rg->Add(totalTimeoutSpin_, 1, wxEXPAND);
        rg->Add(new wxStaticText(this, wxID_ANY, wxT("窗口间隔(ms)")), 0, wxALIGN_CENTER_VERTICAL);
        interAttemptDelaySpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("0"));
        interAttemptDelaySpin_->SetRange(0, 600000);
        interAttemptDelaySpin_->SetIncrement(100);
        rg->Add(interAttemptDelaySpin_, 1, wxEXPAND);
        repeatBox->Add(rg, 1, wxALL | wxEXPAND, 10);
        v->Add(repeatBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        // trigger zone
        auto* trigBox = new wxStaticBoxSizer(wxVERTICAL, this, wxT("Trigger（默认按契约锁定，Override 才允许改）"));
        auto* tg = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
        tg->AddGrowableCol(1, 1);
        tg->Add(new wxStaticText(this, wxID_ANY, wxT("触发模式")), 0, wxALIGN_CENTER_VERTICAL);
        trigModeChoice_ = new wxChoice(this, wxID_ANY);
        trigModeChoice_->Append(wxT("立即触发"));            // 0
        trigModeChoice_->Append(wxT("掩码相等 N 次"));        // 1 kind=mask_equal mode=0 count=N
        trigModeChoice_->Append(wxT("停滞 N 拍不变"));        // 2 intent=state_stall
        trigModeChoice_->Append(wxT("握手超时 N 拍"));        // 3 intent=handshake_timeout
        trigModeChoice_->Append(wxT("上升沿检测"));            // 4 kind=edge_rising mode=3
        trigModeChoice_->Append(wxT("下降沿检测"));            // 5 kind=edge_falling mode=4
        trigModeChoice_->SetSelection(0);
        tg->Add(trigModeChoice_, 1, wxEXPAND);
        tg->Add(new wxStaticText(this, wxID_ANY, wxT("Count/N")), 0, wxALIGN_CENTER_VERTICAL);
        countSpin_ = new wxSpinCtrl(this, wxID_ANY, wxT("1"));
        countSpin_->SetRange(1, 65535);
        tg->Add(countSpin_, 1, wxEXPAND);
        trigBox->Add(tg, 0, wxALL | wxEXPAND, 10);

        // dynamic area
        dynArea_ = new wxPanel(this, wxID_ANY);
        dynArea_->SetBackgroundColour(GetBackgroundColour());
        auto* dv = new wxBoxSizer(wxVERTICAL);
        // mask / value row
        auto* mvRow = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
        mvRow->AddGrowableCol(1, 1);
        maskLabel_ = new wxStaticText(dynArea_, wxID_ANY, wxT("Mask (HEX)"));
        mvRow->Add(maskLabel_, 0, wxALIGN_CENTER_VERTICAL);
        maskCtrl_ = new wxTextCtrl(dynArea_, wxID_ANY);
        mvRow->Add(maskCtrl_, 1, wxEXPAND);
        valueLabel_ = new wxStaticText(dynArea_, wxID_ANY, wxT("Value (HEX)"));
        mvRow->Add(valueLabel_, 0, wxALIGN_CENTER_VERTICAL);
        valueCtrl_ = new wxTextCtrl(dynArea_, wxID_ANY);
        mvRow->Add(valueCtrl_, 1, wxEXPAND);
        // hs valid/ready
        hsValidLabel_ = new wxStaticText(dynArea_, wxID_ANY, wxT("hs_valid 路径"));
        mvRow->Add(hsValidLabel_, 0, wxALIGN_CENTER_VERTICAL);
        hsValidCtrl_ = new wxTextCtrl(dynArea_, wxID_ANY);
        hsValidCtrl_->SetHint(wxT("可从树面板拖拽信号路径"));
        hsValidCtrl_->SetDropTarget(new SignalPathDropTarget(hsValidCtrl_));
        mvRow->Add(hsValidCtrl_, 1, wxEXPAND);
        hsReadyLabel_ = new wxStaticText(dynArea_, wxID_ANY, wxT("hs_ready 路径"));
        mvRow->Add(hsReadyLabel_, 0, wxALIGN_CENTER_VERTICAL);
        hsReadyCtrl_ = new wxTextCtrl(dynArea_, wxID_ANY);
        hsReadyCtrl_->SetHint(wxT("可从树面板拖拽信号路径"));
        hsReadyCtrl_->SetDropTarget(new SignalPathDropTarget(hsReadyCtrl_));
        mvRow->Add(hsReadyCtrl_, 1, wxEXPAND);
        dv->Add(mvRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        dynArea_->SetSizer(dv);
        trigBox->Add(dynArea_, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        v->Add(trigBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        // capture section (read-only, from contract)
        auto* capBox = new wxStaticBoxSizer(wxVERTICAL, this, wxT("Capture（按契约锁定，不可改）"));
        auto* cg = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
        cg->AddGrowableCol(1, 1);
        cg->Add(new wxStaticText(this, wxID_ANY, wxT("采样深度")), 0, wxALIGN_CENTER_VERTICAL);
        depthCtrl_ = new wxTextCtrl(this, wxID_ANY, wxT("0"));
        depthCtrl_->Disable();
        cg->Add(depthCtrl_, 1, wxEXPAND);
        cg->Add(new wxStaticText(this, wxID_ANY, wxT("预触发样本数")), 0, wxALIGN_CENTER_VERTICAL);
        preTriggerCtrl_ = new wxTextCtrl(this, wxID_ANY, wxT("50"));
        preTriggerCtrl_->Disable();
        cg->Add(preTriggerCtrl_, 1, wxEXPAND);
        capBox->Add(cg, 1, wxALL | wxEXPAND, 10);
        v->Add(capBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        SetSizer(v);

        // bindings
        overrideCk_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&){ UpdateEnabledState(); });
        trigModeChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&){ UpdateDynamicAreaVisibility(); });
    }

    void ApplyCapturePlan(TraceBridgeCaptureRequest& request) const {
        request.captureTargetCount = static_cast<std::size_t>(captureCountSpin_->GetValue());
        request.captureMaxAttempts = static_cast<std::size_t>(maxAttemptsSpin_->GetValue());
        request.captureStopAfterFailures = static_cast<std::size_t>(failureLimitSpin_->GetValue());
        request.captureTotalTimeoutMs = static_cast<std::uint32_t>(totalTimeoutSpin_->GetValue());
        request.captureAttemptTimeoutMs = static_cast<std::uint32_t>(timeoutSpin_->GetValue());
        request.captureInterAttemptDelayMs = static_cast<std::uint32_t>(interAttemptDelaySpin_->GetValue());
    }

    void SetSelfTestStatus(const wxString& message) {
        if (linkStatus_) linkStatus_->SetLabel(message);
        Layout();
    }

    // Apply contract → fill UI (read-only values shown regardless of override)
    void ReflectContract(const sigflow::debug::DebugContract& c) {
        baudSpin_->SetValue((int)(c.transport.baud == 0 ? 921600u : c.transport.baud));
        const wxString proto = ToWxString(c.transport.protocol);
        if (proto == wxT("full")) protoChoice_->SetSelection(1);
        else protoChoice_->SetSelection(0);
        timeoutSpin_->SetValue(5000);
        depthCtrl_->SetValue(wxString::Format(wxT("%u"), static_cast<unsigned>(c.capture.depth)));
        preTriggerCtrl_->SetValue(wxString::Format(wxT("%u"), static_cast<unsigned>(c.capture.pretriggerSamples)));

        // trigger → 模式选择
        int modeIdx = 0;
        if (c.trigger.intentKind == "state_stall")            modeIdx = 2;
        else if (c.trigger.intentKind == "handshake_timeout") modeIdx = 3;
        else if (c.trigger.kind.empty() || c.trigger.kind == "none") modeIdx = 0;
        else                                                  modeIdx = 1;
        trigModeChoice_->SetSelection(modeIdx);

        // N / Count
        {
            uint32_t m, v; uint8_t mo; uint16_t co; std::string e;
            bool ok = sigflow::debug::DebugAcquisition::TriggerParamsFromContract(
                c, m, v, mo, co, e);
            if (ok) countSpin_->SetValue((int)co);
            maskCtrl_->SetValue(c.trigger.mask.empty() ? wxString(wxT("0xFFFFFFFF")) : ToWxString(c.trigger.mask));
            valueCtrl_->SetValue(c.trigger.value.empty() ? wxString(wxT("0x00000000")) : ToWxString(c.trigger.value));
        }
        hsValidCtrl_->SetValue(ToWxString(c.trigger.hsValidPath));
        hsReadyCtrl_->SetValue(ToWxString(c.trigger.hsReadyPath));
        UpdateDynamicAreaVisibility();
        UpdateEnabledState();
    }

    // Apply runtime overrides onto request.contract if override mode is on
    void ApplyToRequest(sigflow::debug::DebugContract& c,
                        sigflow::debug::DebugAcquisitionOptions& opts,
                        wxString& error) const {
        // transport
        c.transport.baud = static_cast<uint32_t>(baudSpin_->GetValue());
        c.transport.protocol = ToUtf8(protoChoice_->GetStringSelection());
        opts.syncCalibrate = c.transport.syncEnabled;
        opts.captureTimeoutMs = timeoutSpin_->GetValue();
        opts.checkFingerprint = checkFpCk_->GetValue();

        if (!overrideCk_->GetValue()) return;

        // trigger overrides
        const int sel = trigModeChoice_->GetSelection();
        c.trigger.intentKind.clear();
        c.trigger.intentParams.clear();
        c.trigger.kind.clear();
        c.trigger.mask = Strip0xPrefix(ToUtf8(maskCtrl_->GetValue()));
        c.trigger.value = Strip0xPrefix(ToUtf8(valueCtrl_->GetValue()));
        c.trigger.hsValidPath = ToUtf8(hsValidCtrl_->GetValue());
        c.trigger.hsReadyPath = ToUtf8(hsReadyCtrl_->GetValue());
        Json::Value p;
        p["n"] = countSpin_->GetValue();
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        std::string paramsJson = Json::writeString(wb, p);
        switch (sel) {
            case 0: c.trigger.kind = "none"; break;
            case 1: c.trigger.kind = "mask_equal";
                    if (!IsValidHex32(c.trigger.mask)  || c.trigger.mask.empty())  { error = wxT("Trigger Mask 需为 0x0~0xFFFFFFFF 十六进制"); return; }
                    if (!IsValidHex32(c.trigger.value) || c.trigger.value.empty()) { error = wxT("Trigger Value 需为 0x0~0xFFFFFFFF 十六进制"); return; }
                    c.trigger.intentParams = paramsJson; break;
            case 2: c.trigger.intentKind = "state_stall";
                    c.trigger.intentParams = paramsJson; break;
            case 3: c.trigger.intentKind = "handshake_timeout";
                    if (c.trigger.hsValidPath.empty() || c.trigger.hsReadyPath.empty()) {
                        error = wxT("握手超时需要填 hs_valid / hs_ready 路径"); return;
                    }
                    c.trigger.intentParams = paramsJson; break;
            case 4: c.trigger.kind = "edge_rising";
                    if (!c.trigger.mask.empty() && !IsValidHex32(c.trigger.mask)) {
                        error = wxT("上升沿 Mask 需为 0x0~0xFFFFFFFF 十六进制"); return;
                    }
                    c.trigger.intentParams = paramsJson; break;
            case 5: c.trigger.kind = "edge_falling";
                    if (!c.trigger.mask.empty() && !IsValidHex32(c.trigger.mask)) {
                        error = wxT("下降沿 Mask 需为 0x0~0xFFFFFFFF 十六进制"); return;
                    }
                    c.trigger.intentParams = paramsJson; break;
        }
    }

    // Serial helpers (choices with friendly name, client data = pure port name)
    wxChoice* serialChoice_ = nullptr;
    wxButton* refreshBtn_   = nullptr;

    wxString GetSelectedSerialPureName() const {
        if (!serialChoice_ || serialChoice_->GetSelection() == wxNOT_FOUND) return {};
        const wxStringClientData* cd =
            dynamic_cast<wxStringClientData*>(serialChoice_->GetClientObject(serialChoice_->GetSelection()));
        if (!cd) return serialChoice_->GetStringSelection();
        return cd->GetData();
    }

private:
    void UpdateEnabledState() {
        const bool override = overrideCk_->GetValue();
        serialChoice_->Enable(true);  // 串口不管 Override 都能选
        refreshBtn_->Enable(true);
        baudSpin_->Enable(override);
        protoChoice_->Enable(override);
        timeoutSpin_->Enable(true);   // 超时总是可改（不算破坏契约）
        checkFpCk_->Enable(true);     // 指纹校验独立配置
        // trigger zone
        trigModeChoice_->Enable(override);
        countSpin_->Enable(override);
        maskCtrl_->Enable(override);
        valueCtrl_->Enable(override);
        hsValidCtrl_->Enable(override);
        hsReadyCtrl_->Enable(override);
        // contract lock ones remain always disabled
        depthCtrl_->Disable();
        preTriggerCtrl_->Disable();
    }
    void UpdateDynamicAreaVisibility() {
        const int sel = trigModeChoice_->GetSelection();
        const bool showMV = (sel == 1 || sel == 2 || sel == 4 || sel == 5);
        const bool showHS = (sel == 3);
        const bool showValue = (sel == 1); // 仅掩码相等需要 value
        maskLabel_->Show(showMV);
        maskCtrl_->Show(showMV);
        valueLabel_->Show(showValue);
        valueCtrl_->Show(showValue);
        hsValidLabel_->Show(showHS);
        hsValidCtrl_->Show(showHS);
        hsReadyLabel_->Show(showHS);
        hsReadyCtrl_->Show(showHS);
        dynArea_->Layout();
    }

    wxCheckBox* overrideCk_ = nullptr;
    wxSpinCtrl* baudSpin_ = nullptr;
    wxChoice* protoChoice_ = nullptr;
    wxSpinCtrl* timeoutSpin_ = nullptr;
    wxSpinCtrl* captureCountSpin_ = nullptr;
    wxSpinCtrl* maxAttemptsSpin_ = nullptr;
    wxSpinCtrl* failureLimitSpin_ = nullptr;
    wxSpinCtrl* totalTimeoutSpin_ = nullptr;
    wxSpinCtrl* interAttemptDelaySpin_ = nullptr;
    wxCheckBox* checkFpCk_ = nullptr;
    wxChoice* trigModeChoice_ = nullptr;
    wxSpinCtrl* countSpin_ = nullptr;
    wxPanel* dynArea_ = nullptr;
    wxStaticText* maskLabel_ = nullptr;
    wxTextCtrl* maskCtrl_ = nullptr;
    wxStaticText* valueLabel_ = nullptr;
    wxTextCtrl* valueCtrl_ = nullptr;
    wxStaticText* hsValidLabel_ = nullptr;
    wxTextCtrl* hsValidCtrl_ = nullptr;
    wxStaticText* hsReadyLabel_ = nullptr;
    wxTextCtrl* hsReadyCtrl_ = nullptr;
    wxTextCtrl* depthCtrl_ = nullptr;
    wxTextCtrl* preTriggerCtrl_ = nullptr;
    wxButton* selfTestButton_ = nullptr;
    wxStaticText* linkStatus_ = nullptr;

    friend class TraceBridgeWindow;  // TraceBridgeWindow 需要直接读取控件状态来组装 request
};

// ================== SignalPathDropTarget 实现 ==================
SignalPathDropTarget::SignalPathDropTarget(wxTextCtrl* target)
    : wxDropTarget(new wxTextDataObject), target_(target) {}

wxDragResult SignalPathDropTarget::OnData(wxCoord, wxCoord, wxDragResult def) {
    if (!target_) return wxDragNone;
    if (!GetData()) return wxDragNone;
    const auto* textObj = dynamic_cast<wxTextDataObject*>(GetDataObject());
    if (textObj && !textObj->GetText().IsEmpty()) {
        target_->SetValue(textObj->GetText());
        return def;
    }
    return wxDragNone;
}

bool SignalPathDropTarget::OnDrop(wxCoord, wxCoord) {
    return true;
}

// ================== TraceBridgeWindow 实现 ==================
TraceBridgeWindow::TraceBridgeWindow(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, wxT("TraceBridge 硬件调试平台"), wxDefaultPosition,
              wxSize(1100, 760), wxDEFAULT_FRAME_STYLE | wxRESIZE_BORDER)
{
    Bind(wxEVT_CLOSE_WINDOW, &TraceBridgeWindow::OnClose, this);
    Bind(wxEVT_TB_STEP,     &TraceBridgeWindow::OnTBStep,     this);
    Bind(wxEVT_TB_PROGRESS, &TraceBridgeWindow::OnTBProgress, this);
    Bind(wxEVT_TB_LOG,      &TraceBridgeWindow::OnTBLog,      this);
    Bind(wxEVT_TB_RESULT,   &TraceBridgeWindow::OnTBResult,   this);
    BuildUi();
    RefreshSerialPorts();
    LoadDefaultContract();
    // T-140 / THE-030：订阅主题变化（句柄为本窗成员，先于子控件销毁而解绑）
    themeSubscription_ = UiTheme::Subscribe([this] { ApplyTheme(); });
}

TraceBridgeWindow::~TraceBridgeWindow() {
    m_aborted->store(true);
    JoinWorkerIfAny();
}

void TraceBridgeWindow::JoinWorkerIfAny() {
    if (worker_ && worker_->joinable()) {
        m_aborted->store(true);
        worker_->join();
    }
    worker_.reset();
}

void TraceBridgeWindow::OnClose(wxCloseEvent& e) {
    JoinWorkerIfAny();
    if (e.CanVeto()) {
        Hide();
        e.Veto();
    } else {
        e.Skip();
    }
}

void TraceBridgeWindow::BuildUi() {
    auto* root = new wxBoxSizer(wxVERTICAL);
    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                      wxSP_LIVE_UPDATE | wxSP_3DSASH | wxSP_BORDER);
    splitter_->SetMinimumPaneSize(FromDIP(340));
    leftPanel_ = new wxPanel(splitter_, wxID_ANY);
    rightPanel_ = new wxPanel(splitter_, wxID_ANY);
    BuildLeftPanel(leftPanel_);
    BuildRightPanel(rightPanel_);
    splitter_->SplitVertically(leftPanel_, rightPanel_, FromDIP(440));
    root->Add(splitter_, 1, wxALL | wxEXPAND, 4);
    SetSizerAndFit(root);
    SetMinSize(FromDIP(wxSize(980, 640)));
    ApplyTheme();   // T-140：底色/顶层文字统一从 token 取（主题变化时重放同一函数）
    Centre(wxBOTH);
}

// T-140 / THE-030：本窗直接持有的表面重新从 UiTheme token 取用。
// 未迁移：左栏的 Stepper / 契约摘要卡 / RuntimePanel 三个内部面板与若干提示文字
// 仍是迁移前取值（见 docs/GUI-CHUGAO/_evidence/T140-theme-adapter-map.md）。
void TraceBridgeWindow::ApplyTheme() {
    // T-140 夹具 F-140-01/02 修复：跨档 token 重映射（本窗 46 个文字面里仅 5 个曾随档更新）
    UiTheme::RemapStaleTokenColours(this);
    const UiTheme::Colours& ui = UiTheme::Palette();
    SetBackgroundColour(ui.bgWindow);
    if (splitter_) splitter_->SetBackgroundColour(ui.bgWindow);
    if (leftPanel_) leftPanel_->SetBackgroundColour(ui.bgWindow);
    if (rightPanel_) rightPanel_->SetBackgroundColour(ui.bgPanel);
    if (stepperStatus_) stepperStatus_->SetForegroundColour(ui.text2);
    if (waveHint_) waveHint_->SetForegroundColour(ui.text2);
    if (diffReportText_) {
        diffReportText_->SetBackgroundColour(ui.sunkenOnCanvas);
        diffReportText_->SetForegroundColour(ui.text1);
    }
    // 未显式取色的文字面（系统默认黑）补当期文字 token（PLT-012）
    ApplyTextForegroundDefaults(this, ui.text2);
    Refresh();
}

void TraceBridgeWindow::BuildLeftPanel(wxPanel* left) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->AddSpacer(6);
    // stepper
    stepper_ = new TraceBridgeStepStepper(left);
    root->Add(stepper_, 0, wxLEFT | wxRIGHT | wxEXPAND, 10);
    stepperStatus_ = new wxStaticText(left, wxID_ANY, wxT("就绪：请先加载 Debug Contract"));
    stepperStatus_->SetForegroundColour(UiTheme::Palette().text2);
    root->Add(stepperStatus_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

    // Notebook Tab 1/2
    auto* nb = new wxNotebook(left, wxID_ANY);
    // Tab 1 : Contract
    {
        auto* wrap = new wxPanel(nb, wxID_ANY);
        auto* v = new wxBoxSizer(wxVERTICAL);
        v->AddSpacer(6);
        auto* br = new wxStaticBoxSizer(wxHORIZONTAL, wrap, wxT("Debug Contract"));
        auto* browseBtn = new wxButton(wrap, wxID_ANY, wxT("浏览…"));
        auto* reloadBtn = new wxButton(wrap, wxID_ANY, wxT("重新加载"));
        br->AddStretchSpacer();
        br->Add(browseBtn, 0, wxALL, 6);
        br->Add(reloadBtn, 0, wxUP | wxDOWN | wxRIGHT, 6);
        v->Add(br, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        summaryCard_ = new TraceBridgeContractSummaryCard(wrap);
        summaryCard_->browseBtn = browseBtn;
        summaryCard_->reloadBtn = reloadBtn;
        v->Add(summaryCard_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        auto* probeBox = new wxStaticBoxSizer(wxVERTICAL, wrap, wxT("探针列表（每行：信号路径 [位宽]）"));
        probeEditor_ = new wxTextCtrl(wrap, wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, FromDIP(wxSize(-1, 130)),
                                      wxTE_MULTILINE | wxTE_DONTWRAP);
        probeEditor_->SetHint(wxT("例如：\ndut.state 4 clk_a\ndut.valid 1 clk_b\n# 格式：信号路径 [位宽] [时钟域]"));
        // T-140 F-140-01：本控件建在窗口构造期 ApplyTheme() **之后**，且原生默认前景是纯黑
        //（#000000 不是任何一档 token，跨档重映射看不见它）→ 深色启动下"黑字深底"不可读。
        // 显式给当档 token（正文色 + 输入面底），两档都可读并进入后续刷新遍历。
        probeEditor_->SetForegroundColour(UiTheme::Palette().text1);
        probeEditor_->SetBackgroundColour(UiTheme::Palette().sunkenOnPanel);
        probeBox->Add(probeEditor_, 1, wxALL | wxEXPAND, 6);
        auto* planProbes = new wxButton(wrap, wxID_ANY, wxT("探针规划…"));
        probeBox->Add(planProbes, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, 6);
        planProbes->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnPlanProbes, this);
        v->Add(probeBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        auto* intentHint = new wxStaticText(wrap, wxID_ANY,
            wxT("意图触发器在“运行时”页配置；开启 Override 后可选择状态停滞或握手超时，\n"
                "需要持久化配置时，请使用 FPGA 菜单中的 debug_contract 配置界面。"));
        intentHint->SetForegroundColour(UiTheme::Palette().text3);
        v->Add(intentHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        auto* guidanceBox = new wxStaticBoxSizer(wxHORIZONTAL, wrap, wxT("首次采集与诊断"));
        captureWizardButton_ = new wxButton(wrap, wxID_ANY, wxT("首次采集向导…"));
        recoveryPlanButton_ = new wxButton(wrap, wxID_ANY, wxT("恢复建议…"));
        templateHelpButton_ = new wxButton(wrap, wxID_ANY, wxT("模板帮助…"));
        monitorSummaryButton_ = new wxButton(wrap, wxID_ANY, wxT("监控现场…"));
        guidanceBox->Add(captureWizardButton_, 0, wxALL, 4);
        guidanceBox->Add(recoveryPlanButton_, 0, wxTOP | wxBOTTOM | wxRIGHT, 4);
        guidanceBox->Add(templateHelpButton_, 0, wxTOP | wxBOTTOM | wxRIGHT, 4);
        guidanceBox->Add(monitorSummaryButton_, 0, wxTOP | wxBOTTOM | wxRIGHT, 4);
        v->Add(guidanceBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
        v->AddStretchSpacer();
        wrap->SetSizer(v);
        nb->AddPage(wrap, wxT("1 契约"), true);
        browseBtn->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnBrowseContract, this);
        reloadBtn->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnReloadContract, this);
        captureWizardButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnOpenCaptureWizard, this);
        recoveryPlanButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnShowRecoveryPlan, this);
        templateHelpButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnShowTemplateHelp, this);
        monitorSummaryButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnOpenMonitorSummary, this);
    }
    // Tab 2 : Runtime
    {
        runtimePanel_ = new TraceBridgeRuntimePanel(nb);
        runtimePanel_->refreshBtn_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnRefreshPorts, this);
        runtimePanel_->selfTestButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnSelfTest, this);
        nb->AddPage(runtimePanel_, wxT("2 运行时"), false);
    }
    root->Add(nb, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    // progress bar + cancel
    auto* progBox = new wxStaticBoxSizer(wxHORIZONTAL, left, wxT("采集进度"));
    progressGauge_ = new wxGauge(left, wxID_ANY, 100);
    progressGauge_->SetValue(0);
    cancelCaptureButton_ = new wxButton(left, wxID_ANY, wxT("取消"));
    cancelCaptureButton_->Enable(false);
    progBox->Add(progressGauge_, 1, wxALL | wxALIGN_CENTER_VERTICAL, 8);
    progBox->Add(cancelCaptureButton_, 0, wxUP | wxDOWN | wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
    root->Add(progBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    cancelCaptureButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnCancelCapture, this);

    // log list
    auto* logBox = new wxStaticBoxSizer(wxVERTICAL, left, wxT("运行日志"));
    logList_ = new wxListCtrl(left, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 200)),
                              wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    logList_->AppendColumn(wxT("时间"), wxLIST_FORMAT_LEFT, FromDIP(100));
    logList_->AppendColumn(wxT("TAG"),  wxLIST_FORMAT_LEFT, FromDIP(70));
    logList_->AppendColumn(wxT("消息"),  wxLIST_FORMAT_LEFT, FromDIP(400));
    logBox->Add(logList_, 1, wxALL | wxEXPAND, 6);
    root->Add(logBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    // session list + export
    auto* sessBox = new wxStaticBoxSizer(wxHORIZONTAL, left, wxT("历史会话"));
    sessionChoice_ = new wxChoice(left, wxID_ANY);
    refreshSessionsButton_ = new wxButton(left, wxID_ANY, wxT("↻"));
    exportButton_ = new wxButton(left, wxID_ANY, wxT("导出…"));
    importButton_ = new wxButton(left, wxID_ANY, wxT("导入…"));
    exportButton_->Enable(false);
    sessBox->Add(sessionChoice_, 1, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    sessBox->Add(refreshSessionsButton_, 0, wxUP | wxDOWN | wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    sessBox->Add(exportButton_, 0, wxUP | wxDOWN | wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    sessBox->Add(importButton_, 0, wxUP | wxDOWN | wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    root->Add(sessBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    // primary actions
    auto* primaryActions = new wxBoxSizer(wxHORIZONTAL);
    startCaptureButton_ = new wxButton(left, wxID_ANY, wxT("开始采集"));
    buildDebugButton_ = new wxButton(left, wxID_ANY, wxT("构建调试位流"));
    buildProgramCaptureButton_ = new wxButton(left, wxID_ANY, wxT("构建、下载并采集"));
    startCaptureButton_->SetMinSize(FromDIP(wxSize(140, 36)));
    buildDebugButton_->SetMinSize(FromDIP(wxSize(140, 36)));
    buildProgramCaptureButton_->SetMinSize(FromDIP(wxSize(160, 36)));
    primaryActions->Add(startCaptureButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
    primaryActions->Add(buildDebugButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
    primaryActions->Add(buildProgramCaptureButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
    primaryActions->AddStretchSpacer();
    root->Add(primaryActions, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    // secondary actions
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    openHwVcdButton_ = new wxButton(left, wxID_ANY, wxT("打开 HW VCD"));
    openHwVcdButton_->Enable(false);
    compareSimButton_ = new wxButton(left, wxID_ANY, wxT("比对仿真 VCD…"));
    compareSimButton_->Enable(false);
    generateReplayButton_ = new wxButton(left, wxID_ANY, wxT("生成重放 VCD"));
    generateReplayButton_->Enable(false);
    actions->Add(openHwVcdButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    actions->Add(compareSimButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    actions->Add(generateReplayButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    actions->AddStretchSpacer();
    root->Add(actions, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    left->SetSizer(root);

    startCaptureButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnStartCapture, this);
    buildDebugButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnBuildDebugBitstream, this);
    buildProgramCaptureButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnBuildProgramCapture, this);
    openHwVcdButton_->Bind(wxEVT_BUTTON,   &TraceBridgeWindow::OnOpenCaptureVcd, this);
    compareSimButton_->Bind(wxEVT_BUTTON,  &TraceBridgeWindow::OnCompareSimVcd,   this);
    generateReplayButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnGenerateReplay, this);
    refreshSessionsButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnRefreshSessions, this);
    exportButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnExportSession, this);
    importButton_->Bind(wxEVT_BUTTON, &TraceBridgeWindow::OnImportSession, this);
    sessionChoice_->Bind(wxEVT_CHOICE, &TraceBridgeWindow::OnSessionSelected, this);

    AddLog(TbLogTag::INFO, wxT("TraceBridge UI v2 启动：异步采集 + 取消 + 双轨比对 + 会话管理已就绪"));
}

void TraceBridgeWindow::BuildRightPanel(wxPanel* right) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    rightNotebook_ = new wxNotebook(right, wxID_ANY);

    // Tab 1: HW / Sim 双轨同屏
    {
        auto* dualPage = new wxPanel(rightNotebook_, wxID_ANY);
        auto* dualRoot = new wxBoxSizer(wxVERTICAL);
        auto* dualSplitter = new wxSplitterWindow(dualPage, wxID_ANY,
                                                   wxDefaultPosition, wxDefaultSize,
                                                   wxSP_LIVE_UPDATE);
        auto* hwPage = new wxPanel(dualSplitter, wxID_ANY);
        auto* simPage = new wxPanel(dualSplitter, wxID_ANY);
        auto* hwSizer = new wxBoxSizer(wxVERTICAL);
        auto* simSizer = new wxBoxSizer(wxVERTICAL);
        hwWavePanel_ = new sigflow::wave::TraceViewPanel(hwPage);
        simWavePanel_ = new sigflow::wave::TraceViewPanel(simPage);
        if (navigationCallback_) {
            hwWavePanel_->SetNavigationCallback(navigationCallback_);
            simWavePanel_->SetNavigationCallback(navigationCallback_);
        }
        hwSizer->Add(new wxStaticText(hwPage, wxID_ANY, wxT("硬件捕获")), 0, wxLEFT | wxTOP, 5);
        hwSizer->Add(hwWavePanel_, 1, wxALL | wxEXPAND, 2);
        simSizer->Add(new wxStaticText(simPage, wxID_ANY, wxT("仿真 / 重放")), 0, wxLEFT | wxTOP, 5);
        simSizer->Add(simWavePanel_, 1, wxALL | wxEXPAND, 2);
        hwPage->SetSizer(hwSizer);
        simPage->SetSizer(simSizer);
        dualSplitter->SplitVertically(hwPage, simPage, -1);
        dualRoot->Add(dualSplitter, 1, wxALL | wxEXPAND, 2);
        dualPage->SetSizer(dualRoot);
        rightNotebook_->AddPage(dualPage, wxT("双轨波形"), true);
    }
    // Tab 3: 比对结果
    {
        auto* diffPage = new wxPanel(rightNotebook_, wxID_ANY);
        auto* dv = new wxBoxSizer(wxVERTICAL);
        waveHint_ = new wxStaticText(diffPage, wxID_ANY,
            wxT("点击左下角【开始采集】完成 HW 捕获，HW 完成后可选择仿真 VCD 开始双轨比对。"));
        waveHint_->SetForegroundColour(UiTheme::Palette().text2);
        waveHint_->Wrap(FromDIP(420));
        dv->Add(waveHint_, 0, wxALL, 10);
        diffReportText_ = new wxTextCtrl(diffPage, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH);
        diffReportText_->SetValue(wxT("比对结果将在选择仿真 VCD 后显示。"));
        diffReportText_->SetBackgroundColour(UiTheme::Palette().sunkenOnCanvas);
        dv->Add(diffReportText_, 1, wxALL | wxEXPAND, 10);
        diffPage->SetSizer(dv);
        rightNotebook_->AddPage(diffPage, wxT("比对结果"), false);
    }
    root->Add(rightNotebook_, 1, wxALL | wxEXPAND, 4);
    right->SetSizer(root);
}

void TraceBridgeWindow::SetProjectContext(const wxString& projectPath) {
    if (projectPath_ == projectPath) return;
    projectPath_ = projectPath;
    lastCaptureHwVcd_.clear();
    lastCaptureSimVcd_.clear();
    openHwVcdButton_->Enable(false);
    compareSimButton_->Enable(false);
    generateReplayButton_->Enable(false);
    LoadDefaultContract();
}

void TraceBridgeWindow::ReloadContract() { LoadDefaultContract(); }

void TraceBridgeWindow::SetCaptureStartHandler(std::function<void(const TraceBridgeCaptureRequest&)> handler) {
    captureStartHandler_ = std::move(handler);
}

void TraceBridgeWindow::SetDebugBuildStartHandler(
    std::function<void(const TraceBridgeDebugBuildRequest&)> handler) {
    debugBuildStartHandler_ = std::move(handler);
}

void TraceBridgeWindow::SetOpenVcdHandler(std::function<void(const wxString&)> handler) {
    openVcdHandler_ = std::move(handler);
}

void TraceBridgeWindow::SetNavigationCallback(
    sigflow::wave::WaveNavigationCallback callback) {
    navigationCallback_ = std::move(callback);
    if (hwWavePanel_) hwWavePanel_->SetNavigationCallback(navigationCallback_);
    if (simWavePanel_) simWavePanel_->SetNavigationCallback(navigationCallback_);
}

// 进度语义：只更新"当前阶段状态"标签，**不写事件日志列表**。
// 原实现每条进度都 AddLog(TbLogTag::INFO, message)，而回读样本阶段是
// 「每 1 个样本一条进度」（ReadCapture 按 kMaxSamples=1 分块），
// 一次采集会往日志列表塞上千/上万条；wxListCtrl 在 wxGTK 下是 generic 实现
// （插入 + 逐行上色 + DeleteItem(0) 裁剪 + EnsureVisible 滚动重绘），
// 界面会被拖死。进度属于"状态"而非"事件"，因此只更新标签。
// 阶段切换与最终结果仍由调用方显式记录（见 OnBuildProgramCapture 的进度回调）。
void TraceBridgeWindow::SetCaptureStatus(const wxString& message) {
    if (!wxIsMainThread()) {
        CallAfter([this, message] { SetCaptureStatus(message); });
        return;
    }
    stepperStatus_->SetLabel(message);
}

void TraceBridgeWindow::SetCaptureResult(bool success, const wxString& message, const wxString& vcdPath) {
    if (!wxIsMainThread()) {
        CallAfter([this, success, message, vcdPath] { SetCaptureResult(success, message, vcdPath); });
        return;
    }
    progressGauge_->SetValue(success ? 100 : 0);
    startCaptureButton_->Enable(true);
    cancelCaptureButton_->Enable(false);
    stepperStatus_->SetLabel(message);
    if (success) {
        AddLog(TbLogTag::OK, message);
        if (!vcdPath.IsEmpty()) {
            lastCaptureHwVcd_ = vcdPath;
            openHwVcdButton_->Enable(true);
            compareSimButton_->Enable(true);
            generateReplayButton_->Enable(true);
            if (hwWavePanel_) hwWavePanel_->OpenTrace(std::string(vcdPath.ToUTF8().data()));
            if (rightNotebook_) rightNotebook_->SetSelection(0); // 切到 HW 波形 Tab
            if (waveHint_) waveHint_->SetLabel(wxT("✅ HW 捕获完成：") + vcdPath +
                wxT("\n现在可以点【比对仿真 VCD…】选择仿真输出，查看双轨差异。"));
            // 从 vcdPath 中提取 sessionId：.../.sigflow/debug/<id>/artifacts/capture.vcd
            {
                // 统一成 '/' 再查找。原实现先把 '/' 换成 '\\' 再找 "\\.sigflow\\debug\\"，
                // 在 Linux 上路径本来就是 '/'，替换后反而永远匹配不到 → lastSessionId_ 取不到。
                wxString path = vcdPath;
                path.Replace(wxT("\\"), wxT("/"));
                const wxString marker = wxT("/.sigflow/debug/");
                size_t pos = path.rfind(marker);
                if (pos != wxString::npos) {
                    wxString rest = path.Mid(pos + marker.length());
                    size_t sep = rest.find(wxT("/"));
                    if (sep != wxString::npos) {
                        lastSessionId_ = rest.Left(static_cast<std::size_t>(sep));
                    }
                }
            }
            RefreshSessionList();
        }
        if (stepper_) stepper_->SetAllDone();
    } else {
        AddLog(TbLogTag::ERR, message);
    }
    JoinWorkerIfAny();
}

void TraceBridgeWindow::SetDebugWorkflowResult(bool success, const wxString& message) {
    if (!wxIsMainThread()) {
        CallAfter([this, success, message] { SetDebugWorkflowResult(success, message); });
        return;
    }
    startCaptureButton_->Enable(true);
    buildDebugButton_->Enable(true);
    buildProgramCaptureButton_->Enable(true);
    cancelCaptureButton_->Enable(false);
    stepperStatus_->SetLabel(message);
    AddLog(success ? TbLogTag::OK : TbLogTag::ERR, message);
    if (success) pendingProbeBuild_ = false;
}

void TraceBridgeWindow::LoadDefaultContract() {
    loadingContract_ = true;
    contract_ = sigflow::debug::DebugContract();
    pendingProbeBuild_ = false;
    runtimePanel_->ReflectContract(contract_);
    if (probeEditor_) probeEditor_->Clear();
    RefreshContractSummaryUi();
    loadingContract_ = false;
    if (projectPath_.IsEmpty()) return;
    std::vector<wxString> candidates;
    candidates.push_back(wxFileName(projectPath_, wxT("debug-contract.json")).GetFullPath());
    candidates.push_back(wxFileName(wxFileName(projectPath_, wxT(".sigflow")).GetFullPath(),
                                    wxT("debug-contract.json")).GetFullPath());
    for (const wxString& c : candidates) {
        if (wxFileName::FileExists(c)) { LoadContractFromPath(c); return; }
    }
}

void TraceBridgeWindow::LoadContractFromPath(const wxString& path) {
    if (path.IsEmpty()) {
        AddLog(TbLogTag::ERR, wxT("未指定 Debug Contract 文件。"));
        return;
    }
    if (!wxFileExists(path)) {
        AddLog(TbLogTag::ERR, wxT("契约文件不存在：") + path);
        return;
    }
    wxFile file(path);
    wxString content;
    if (!file.IsOpened() || !file.ReadAll(&content)) {
        AddLog(TbLogTag::ERR, wxT("读契约失败：") + path);
        return;
    }
    sigflow::debug::DebugContract parsed;
    std::string error;
    if (!parsed.ParseJson(ToUtf8(content), error)) {
        AddLog(TbLogTag::ERR, wxT("契约 JSON 解析失败：") + ToWxString(error));
        return;
    }
    parsed.ApplyDefaults();
    if (!parsed.AssignProbeBitOffsets(error)) {
        AddLog(TbLogTag::ERR, wxT("探针 bit 分配失败：") + ToWxString(error));
        return;
    }
    wxString ve;
    std::string contractErr;
    if (!parsed.Validate(contractErr)) ve = ToWxString(contractErr);
    loadingContract_ = true;
    contract_ = std::move(parsed);
    pendingProbeBuild_ = false;
    contractFilePath_ = path;
    runtimePanel_->ReflectContract(contract_);
    if (probeEditor_) {
        wxString text;
        for (const auto& probe : contract_.probes) {
            text += ToWxString(probe.path);
            if (probe.width != 1) text += wxString::Format(wxT(" %u"), probe.width);
            if (!probe.clockDomain.empty()) text += wxT(" ") + ToWxString(probe.clockDomain);
            text += wxT("\n");
        }
        probeEditor_->SetValue(text);
    }
    stepper_->SetActive(1, wxT("契约已加载：") + wxFileName(path).GetFullName());
    RefreshContractSummaryUi();
    // override panel data to summary card contractPath + error
    summaryCard_->UpdateFrom(contract_, path, ve);
    loadingContract_ = false;
    AddLog(TbLogTag::INFO, wxT("已加载契约：") + path);
}

void TraceBridgeWindow::RefreshContractSummaryUi() {
    // No-op if empty path; but summary card keeps last display.
}

void TraceBridgeWindow::RefreshSerialPorts() {
    auto* choice = runtimePanel_->serialChoice_;
    const wxString prev = choice->GetCount() > 0 && choice->GetSelection() != wxNOT_FOUND
                              ? (dynamic_cast<wxStringClientData*>(choice->GetClientObject(choice->GetSelection()))
                                     ? dynamic_cast<wxStringClientData*>(choice->GetClientObject(choice->GetSelection()))->GetData()
                                     : wxString{})
                              : wxString{};
    choice->Clear();
    // 自动选口策略：**必须按接口号，不能只看名字**。
    // 双通道调试器（Tang Nano 全系板载 BL702 伪装 FT2232、FT2232/FT4232 等）
    // 会暴露两个串口：接口 0 = JTAG，接口 1 = UART。采集协议走 UART ——
    // 若选中接口 0，同步前导帧发出去一个字节都收不到（response timeout）。
    // 旧实现只按 friendlyName 关键字（sipeed/tang/ft2232）匹配，而两个通道的
    // by-id 名字都含 SIPEED_...，于是取了枚举到的第一个（通常是接口 0）→ 必超时。
    int bestIdx = wxNOT_FOUND;
    int bestScore = -100000;   // 保证至少有一个候选（即使它是接口 0）
    for (const auto& p : sigflow::debug::EnumerateSerialPorts()) {
        wxString label = ToWxString(p.name);
        if (!p.friendlyName.empty()) {
            label += wxT(" (") + ToWxString(p.friendlyName) + wxT(")");
        }
        if (p.interfaceNumber >= 0) {
            label += wxString::Format(wxT(" · IF%02d"), p.interfaceNumber);
            if (p.interfaceNumber == 1) label += wxT(" [UART]");
        }
        choice->Append(label, new wxStringClientData(ToWxString(p.name)));

        const wxString fn = ToWxString(p.friendlyName).Lower();
        int score = 0;
        if (p.interfaceNumber == 1) score += 100;   // ★ UART 通道：采集要的就是它
        if (p.interfaceNumber == 0) score -= 100;   // ★ JTAG 通道：绝不能默认选它
        if (fn.Contains(wxT("sipeed")) || fn.Contains(wxT("tang")) ||
            fn.Contains(wxT("ft2232")) || fn.Contains(wxT("ftdi"))) {
            score += 10;
        } else if (fn.Contains(wxT("ch340"))) {
            score += 5;
        }
        if (score > bestScore) {
            bestScore = score;
            bestIdx = static_cast<int>(choice->GetCount() - 1);
        }
    }
    if (!prev.IsEmpty()) {
        for (unsigned i = 0; i < choice->GetCount(); ++i) {
            auto* cd = dynamic_cast<wxStringClientData*>(choice->GetClientObject(i));
            if (cd && cd->GetData() == prev) { choice->SetSelection(i); break; }
        }
    }
    if (choice->GetSelection() == wxNOT_FOUND) {
        if (bestIdx != wxNOT_FOUND) choice->SetSelection(bestIdx);
        else if (choice->GetCount() == 1) choice->SetSelection(0);
    }
    AddLog(TbLogTag::INFO, wxString::Format(wxT("枚举串口：共 %u 个"),
                                            static_cast<unsigned>(choice->GetCount())));
}

void TraceBridgeWindow::OnBrowseContract(wxCommandEvent&) {
    wxFileDialog dlg(this, wxT("选择 Debug Contract"), projectPath_,
                     wxT("debug-contract.json"), wxT("JSON files (*.json)|*.json|All files (*.*)|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) LoadContractFromPath(dlg.GetPath());
}

void TraceBridgeWindow::OnReloadContract(wxCommandEvent&) {
    if (contractFilePath_.empty()) { LoadDefaultContract(); return; }
    LoadContractFromPath(contractFilePath_);
}

void TraceBridgeWindow::OnRefreshPorts(wxCommandEvent&) { RefreshSerialPorts(); }

void TraceBridgeWindow::OnSelfTest(wxCommandEvent&)
{
    // 采集按钮是窗口级的忙碌标志；外部 MainFrame 采集也会在整个 run
    // 期间保持它开启，故这里不创建第二条可能争抢 UART 的隐式通道。
    if (cancelCaptureButton_ && cancelCaptureButton_->IsEnabled()) {
        AddLog(TbLogTag::WR, wxT("采集进行中，已拒绝链路自测；请等待采集完成。"));
        return;
    }
    if (!runtimePanel_ || runtimePanel_->serialChoice_->GetSelection() == wxNOT_FOUND) {
        AddLog(TbLogTag::ERR, wxT("链路自测需要先选择串口。"));
        return;
    }

    JoinWorkerIfAny();
    const std::string port = ToUtf8(runtimePanel_->GetSelectedSerialPureName());
    const std::uint32_t baud = static_cast<std::uint32_t>(runtimePanel_->baudSpin_->GetValue());
    const std::string protocol = ToUtf8(runtimePanel_->protoChoice_->GetStringSelection());
    const int timeoutMs = runtimePanel_->timeoutSpin_->GetValue();
    const bool checkFingerprint = runtimePanel_->checkFpCk_->GetValue();
    runtimePanel_->selfTestButton_->Disable();
    startCaptureButton_->Disable();
    buildDebugButton_->Disable();
    buildProgramCaptureButton_->Disable();
    AddLog(TbLogTag::CFG, wxT("用户主动链路自测开始：") + ToWxString(port) +
           wxT(" / ") + ToWxString(protocol));
    runtimePanel_->SetSelfTestStatus(wxT("后端：连接中 · 目标：") + ToWxString(port) +
                                     wxT(" · 验证级别：用户主动只读握手"));

    const wxWeakRef<TraceBridgeWindow> weakWindow(this);
    worker_ = std::make_unique<std::thread>(
        [this, weakWindow, port, baud, protocol, timeoutMs, checkFingerprint]() {
            sigflow::debug::TransportSelfTestResult result;
            result.protocol = protocol;
            bool passed = false;
            std::string openError;
            sigflow::debug::DeviceControlLease lease;
            if (!sigflow::debug::DeviceControlLease::Acquire(
                    "tracebridge-hardware", "tracebridge-self-test", lease, openError)) {
                result.status = sigflow::debug::TransportSelfTestStatus::CaptureActive;
                result.error = openError;
                result.diagnostics.push_back("DEVICE_BUSY: " + openError);
            } else {
                const sigflow::debug::TransportOpenOptions openOptions{
                    port, baud, true, true};
                sigflow::debug::TransportOpenResult openResult;
                if (!sigflow::debug::OpenDebugTransport(openOptions, openResult, openError)) {
                    result.status = sigflow::debug::TransportSelfTestStatus::Failed;
                    result.error = openError;
                    result.diagnostics = openResult.diagnostics;
                    result.diagnostics.push_back("OPEN_FAILED: " + openError);
                } else {
                    result.backend = openResult.backendId;
                    sigflow::debug::TransportSelfTestOptions options;
                    options.userInitiated = true;
                    options.captureActive = false;
                    options.protocol = protocol;
                    options.timeoutMs = timeoutMs;
                    options.attempts = 2;
                    passed = sigflow::debug::TransportSelfTestService::Run(
                        *openResult.transport, options, result);
                    result.backend = openResult.backendId;
                    openResult.transport->Close();
                }
            }

            if (!wxTheApp) return;
            wxTheApp->CallAfter([weakWindow, passed, result, baud, checkFingerprint]() {
                if (!weakWindow) return;
                auto* window = weakWindow.get();
                window->runtimePanel_->selfTestButton_->Enable(true);
                window->startCaptureButton_->Enable(true);
                window->buildDebugButton_->Enable(true);
                window->buildProgramCaptureButton_->Enable(true);
                std::ostringstream summary;
                summary << "链路自测 " << (passed ? "通过" : "失败")
                        << " [" << sigflow::debug::ToString(result.status) << "]"
                        << " op=" << result.operation
                        << " attempts=" << result.protocolStats.attempts
                        << " retries=" << result.protocolStats.retries;
                if (!result.backend.empty()) summary << " backend=" << result.backend;
                if (!result.error.empty()) summary << " error=" << result.error;
                std::ostringstream uiStatus;
                uiStatus << "后端：" << (result.backend.empty() ? "未打开" : result.backend)
                         << " · 目标：" << (result.backend.empty() ? "未验证" : result.backend)
                         << " · 协议：" << result.protocol
                         << " · 波特率：" << baud
                         << " · 验证级别：用户主动只读/指纹校验="
                         << (checkFingerprint ? "启用" : "关闭")
                         << " · 结果：" << sigflow::debug::ToString(result.status);
                window->runtimePanel_->SetSelfTestStatus(ToWxString(uiStatus.str()));
                window->AddLog(passed ? TbLogTag::OK : TbLogTag::ERR,
                               ToWxString(summary.str()));
                for (const auto& diagnostic : result.diagnostics) {
                    window->AddLog(passed ? TbLogTag::INFO : TbLogTag::WR,
                                  ToWxString(diagnostic));
                }
            });
        });
}

void TraceBridgeWindow::OnOpenCaptureWizard(wxCommandEvent&)
{
    sigflow::debug::TraceBridgeCaptureWizardInput input;
    input.targetReady = !projectPath_.IsEmpty();
    input.targetLabel = projectPath_.IsEmpty()
                            ? std::string()
                            : ToUtf8(wxFileName(projectPath_).GetFullName());
    input.contractReady = !contract_.probes.empty();
    input.impactReportReady = !pendingProbeBuild_;
    input.deviceReady = runtimePanel_ &&
                        runtimePanel_->serialChoice_->GetSelection() != wxNOT_FOUND;
    input.captureActive = cancelCaptureButton_ && cancelCaptureButton_->IsEnabled();
    input.captureComplete = !lastCaptureHwVcd_.IsEmpty();
    input.resultReady = !lastCaptureHwVcd_.IsEmpty();

    sigflow::debug::TraceBridgeCaptureWizardPlan plan;
    std::string error;
    if (!sigflow::debug::TraceBridgeCaptureWizardService::Build(input, plan, error)) {
        AddLog(TbLogTag::ERR, wxT("首次采集向导生成失败：") + ToWxString(error));
        wxMessageBox(ToWxString(error), wxT("首次采集向导"), wxOK | wxICON_ERROR, this);
        return;
    }

    std::ostringstream out;
    out << "TraceBridge 首次采集向导\n"
        << "当前步骤: " << sigflow::debug::ToString(plan.currentStage)
        << "\n可开始采集: " << (plan.readyToStart ? "是" : "否")
        << "\n流程完成: " << (plan.complete ? "是" : "否") << "\n\n";
    for (const auto& step : plan.steps) {
        out << "[" << sigflow::debug::ToString(step.state) << "] "
            << step.id << " -> " << step.navigationTarget << "\n"
            << "  " << step.summary << "\n";
        for (const auto& issue : step.issues) {
            out << "  ! " << issue.code << ": " << issue.observed
                << " | action=" << issue.action << "\n";
        }
    }
    if (!plan.issues.empty()) {
        out << "\n诊断汇总:\n";
        for (const auto& issue : plan.issues) {
            out << "- " << issue.code << ": " << issue.observed
                << " | " << issue.action << "\n";
        }
    }
    out << "\n说明: 向导只编排现有契约、影响报告、设备预检和采集入口，"
           "不会自动打开设备、下载位流或重新布防。";
    ShowTraceBridgeDetails(this, wxT("首次采集向导"), ToWxString(out.str()));
    AddLog(TbLogTag::INFO, wxString::Format(wxT("首次采集向导已生成：当前步骤 %s"),
                                            ToWxString(sigflow::debug::ToString(plan.currentStage))));
}

void TraceBridgeWindow::OnShowRecoveryPlan(wxCommandEvent&)
{
    sigflow::debug::TraceBridgeRecoveryContext context;
    context.captureActive = cancelCaptureButton_ && cancelCaptureButton_->IsEnabled();
    context.hasProbePlan = !contract_.probes.empty();
    context.hasTimingReport = false;
    context.ownsControl = context.captureActive;

    const std::string port = runtimePanel_ ? ToUtf8(runtimePanel_->GetSelectedSerialPureName()) : "";
    context.deviceAvailable = !port.empty();
    if (!context.deviceAvailable) {
        sigflow::debug::DiagnosticIssue issue;
        issue.code = "DEVICE_MISSING";
        issue.stage = "device";
        issue.severity = sigflow::debug::DiagnosticSeverity::Blocking;
        issue.observed = "no serial target is selected";
        issue.action = "refresh ports and select the intended target";
        context.issues.push_back(std::move(issue));
    }
    if (pendingProbeBuild_) {
        sigflow::debug::DiagnosticIssue issue;
        issue.code = "PROBE_BUDGET_EXCEEDED";
        issue.stage = "contract";
        issue.severity = sigflow::debug::DiagnosticSeverity::Blocking;
        issue.observed = "the edited probe plan has not been built and downloaded";
        issue.action = "review the probe plan, then build and download before capture";
        context.issues.push_back(std::move(issue));
    }

    sigflow::debug::TransportOpenOptions transport;
    if (runtimePanel_) {
        transport.serialPort = port;
        transport.baud = static_cast<std::uint32_t>(runtimePanel_->baudSpin_->GetValue());
        transport.preferFtdi = runtimePanel_->protoChoice_->GetStringSelection() == wxT("full");
        transport.allowSerialFallback = true;
    }
    sigflow::debug::PreflightInput preflightInput;
    preflightInput.contract = &contract_;
    preflightInput.transport = &transport;
    preflightInput.offlineOnly = false;
    auto registry = sigflow::debug::MakeTraceBridgePreflightRegistry();
    sigflow::debug::PreflightReport preflightReport;
    std::string preflightError;
    if (registry.Run(preflightInput, preflightReport, preflightError)) {
        context.issues.insert(context.issues.end(), preflightReport.issues.begin(),
                              preflightReport.issues.end());
    } else if (!preflightError.empty()) {
        AddLog(TbLogTag::WR, wxT("恢复建议预检未完成：") + ToWxString(preflightError));
    }

    sigflow::debug::TraceBridgeRecoveryPlan plan;
    std::string error;
    if (!sigflow::debug::TraceBridgeRecoveryService::Build(context, plan, error)) {
        AddLog(TbLogTag::ERR, wxT("恢复建议生成失败：") + ToWxString(error));
        wxMessageBox(ToWxString(error), wxT("恢复建议"), wxOK | wxICON_ERROR, this);
        return;
    }

    std::ostringstream out;
    out << "TraceBridge 恢复建议\n"
        << "采集活动: " << (context.captureActive ? "是" : "否")
        << " | 设备可达: " << (context.deviceAvailable ? "是" : "否")
        << " | 本任务持有控制权: " << (context.ownsControl ? "是" : "否") << "\n\n";
    if (plan.actions.empty()) {
        out << "当前诊断没有可生成的恢复动作。请先运行预检或执行一次采集。\n";
    }
    for (const auto& action : plan.actions) {
        out << (action.available ? "[可用] " : "[不可用] ") << action.id << "\n"
            << "  " << action.summary << "\n"
            << "  需要确认: " << (action.requiresUserConfirmation ? "是" : "否")
            << " | 改变 DUT/采集状态: " << (action.changesDut ? "是" : "否") << "\n";
        if (!action.issueCodes.empty()) {
            out << "  关联诊断: ";
            for (std::size_t i = 0; i < action.issueCodes.size(); ++i) {
                if (i) out << ", ";
                out << action.issueCodes[i];
            }
            out << "\n";
        }
    }
    out << "\n说明: 当前入口只生成计划，不释放租约、不修改契约、不向 DUT 发送 ARM/RESET。";
    ShowTraceBridgeDetails(this, wxT("恢复建议"), ToWxString(out.str()));
    AddLog(TbLogTag::INFO, wxString::Format(wxT("恢复建议已生成：%u 个动作"),
                                            static_cast<unsigned>(plan.actions.size())));
}

void TraceBridgeWindow::OnShowTemplateHelp(wxCommandEvent&)
{
    const auto& entries = sigflow::debug::TraceBridgeTemplateCatalog::All();
    wxArrayString choices;
    for (const auto& entry : entries) choices.Add(ToWxString(entry.title));
    wxSingleChoiceDialog chooser(this, wxT("选择要查看的观测模板"), wxT("模板帮助"), choices);
    if (chooser.ShowModal() != wxID_OK) return;
    const int selected = chooser.GetSelection();
    if (selected < 0 || static_cast<std::size_t>(selected) >= entries.size()) return;
    const auto& entry = entries[static_cast<std::size_t>(selected)];

    std::ostringstream out;
    out << entry.title << " (" << entry.id << ")\n"
        << "帮助目标: " << entry.helpTarget << "\n\n"
        << "建议观测信号: ";
    for (std::size_t i = 0; i < entry.suggestedSignals.size(); ++i) {
        if (i) out << ", ";
        out << entry.suggestedSignals[i];
    }
    out << "\n\n判断依据:\n" << entry.basis
        << "\n\n预期结果:\n" << entry.expectedResult
        << "\n\n不能单独判断:\n" << entry.limitations
        << "\n\n说明: 模板只提供建议，不自动修改 Debug Contract 或 RTL。";
    ShowTraceBridgeDetails(this, wxT("模板帮助 · ") + ToWxString(entry.title),
                           ToWxString(out.str()));
    AddLog(TbLogTag::INFO, wxT("已查看 TraceBridge 模板：") + ToWxString(entry.id));
}

void TraceBridgeWindow::OnOpenMonitorSummary(wxCommandEvent&)
{
    wxFileDialog dialog(this, wxT("选择 TraceBridge monitor.json"), projectPath_,
                       wxT("monitor.json"),
                       wxT("TraceBridge monitor (*.json)|*.json|All files (*.*)|*.*"),
                       wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;

    sigflow::debug::MonitorSummary summary;
    std::string error;
    if (!sigflow::debug::MonitorSummaryService::Load(ToUtf8(dialog.GetPath()), summary, error)) {
        AddLog(TbLogTag::ERR, wxT("读取监控现场失败：") + ToWxString(error));
        wxMessageBox(ToWxString(error), wxT("监控现场"), wxOK | wxICON_ERROR, this);
        return;
    }

    const auto mb = [](std::uint64_t bytes) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1)
            << (static_cast<long double>(bytes) / (1024.0L * 1024.0L)) << " MiB";
        return out.str();
    };
    std::ostringstream out;
    out << "TraceBridge 监控现场摘要\n"
        << "monitor: " << summary.plan.monitorId << "\n"
        << "状态: " << sigflow::debug::ToString(summary.checkpoint.state)
        << " | 下一个序号: " << summary.checkpoint.nextSequence << "\n"
        << "采集计划: " << summary.plan.captureRunId
        << " | 周期: " << summary.plan.intervalMs << " ms\n"
        << "最近现场: " << summary.checkpoint.lastCaptureId
        << " | 更新时间: " << summary.checkpoint.updatedAtUtc << "\n"
        << "事件日志: " << (summary.eventLogValid ? "有效" : "存在问题")
        << " | 最近事件: " << summary.recentEvents.size() << "\n"
        << "监控目录大小: " << mb(summary.totalBytes) << "\n";
    if (summary.storageSampled) {
        out << "存储可用: " << mb(summary.storage.availableBytes)
            << " / 容量 " << mb(summary.storage.capacityBytes) << "\n";
    } else {
        out << "存储可用: 无法读取\n";
    }
    out << "incident 数量: " << summary.incidents.size() << "\n\n";

    if (!summary.diagnostics.empty()) {
        out << "诊断:\n";
        for (const auto& diagnostic : summary.diagnostics) out << "- " << diagnostic << "\n";
        out << "\n";
    }
    if (!summary.incidents.empty()) {
        out << "Incident:\n";
        for (const auto& incident : summary.incidents) {
            out << "- " << incident.snapshotId;
            if (!incident.incidentId.empty()) out << " / " << incident.incidentId;
            if (!incident.triggerType.empty()) out << " / " << incident.triggerType;
            if (!incident.message.empty()) out << " / " << incident.message;
            out << "\n  " << incident.path << "\n";
        }
        out << "\n";
    }
    out << "最近事件:\n";
    if (summary.recentEvents.empty()) {
        out << "- 暂无事件\n";
    } else {
        for (const auto& event : summary.recentEvents) {
            out << "- #" << event.sequence << " " << event.type
                << " @ " << event.observedAtUtc;
            if (!event.captureId.empty()) out << " capture=" << event.captureId;
            if (!event.message.empty()) out << " : " << event.message;
            out << "\n";
        }
    }
    out << "\n说明: 该页面只读取 monitor.json、checkpoint.json、events.jsonl 和 incident 索引，"
           "不会启动/暂停监控、修改状态或执行清理。";
    ShowTraceBridgeDetails(this, wxT("监控现场"), ToWxString(out.str()));
    AddLog(TbLogTag::INFO, wxT("已加载监控现场：") + dialog.GetPath());
}

void TraceBridgeWindow::OnPlanProbes(wxCommandEvent&) {
    if (cancelCaptureButton_->IsEnabled() || !buildProgramCaptureButton_->IsEnabled()) {
        AddLog(TbLogTag::WR, wxT("请在当前采集/构建流程结束后规划探针。"));
        return;
    }
    sigflow::debug::ProbePlanningRequest planning;
    planning.contract = contract_;
    wxString editorError;
    if (!ParseProbeEditor(probeEditor_->GetValue(), planning.requestedProbes, editorError)) {
        wxMessageBox(editorError, wxT("探针规划"), wxOK | wxICON_WARNING, this);
        return;
    }
    wxDialog dialog(this, wxID_ANY, wxT("探针规划 · 32-bit 单时钟核"), wxDefaultPosition,
                    FromDIP(wxSize(800, 560)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* hint = new wxStaticText(&dialog, wxID_ANY,
        wxT("保留当前契约探针及位序；编辑器中的新增项为必选。源映射需提供完整实例路径、位宽和时钟域。"));
    hint->Wrap(FromDIP(740));
    layout->Add(hint, 0, wxALL | wxEXPAND, FromDIP(10));
    auto* mapRow = new wxBoxSizer(wxHORIZONTAL);
    mapRow->Add(new wxStaticText(&dialog, wxID_ANY, wxT("源映射")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* mapPath = new wxTextCtrl(&dialog, wxID_ANY);
    mapPath->SetHint(wxT("可选 signal-map.json；同目录 dependency-graph.json 用于上游建议"));
    const auto sessionKey = lastSessionId_.IsEmpty() ? ToWxString(contract_.sessionId) : lastSessionId_;
    if (!projectPath_.IsEmpty() && !sessionKey.IsEmpty()) {
        const wxString candidate = JoinPath(JoinPath(JoinPath(JoinPath(projectPath_, ".sigflow"), "debug"), sessionKey), "signal-map.json");
        if (wxFileName::FileExists(candidate)) mapPath->SetValue(candidate);
    }
    mapRow->Add(mapPath, 1, wxRIGHT | wxEXPAND, FromDIP(8));
    auto* browse = new wxButton(&dialog, wxID_ANY, wxT("浏览…"));
    mapRow->Add(browse);
    layout->Add(mapRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(10));
    auto* symptom = new wxTextCtrl(&dialog, wxID_ANY);
    symptom->SetHint(wxT("可选症状信号完整路径，例如 top.uart.state"));
    layout->Add(symptom, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(10));
    auto* rows = new wxListCtrl(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    rows->AppendColumn(wxT("选择"), wxLIST_FORMAT_LEFT, FromDIP(55));
    rows->AppendColumn(wxT("信号路径"), wxLIST_FORMAT_LEFT, FromDIP(220));
    rows->AppendColumn(wxT("位宽"), wxLIST_FORMAT_RIGHT, FromDIP(50));
    rows->AppendColumn(wxT("时钟域"), wxLIST_FORMAT_LEFT, FromDIP(100));
    rows->AppendColumn(wxT("依据 / 未选原因"), wxLIST_FORMAT_LEFT, FromDIP(300));
    layout->Add(rows, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(10));
    auto* summary = new wxTextCtrl(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition,
        FromDIP(wxSize(-1, 90)), wxTE_MULTILINE | wxTE_READONLY);
    layout->Add(summary, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(10));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* preview = new wxButton(&dialog, wxID_ANY, wxT("生成预览"));
    auto* apply = new wxButton(&dialog, wxID_ANY, wxT("应用到探针列表"));
    auto* exportPlan = new wxButton(&dialog, wxID_ANY, wxT("导出 JSON…"));
    buttons->Add(preview, 0, wxRIGHT, FromDIP(8));
    buttons->Add(apply, 0, wxRIGHT, FromDIP(8));
    buttons->Add(exportPlan, 0, wxRIGHT, FromDIP(8));
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(&dialog, wxID_CANCEL, wxT("关闭")));
    layout->Add(buttons, 0, wxALL | wxEXPAND, FromDIP(10));
    dialog.SetSizer(layout);
    sigflow::debug::ProbePlan plan;
    auto generate = [&]() {
        try {
        rows->DeleteAllItems();
        apply->Disable();
        exportPlan->Disable();
        sigflow::debug::SignalSourceMap sourceMap;
        sigflow::debug::DependencyGraph graph;
        const sigflow::debug::SignalSourceMap* mapInput = nullptr;
        const sigflow::debug::DependencyGraph* graphInput = nullptr;
        std::string error;
        planning.symptomSignal = ToUtf8(symptom->GetValue());
        const auto selectedMap = ToUtf8(mapPath->GetValue());
        if (!selectedMap.empty()) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(selectedMap, ec);
            if (ec || size > 1024 * 1024 || !sourceMap.LoadJson(selectedMap, error)) {
                summary->SetValue(error.empty() ? wxT("源映射不可读或超过当前预览的 1 MiB 限制。") : ToWxString(error));
                return false;
            }
            mapInput = &sourceMap;
            const auto graphPath = std::filesystem::path(selectedMap).parent_path() / "dependency-graph.json";
            if (std::filesystem::exists(graphPath, ec)) {
                const auto graphSize = std::filesystem::file_size(graphPath, ec);
                if (ec || graphSize > 1024 * 1024 || !graph.LoadJson(graphPath.string(), error)) {
                    summary->SetValue(error.empty() ? wxT("依赖图不可读或超过 1 MiB。") : ToWxString(error));
                    return false;
                }
                graphInput = &graph;
            }
        }
        if (!sigflow::debug::ProbePlanningService::Plan(planning, mapInput, graphInput, plan, error)) {
            summary->SetValue(ToWxString(error));
            return false;
        }
        for (const auto& entry : plan.entries) {
            const long row = rows->InsertItem(rows->GetItemCount(), entry.selected ? wxT("✓") : wxT("—"));
            rows->SetItem(row, 1, ToWxString(entry.signal));
            rows->SetItem(row, 2, wxString::Format("%u", entry.width));
            rows->SetItem(row, 3, ToWxString(entry.clockDomain));
            rows->SetItem(row, 4, ToWxString(entry.selected ? entry.reason : entry.diagnostic));
        }
        wxString text = wxString::Format(wxT("预算 %zu/%zu bits；%s；%s\n"), plan.usedBits, plan.capacityBits,
            plan.feasible ? wxT("必选项满足") : wxT("存在必选缺口"),
            plan.requiresNewBuild ? wxT("应用后需要重新构建并下载") : wxT("未新增探针"));
        for (const auto& diagnostic : plan.diagnostics) text += ToWxString(diagnostic) + wxT("\n");
        summary->SetValue(text);
        apply->Enable(plan.feasible && plan.requiresNewBuild);
        exportPlan->Enable(true);
        return true;
        } catch (const std::exception& exception) {
            summary->SetValue(wxT("规划输入无效：") + wxString::FromUTF8(exception.what()));
            return false;
        }
    };
    browse->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        wxFileDialog file(&dialog, wxT("选择源映射"), projectPath_, wxT("signal-map.json"),
                          wxT("JSON files (*.json)|*.json"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (file.ShowModal() == wxID_OK) { mapPath->SetValue(file.GetPath()); generate(); }
    });
    preview->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { generate(); });
    exportPlan->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        if (!generate()) return;
        wxFileDialog file(&dialog, wxT("导出探针规划"), projectPath_, wxT("probe-plan.json"),
                          wxT("JSON files (*.json)|*.json"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (file.ShowModal() != wxID_OK) return;
        wxTempFile output(file.GetPath());
        if (!output.IsOpened() || !output.Write(ToWxString(plan.ToJson())) || !output.Commit())
            summary->SetValue(wxT("探针规划保存失败，原文件保持不变。"));
        else AddLog(TbLogTag::OK, wxT("已导出探针规划：") + file.GetPath());
    });
    apply->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        if (!generate() || !plan.feasible || !plan.requiresNewBuild) return;
        sigflow::debug::DebugContract draft;
        std::string error;
        if (!sigflow::debug::ProbePlanningService::BuildDraft(planning.contract, plan, draft, error)) {
            summary->SetValue(ToWxString(error)); return;
        }
        contract_ = std::move(draft);
        wxString text;
        for (const auto& probe : contract_.probes) {
            text += ToWxString(probe.path) + wxString::Format(" %u", probe.width);
            if (!probe.clockDomain.empty()) text += " " + ToWxString(probe.clockDomain);
            text += "\n";
        }
        probeEditor_->SetValue(text);
        pendingProbeBuild_ = true;
        startCaptureButton_->Disable();
        summaryCard_->UpdateFrom(contract_, contractFilePath_);
        AddLog(TbLogTag::WR, wxT("探针规划已应用到内存草案。请构建、下载并采集；原契约文件未改写。"));
        dialog.EndModal(wxID_APPLY);
    });
    generate();
    dialog.CentreOnParent();
    dialog.ShowModal();
}

bool TraceBridgeWindow::BuildDebugBuildRequest(
    TraceBridgeDebugBuildRequest& request, bool programAndCapture, wxString& error) const {
    TraceBridgeCaptureRequest captureRequest;
    if (programAndCapture) {
        if (!BuildCaptureRequest(captureRequest, error)) return false;
    } else {
        if (projectPath_.IsEmpty()) {
            error = wxT("请先打开 SigFlow 项目。");
            return false;
        }
        captureRequest.projectPath = ToUtf8(projectPath_);
        captureRequest.contract = contract_;
        if (probeEditor_) {
            std::vector<sigflow::debug::DebugProbe> probes;
            if (!ParseProbeEditor(probeEditor_->GetValue(), probes, error)) return false;
            captureRequest.contract.probes = std::move(probes);
        }
        captureRequest.contract.ApplyDefaults();
        std::string contractError;
        if (!captureRequest.contract.AssignProbeBitOffsets(contractError) ||
            !captureRequest.contract.Validate(contractError)) {
            error = wxT("调试构建契约校验失败：") + ToWxString(contractError);
            return false;
        }
        wxString runtimeError;
        sigflow::debug::DebugAcquisitionOptions runtimeOptions;
        runtimePanel_->ApplyToRequest(captureRequest.contract, runtimeOptions, runtimeError);
        if (!runtimeError.IsEmpty()) {
            error = runtimeError;
            return false;
        }
        wxString guidance;
        if (!ValidateDebugTransportPins(captureRequest.contract, projectPath_, error, guidance)) {
            return false;
        }
        if (!guidance.IsEmpty()) const_cast<TraceBridgeWindow*>(this)->AddLog(
            TbLogTag::WR, guidance);
        sigflow::debug::PreflightInput preflightInput;
        preflightInput.contract = &captureRequest.contract;
        preflightInput.offlineOnly = true;
        sigflow::debug::PreflightReport preflightReport;
        std::string preflightError;
        auto preflight = sigflow::debug::MakeTraceBridgePreflightRegistry();
        if (!preflight.Run(preflightInput, preflightReport, preflightError)) {
            for (const auto& issue : preflightReport.issues) {
                const_cast<TraceBridgeWindow*>(this)->AddLog(
                    issue.severity == sigflow::debug::DiagnosticSeverity::Blocking
                        ? TbLogTag::ERR : TbLogTag::WR,
                    ToWxString(issue.code + ": " + issue.observed));
            }
            error = wxT("TraceBridge 构建预检失败：") + ToWxString(preflightError);
            return false;
        }
    }

    request.projectPath = captureRequest.projectPath;
    request.contract = captureRequest.contract;
    request.programAndCapture = programAndCapture;
    request.captureRequest = std::move(captureRequest);
    return true;
}

void TraceBridgeWindow::OnBuildDebugBitstream(wxCommandEvent&) {
    if (!debugBuildStartHandler_) {
        AddLog(TbLogTag::ERR, wxT("调试构建服务未连接"));
        return;
    }
    wxString error;
    TraceBridgeDebugBuildRequest request;
    const bool valid = BuildDebugBuildRequest(request, false, error);
    if (!error.IsEmpty()) {
        AddLog(TbLogTag::ERR, error);
        wxMessageBox(error, wxT("调试构建参数错误"), wxOK | wxICON_ERROR, this);
        return;
    }
    if (!valid) return;
    contract_ = request.contract;
    summaryCard_->UpdateFrom(contract_, contractFilePath_);
    AddLog(TbLogTag::CFG, wxT("已提交调试位流构建：将复用 Yosys / nextpnr / gowin_pack"));
    buildDebugButton_->Enable(false);
    debugBuildStartHandler_(request);
    CallAfter([this] { if (buildDebugButton_) buildDebugButton_->Enable(true); });
}

void TraceBridgeWindow::OnBuildProgramCapture(wxCommandEvent&) {
    if (!debugBuildStartHandler_) {
        AddLog(TbLogTag::ERR, wxT("调试构建服务未连接"));
        return;
    }

    TraceBridgeDebugBuildRequest request;
    wxString error;
    if (!BuildDebugBuildRequest(request, true, error)) {
        AddLog(TbLogTag::ERR, error);
        wxMessageBox(error, wxT("构建、下载并采集参数错误"), wxOK | wxICON_ERROR, this);
        return;
    }
    contract_ = request.contract;
    summaryCard_->UpdateFrom(contract_, contractFilePath_);

    m_aborted->store(true);
    m_aborted = std::make_shared<std::atomic<bool>>(false);
    request.captureRequest.options.aborted = m_aborted.get();
    request.captureRequest.abortOwner = m_aborted;
    // 进度回调在 worker 线程触发。
    // 进度只更新状态标签（SetCaptureStatus 内部已 marshal 到 UI 线程）；
    // 日志则**只在阶段切换时**记一条，既能看清流程又不刷屏。
    request.captureRequest.options.onProgress =
        [weakWindow = wxWeakRef<TraceBridgeWindow>(this), lastStage = -1](sigflow::debug::DebugAcqStage stage, int,
                               const std::string& message) mutable {
            const wxString text = ToWxString(message);
            const int order = sigflow::debug::DebugAcquisition::StageOrder(stage);
            const bool changed = order != lastStage;
            lastStage = order;
            if (wxTheApp) wxTheApp->CallAfter([weakWindow, text, changed, stage] {
                if (!weakWindow) return;
                if (!text.IsEmpty()) weakWindow->SetCaptureStatus(text);
                if (changed) PostLog(weakWindow.get(), TbLogTag::INFO,
                    wxString(wxT("阶段: ")) + ToWxString(sigflow::debug::DebugAcquisition::StageName(stage)));
            });
        };
    request.completion = [weakWindow = wxWeakRef<TraceBridgeWindow>(this)](bool success, const wxString& message) {
        if (wxTheApp) wxTheApp->CallAfter([weakWindow, success, message] {
            if (weakWindow) weakWindow->SetDebugWorkflowResult(success, message);
        });
    };

    AddLog(TbLogTag::CFG, wxT("已提交一键流程：构建调试位流 → 下载 FPGA → 开始采集"));
    startCaptureButton_->Enable(false);
    buildDebugButton_->Enable(false);
    buildProgramCaptureButton_->Enable(false);
    cancelCaptureButton_->Enable(false);
    stepper_->Reset();
    stepper_->SetActive(1, wxT("一键流程已启动"));
    stepperStatus_->SetLabel(wxT("正在构建调试位流…"));
    debugBuildStartHandler_(request);
}

void TraceBridgeWindow::OnCancelCapture(wxCommandEvent&) {
    m_aborted->store(true);
    AddLog(TbLogTag::CANCEL, wxT("用户请求取消采集…"));
    cancelCaptureButton_->Enable(false);
}

void TraceBridgeWindow::OnOpenCaptureVcd(wxCommandEvent&) {
    if (!lastCaptureHwVcd_.IsEmpty() && openVcdHandler_) openVcdHandler_(lastCaptureHwVcd_);
}

void TraceBridgeWindow::OnCompareSimVcd(wxCommandEvent&) {
    wxFileDialog dlg(this, wxT("选择仿真 VCD（sim.vcd / test.vcd）"), projectPath_,
                     wxEmptyString, wxT("VCD files (*.vcd)|*.vcd|All files (*.*)|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    lastCaptureSimVcd_ = dlg.GetPath();
    AddLog(TbLogTag::INFO, wxT("已选仿真 VCD：") + lastCaptureSimVcd_);
    if (simWavePanel_) {
        simWavePanel_->OpenTrace(std::string(lastCaptureSimVcd_.ToUTF8().data()));
        if (rightNotebook_) rightNotebook_->SetSelection(0);
    }
    RunComparison();
}

void TraceBridgeWindow::OnGenerateReplay(wxCommandEvent&) {
    if (lastCaptureHwVcd_.IsEmpty() || lastSessionId_.IsEmpty() || projectPath_.IsEmpty()) {
        wxMessageBox(wxT("请先完成一次硬件采集。"), wxT("输入重放"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const wxString sessionRoot = JoinPath(JoinPath(JoinPath(projectPath_, ".sigflow"), "debug"), lastSessionId_);
    const wxString mapPath = JoinPath(sessionRoot, "signal-map.json");
    const wxString graphPath = JoinPath(sessionRoot, "dependency-graph.json");
    if (!wxFileExists(mapPath) || !wxFileExists(graphPath)) {
        std::string topModule;
        std::vector<std::string> sourceFiles;
        std::string configError;
        if (!LoadReplayBuildConfig(ToUtf8(projectPath_), topModule, sourceFiles, configError)) {
            wxMessageBox(ToWxString(configError), wxT("输入重放"), wxOK | wxICON_ERROR, this);
            return;
        }
        sigflow::debug::DebugMappingBuildResult mappingResult;
        if (!sigflow::debug::DebugMappingBuilder::Generate(
                topModule, sourceFiles, contract_, ToUtf8(sessionRoot), mappingResult, configError)) {
            wxMessageBox(ToWxString(configError), wxT("输入重放"), wxOK | wxICON_ERROR, this);
            return;
        }
    }

    JoinWorkerIfAny();
    m_aborted->store(false);
    generateReplayButton_->Enable(false);
    AddLog(TbLogTag::INFO, wxT("开始从硬件输入探针生成 Verilator 重放…"));
    const std::string hwPath = ToUtf8(lastCaptureHwVcd_);
    const std::string projectPath = ToUtf8(projectPath_);
    const std::string rootPath = ToUtf8(sessionRoot);
    const std::string mapFile = ToUtf8(mapPath);
    const sigflow::debug::DebugContract replayContract = contract_;
    worker_ = std::make_unique<std::thread>([this, hwPath, projectPath, rootPath, mapFile,
                                              replayContract]() {
        std::string error;
        bool success = false;
        std::string replayVcd;
        try {
            sigflow::debug::SignalSourceMap sourceMap;
            if (!sourceMap.LoadJson(mapFile, error)) throw std::runtime_error(error);
            const auto inputSignals = sourceMap.InputSignals();
            if (inputSignals.empty()) {
                throw std::runtime_error("当前契约没有被采样的顶层 input 探针；请把 clk、rst_n 和业务输入加入探针后重新构建下载");
            }
            std::string mutablePath = hwPath;
            std::unique_ptr<vcd_t, void(*)(vcd_t*)> capture(
                vcd_read_from_path(mutablePath.data()), vcd_free);
            if (!capture) throw std::runtime_error("无法读取硬件 capture.vcd");
            sigflow::debug::ReplayScenario scenario;
            if (!sigflow::debug::ReplayStimulusExtractor::Extract(
                    capture.get(), inputSignals, replayContract.sampleClock.signal, scenario, error)) {
                throw std::runtime_error(error);
            }
            const auto consistency = sigflow::debug::EvaluateReplayConsistency(
                scenario, inputSignals.size());
            if (!consistency.verified) throw std::runtime_error(
                "激励一致性未验证：" + consistency.message);

            std::string topModule = replayContract.topModule;
            std::vector<std::string> sourceFiles;
            if (!LoadReplayBuildConfig(projectPath, topModule, sourceFiles, error)) {
                throw std::runtime_error(error);
            }
            const std::filesystem::path artifacts = std::filesystem::path(rootPath) / "artifacts";
            const std::filesystem::path tbPath = artifacts / "replay_tb.cpp";
            const std::filesystem::path objectPath = std::filesystem::path(rootPath) / "replay_obj";
            std::filesystem::create_directories(artifacts);
            if (!sigflow::debug::ReplayTestbenchGenerator::Generate(
                    scenario, topModule, tbPath.string(), "artifacts/replay.vcd", error)) {
                throw std::runtime_error(error);
            }
            Json::Value consistencyJson;
            consistencyJson["verified"] = consistency.verified;
            consistencyJson["score"] = consistency.score;
            consistencyJson["requested_inputs"] = Json::UInt64(consistency.requestedInputs);
            consistencyJson["captured_inputs"] = Json::UInt64(consistency.capturedInputs);
            consistencyJson["assignment_count"] = Json::UInt64(consistency.assignmentCount);
            consistencyJson["message"] = consistency.message;
            std::ofstream consistencyFile(artifacts / "replay-consistency.json", std::ios::trunc);
            if (!consistencyFile) throw std::runtime_error(
                "无法保存 replay-consistency.json");
            Json::StreamWriterBuilder consistencyWriter;
            consistencyWriter["indentation"] = "  ";
            consistencyFile << Json::writeString(consistencyWriter, consistencyJson);
             const std::string verilatorPath = FindBundledVerilator();
             std::string buildCommand = QuoteProcessArg(verilatorPath) +
                 " --cc --exe --build --trace --top-module " +
                 QuoteProcessArg(topModule) + " --Mdir " + QuoteProcessArg(objectPath.string());
            for (const auto& source : sourceFiles) buildCommand += " " + QuoteProcessArg(source);
            buildCommand += " " + QuoteProcessArg(tbPath.string());
            const std::string runCommand = QuoteProcessArg(
                (objectPath / sigflow::platform::WithExecutableSuffix(
                    wxString("V") + wxString::FromUTF8(topModule)).ToStdString()).string());
            sigflow::debug::ReplayRunResult runResult;
            if (!sigflow::debug::ReplayRunner::Run(
                    buildCommand + " && " + runCommand, rootPath, runResult, error)) {
                throw std::runtime_error(
                    "Verilator 重放失败；请确认仓库内 tools\\verilator 可用。" + error);
            }
            replayVcd = (artifacts / "replay.vcd").string();
            if (!std::filesystem::exists(replayVcd)) {
                throw std::runtime_error("Verilator 已返回成功，但未生成 artifacts/replay.vcd");
            }
            success = true;
        } catch (const std::exception& ex) {
            error = ex.what();
        }
        CallAfter([this, success, replayVcd, error] {
            generateReplayButton_->Enable(true);
            if (!success) {
                AddLog(TbLogTag::ERR, wxT("输入重放失败：") + ToWxString(error));
                wxMessageBox(ToWxString(error), wxT("输入重放"), wxOK | wxICON_ERROR, this);
                return;
            }
            lastCaptureSimVcd_ = ToWxString(replayVcd);
            if (simWavePanel_) simWavePanel_->OpenTrace(replayVcd);
            if (compareSimButton_) compareSimButton_->Enable(true);
            AddLog(TbLogTag::OK, wxT("输入重放完成：") + ToWxString(replayVcd));
            if (waveHint_) waveHint_->SetLabel(wxT("✅ replay.vcd 已生成，可直接执行双轨比对。"));
            RunComparison();
        });
    });
}

void TraceBridgeWindow::RunComparison() {
    if (lastCaptureHwVcd_.IsEmpty() || lastCaptureSimVcd_.IsEmpty()) {
        AddLog(TbLogTag::ERR, wxT("需要先有 HW 捕获 VCD 和 Sim VCD 才能比对"));
        return;
    }
    AddLog(TbLogTag::INFO, wxT("开始波形比对…"));
    auto hwPath = ToUtf8(lastCaptureHwVcd_);
    auto simPath = ToUtf8(lastCaptureSimVcd_);
    const wxString sidecarRoot = JoinPath(JoinPath(JoinPath(projectPath_, ".sigflow"), "debug"), lastSessionId_);
    const wxString replayReportFile = JoinPath(JoinPath(sidecarRoot, "artifacts"), "replay-consistency.json");
    const bool replayReference = wxFileExists(replayReportFile) &&
        wxFileName(lastCaptureSimVcd_).GetFullName().CmpNoCase(wxT("replay.vcd")) == 0;
    const wxString sourceMapFile = JoinPath(sidecarRoot, "signal-map.json");
    const wxString dependencyGraphFile = JoinPath(sidecarRoot, "dependency-graph.json");
    const bool hasSourceMap = wxFileExists(sourceMapFile);
    const bool hasDependencyGraph = wxFileExists(dependencyGraphFile);
    const sigflow::debug::DebugContract behaviorContract = contract_;
    const std::string sourceMapPath = ToUtf8(sourceMapFile);
    const std::string dependencyGraphPath = ToUtf8(dependencyGraphFile);
    // 在 worker 线程跑比对，避免大 VCD 阻塞 UI
    JoinWorkerIfAny();
    m_aborted->store(false);
    worker_ = std::make_unique<std::thread>([this, hwPath = std::move(hwPath),
                                             simPath = std::move(simPath), sourceMapPath,
                                             dependencyGraphPath, hasSourceMap,
                                             hasDependencyGraph, replayReference,
                                             replayReportPath = ToUtf8(replayReportFile),
                                             behaviorContract]() {
        try {
            std::unique_ptr<vcd_t, void(*)(vcd_t*)> hwVcd(
                vcd_read_from_path(const_cast<char*>(hwPath.c_str())), vcd_free);
            std::unique_ptr<vcd_t, void(*)(vcd_t*)> simVcd(
                vcd_read_from_path(const_cast<char*>(simPath.c_str())), vcd_free);
            if (!hwVcd || !simVcd) {
                PostLog(this, TbLogTag::ERR, wxT("VCD 文件读取失败"));
                PostResult(this, 0, wxT("VCD 文件读取失败"));
                return;
            }
            auto result = sigflow::debug::WaveformComparator::CompareAuto(
                hwVcd.get(), simVcd.get());
            result.referenceKind = replayReference ? "hardware-input-replay" : "simulation";
            if (replayReference) {
                std::ifstream replayReport(replayReportPath);
                Json::Value reportJson;
                Json::CharReaderBuilder reportBuilder;
                std::string reportError;
                std::unique_ptr<Json::CharReader> reportReader(reportBuilder.newCharReader());
                const std::string reportContent((std::istreambuf_iterator<char>(replayReport)), {});
                if (replayReport && reportReader->parse(reportContent.data(),
                                                        reportContent.data() + reportContent.size(),
                                                        &reportJson, &reportError)) {
                    result.stimulusVerified = reportJson.get("verified", false).asBool();
                    result.stimulusScore = reportJson.get("score", 0.0).asDouble();
                }
            }
            sigflow::debug::SignalSourceMap sourceMap;
            sigflow::debug::DependencyGraph dependencyGraph;
            std::string mappingError;
            if (hasSourceMap) sourceMap.LoadJson(sourceMapPath, mappingError);
            if (hasDependencyGraph) dependencyGraph.LoadJson(dependencyGraphPath, mappingError);
            if (sourceMap.Size() > 0 && dependencyGraph.NodeCount() > 0) {
                sigflow::debug::WaveformComparator::Enrich(result, sourceMap, dependencyGraph);
            }
            auto behaviorSummary = sigflow::debug::DebugBehaviorSummaryBuilder::Extract(
                hwVcd.get(), behaviorContract, result);
            // 将结果序列化为文本通过自定义事件回传
            auto* e = new wxCommandEvent(wxEVT_TB_LOG);
            e->SetString(wxT("OK|比对完成"));
            this->QueueEvent(e);
            // 在主线程显示结果
            wxString summary = ToWxString(result.summary);
            CallAfter([this, result = std::move(result), behaviorSummary = std::move(behaviorSummary), summary] {
                lastComparison_ = std::move(result);
                lastBehaviorSummary_ = std::move(behaviorSummary);
                ShowComparisonResult(lastComparison_);
            });
        } catch (const std::exception& ex) {
            wxString err = wxT("比对异常：") + wxString::FromUTF8(ex.what());
            PostLog(this, TbLogTag::ERR, err);
            PostResult(this, 0, err);
        }
    });
}

void TraceBridgeWindow::ShowComparisonResult(const sigflow::debug::ComparisonResult& r) {
    if (!diffReportText_) return;
    wxString text = ToWxString(r.summary);
    if (!lastBehaviorSummary_.events.empty()) {
        text += wxT("\n\n--- 硬件行为摘要（确定性规则）---\n");
        for (const auto& event : lastBehaviorSummary_.events) {
            text += wxString::Format(wxT("t=%u  %s  %s\n"), event.time,
                                     ToWxString(event.kind), ToWxString(event.message));
        }
    }
    text += wxT("\n参考基准：") + ToWxString(r.referenceKind);
    if (r.referenceKind == "hardware-input-replay") {
        text += wxString::Format(wxT("（激励一致性 %.0f%%）"), r.stimulusScore * 100.0);
        if (!r.stimulusVerified) text += wxT(" [未验证，不输出确定性首因]");
    }
    text += wxT("\n\n--- 差异明细 ---\n");
    if (r.firstDiffs.empty()) {
        text += wxT("✅ 所有共有信号在比对范围内一致。\n");
    } else {
        for (const auto& d : r.firstDiffs) {
            wxString line;
            line.Printf(wxT("信号: %-20s HW t=%-6u Sim t=%-6u 期望=%-6s 实测=%-6s 置信度=%.0f%%\n"),
                        ToWxString(d.signalName), d.hwTime, d.simTime,
                        ToWxString(d.expectedValue), ToWxString(d.actualValue),
                        d.confidence * 100);
            text += line;
            if (!d.location.sourcePath.empty() && d.location.sourceLine > 0) {
                text += wxString::Format(wxT("  RTL: %s:%d\n"),
                                         ToWxString(d.location.sourcePath), d.location.sourceLine);
            }
            if (!d.upstreamCandidates.empty()) {
                text += wxT("  上游候选: ");
                for (std::size_t i = 0; i < d.upstreamCandidates.size(); ++i) {
                    if (i != 0) text += wxT(" -> ");
                    text += ToWxString(d.upstreamCandidates[i].location.signalName);
                }
                text += wxT("\n");
            }
        }
    }
    if (r.totalDiffs > r.firstDiffs.size()) {
        text += wxString::Format(wxT("\n（共 %zu 处差异，仅显示每信号首个）"),
                                r.totalDiffs);
    }
    diffReportText_->SetValue(text);
    if (rightNotebook_) rightNotebook_->SetSelection(1); // 切到比对结果 Tab
    AddLog(TbLogTag::OK, wxString::Format(wxT("比对完成：%zu 信号 %zu 差异"),
                                          r.totalSignalsCompared, r.totalDiffs));

    // ── W4 集成：波形事件注入 + Compare Hub 联动 ──
    if (hwWavePanel_ && simWavePanel_) {
        // 1. 比对事件注入（锚点绿/差异红/摘要橙）
        hwWavePanel_->InjectCompareEvents(r, true);
        simWavePanel_->InjectCompareEvents(r, false);
        hwWavePanel_->InjectBehaviorEvents(lastBehaviorSummary_.events, true);
        simWavePanel_->InjectBehaviorEvents(lastBehaviorSummary_.events, false,
                                             r.alignment.timeOffset);

        // 2. Compare Hub 注册并开启联动
        hwWavePanel_->RegisterCompareHub();
        simWavePanel_->RegisterCompareHub();

        // 3. 把播放头跳到首个差异位置
        if (!r.firstDiffs.empty()) {
            const sigflow::debug::WaveformDiff& first = r.firstDiffs.front();
            hwWavePanel_->View()->JumpToTime(static_cast<sigflow::trace::TimeValue>(first.hwTime));
            hwWavePanel_->View()->SetPlayhead(static_cast<sigflow::trace::TimeValue>(first.hwTime));
            simWavePanel_->View()->JumpToTime(static_cast<sigflow::trace::TimeValue>(first.simTime));
            simWavePanel_->View()->SetPlayhead(static_cast<sigflow::trace::TimeValue>(first.simTime));
        } else if (r.aligned) {
            // 无差异则跳到对齐锚点
            hwWavePanel_->View()->JumpToTime(static_cast<sigflow::trace::TimeValue>(r.alignment.hwAnchorTime));
            hwWavePanel_->View()->SetPlayhead(static_cast<sigflow::trace::TimeValue>(r.alignment.hwAnchorTime));
            simWavePanel_->View()->JumpToTime(static_cast<sigflow::trace::TimeValue>(r.alignment.simAnchorTime));
            simWavePanel_->View()->SetPlayhead(static_cast<sigflow::trace::TimeValue>(r.alignment.simAnchorTime));
        }
    }

    // 4. 保存 compare.json 到会话目录
    if (!lastSessionId_.IsEmpty() && !projectPath_.IsEmpty()) {
        wxString sessionDir = JoinPath(JoinPath(JoinPath(projectPath_, ".sigflow"), "debug"), lastSessionId_);
        SaveCompareJson(sessionDir, lastSessionId_, r);
        const auto paths = sigflow::debug::DebugSessionService::GetPaths(
            ToUtf8(projectPath_), ToUtf8(lastSessionId_));
        std::string summaryError;
        if (!sigflow::debug::DebugBehaviorSummaryBuilder::Save(
                lastBehaviorSummary_, JoinPath(paths.reports, "behavior-summary.json"),
                JoinPath(paths.reports, "behavior-summary.md"), summaryError)) {
            AddLog(TbLogTag::WR, ToWxString(summaryError));
        }
    }
}

void TraceBridgeWindow::AddLog(TbLogTag tag, const wxString& message) {
    if (!logList_) return;
    const long idx = logList_->InsertItem(logList_->GetItemCount(), NowStamp());
    logList_->SetItem(idx, 1, TbLogTagText(tag));
    logList_->SetItem(idx, 2, message);
    logList_->SetItemTextColour(idx, TbLogTagColour(tag));
    // trim
    while (logList_->GetItemCount() > 200) logList_->DeleteItem(0);
    logList_->EnsureVisible(idx);
}

void TraceBridgeWindow::UpdateStepperFromStage(int stageOrder, const wxString& message) {
    // StageOrder 0(Idle) -> Step 1 contract loaded previously...
    // Map stage -> our 7 step bar; Step 2 Build skipped in runtime (user has done that) so
    // sync -> step 3 fingerprint; config -> step 4; arm -> step 5; poll/read -> step 6; done -> step 7.
    int barStep = 1;
    switch (stageOrder) {
        case 0: barStep = 1; break; // Idle
        case 1: barStep = 3; break; // Sync calibrate -> treat as step3 get-info start
        case 2: barStep = 3; break; // GetInfo fingerprint check -> step 3
        case 3: barStep = 4; break; // MapTrigger -> Config step (step 4 start)
        case 4: barStep = 4; break; // Configure -> step 4
        case 5: barStep = 5; break; // Arm -> step 5
        case 6: barStep = 6; break; // PollStatus -> step 6 acquisition
        case 7: barStep = 6; break; // ReadSamples -> step 6 acquisition
        case 8: barStep = 7; break; // SaveRaw -> step7 waveform write
        case 9: barStep = 7; break; // DecodeVcd -> step7
        case 10:barStep = 7; break; // Done
        default: barStep = std::max(1, std::min(7, stageOrder)); break;
    }
    stepper_->SetActive(barStep, message);
    stepperStatus_->SetLabel(message.IsEmpty() ? stepperStatus_->GetLabel() : message);
}

void TraceBridgeWindow::OnTBStep(wxCommandEvent& e) {
    UpdateStepperFromStage(e.GetInt(), e.GetString());
}
void TraceBridgeWindow::OnTBProgress(wxCommandEvent& e) {
    const int pct = std::clamp(e.GetInt(), 0, 100);
    progressGauge_->SetValue(pct);
    if (!e.GetString().IsEmpty()) stepperStatus_->SetLabel(e.GetString());
}
void TraceBridgeWindow::OnTBLog(wxCommandEvent& e) {
    const wxString s = e.GetString();
    const auto bar = s.find(wxT('|'));
    if (bar == wxString::npos) { AddLog(TbLogTag::INFO, s); return; }
    const wxString tagS = s.Left(bar);
    const wxString msg  = s.Mid(bar + 1);
    auto mapTag = [](const wxString& t) -> TbLogTag {
        if (t == wxT("CFG"))    return TbLogTag::CFG;
        if (t == wxT("ARM"))    return TbLogTag::ARM;
        if (t == wxT("POL"))    return TbLogTag::POL;
        if (t == wxT("RD"))     return TbLogTag::RD;
        if (t == wxT("OK"))     return TbLogTag::OK;
        if (t == wxT("WR"))     return TbLogTag::WR;
        if (t == wxT("ERR"))    return TbLogTag::ERR;
        if (t == wxT("CANCEL")) return TbLogTag::CANCEL;
        return TbLogTag::INFO;
    };
    AddLog(mapTag(tagS), msg);
}
void TraceBridgeWindow::OnTBResult(wxCommandEvent& e) {
    const int code = e.GetInt();
    if (code == 1) {
        SetCaptureResult(true, wxT("✅ 采集完成"), e.GetString());
    } else if (code == -1) {
        SetCaptureResult(false, wxT("❌ 已取消：") + e.GetString(), wxEmptyString);
    } else {
        SetCaptureResult(false, wxT("❌ 失败：") + e.GetString(), wxEmptyString);
    }
    JoinWorkerIfAny();
}

bool TraceBridgeWindow::BuildCaptureRequest(TraceBridgeCaptureRequest& request, wxString& error) const {
    if (projectPath_.IsEmpty()) {
        error = wxT("请先打开一个 SigFlow 项目（File → Open Project）。");
        return false;
    }
    if (contract_.probes.empty() && contractFilePath_.empty()) {
        error = wxT("请先加载 Debug Contract JSON（Tab 1 → 浏览）。");
        return false;
    }
    if (!runtimePanel_ || !runtimePanel_->serialChoice_ || runtimePanel_->serialChoice_->GetSelection() == wxNOT_FOUND) {
        error = wxT("请选择一个串口（点【刷新】或检查 FPGA 是否连接）。");
        return false;
    }
    const int baud = runtimePanel_->baudSpin_->GetValue();
    if (!sigflow::debug::SerialTransport::IsSupportedBaud(static_cast<uint32_t>(baud))) {
        error = wxT("波特率不在允许范围内，允许示例：921600 / 2000000 / 3000000。");
        return false;
    }
    request.projectPath = ToUtf8(projectPath_);
    request.contractPath = ToUtf8(contractFilePath_);
    request.serialPort = ToUtf8(runtimePanel_->GetSelectedSerialPureName());
    // 把实际使用的串口写进日志。旧实现不记录，出问题时只能靠猜
    // （曾出现自动选到 JTAG 通道 → 同步校准 response timeout）。
    {
        auto* self = const_cast<TraceBridgeWindow*>(this);
        if (request.serialPort.empty()) {
            PostLog(self, TbLogTag::ERR,
                    wxT("未选择串口：请在运行时面板选择调试器串口"));
        } else {
            PostLog(self, TbLogTag::CFG,
                    wxT("使用串口：") + ToWxString(request.serialPort));
        }
    }
    request.contract = contract_;
    if (probeEditor_) {
        std::vector<sigflow::debug::DebugProbe> editedProbes;
        if (!ParseProbeEditor(probeEditor_->GetValue(), editedProbes, error)) return false;
        request.contract.probes = std::move(editedProbes);
    }
    request.options = sigflow::debug::DebugAcquisitionOptions{};
    wxString rErr;
    runtimePanel_->ApplyToRequest(request.contract, request.options, rErr);
    if (!rErr.IsEmpty()) { error = rErr; return false; }
    runtimePanel_->ApplyCapturePlan(request);
    std::string cErr;
    request.contract.ApplyDefaults();
    if (!request.contract.AssignProbeBitOffsets(cErr) || !request.contract.Validate(cErr)) {
        error = wxT("参数/契约校验失败：") + ToWxString(cErr);
        return false;
    }
    wxString pinGuidance;
    if (!ValidateDebugTransportPins(request.contract, projectPath_, error, pinGuidance)) return false;
    if (!pinGuidance.IsEmpty()) {
        const_cast<TraceBridgeWindow*>(this)->AddLog(TbLogTag::WR, pinGuidance);
    }
    // 统一走宿主预检服务，确保 GUI 的即时校验与后续向导/CI 使用同一套
    // 稳定诊断码。这里仅执行离线检查；设备打开和租约仍由采集 worker 管理。
    sigflow::debug::TransportOpenOptions transportOptions{
        request.serialPort, request.contract.transport.baud, true, true};
    sigflow::debug::PreflightInput preflightInput;
    preflightInput.contract = &request.contract;
    preflightInput.transport = &transportOptions;
    sigflow::debug::PreflightReport preflightReport;
    std::string preflightError;
    auto preflight = sigflow::debug::MakeTraceBridgePreflightRegistry();
    if (!preflight.Run(preflightInput, preflightReport, preflightError)) {
        for (const auto& issue : preflightReport.issues) {
            const_cast<TraceBridgeWindow*>(this)->AddLog(
                issue.severity == sigflow::debug::DiagnosticSeverity::Blocking
                    ? TbLogTag::ERR : TbLogTag::WR,
                ToWxString(issue.code + ": " + issue.observed));
        }
        error = wxT("TraceBridge 预检失败：") + ToWxString(preflightError);
        return false;
    }
    for (const auto& issue : preflightReport.issues) {
        if (issue.severity != sigflow::debug::DiagnosticSeverity::Info) {
            const_cast<TraceBridgeWindow*>(this)->AddLog(
                TbLogTag::WR, ToWxString(issue.code + ": " + issue.observed));
        }
    }
    return true;
}

// ---------- worker thread ----------
namespace {
void PostStep(wxEvtHandler* h, int order, const wxString& msg) {
    auto* e = new wxCommandEvent(wxEVT_TB_STEP);
    e->SetInt(order);
    e->SetString(msg);
    h->QueueEvent(e);
}
void PostProgress(wxEvtHandler* h, int pct, const wxString& msg) {
    auto* e = new wxCommandEvent(wxEVT_TB_PROGRESS);
    e->SetInt(pct);
    e->SetString(msg);
    h->QueueEvent(e);
}
// PostLog / PostResult 已在前面的匿名命名空间中定义。
int StageOverallPct(sigflow::debug::DebugAcqStage stage, int pctInStage) {
    // 阶段权重：Sync 3, Info 5, Map 2, Config 8, Arm 3, Poll 33, Read 30, SaveRaw 4, Decode 8, Done 5
    constexpr int weights[] = { 0, 3, 5, 2, 8, 3, 33, 30, 4, 8, 5 };
    constexpr int nStages = sizeof(weights)/sizeof(weights[0]);
    int acc = 0;
    const int idx = static_cast<int>(stage);
    for (int i = 0; i < idx && i < nStages; ++i) acc += weights[i];
    if (idx < nStages) {
        const int add = (weights[idx] * std::max(0, std::min(100, pctInStage))) / 100;
        acc += add;
    }
    return std::max(0, std::min(100, acc));
}
} // namespace

void TraceBridgeWindow::OnStartCapture(wxCommandEvent&) {
    if (pendingProbeBuild_) {
        AddLog(TbLogTag::WR, wxT("新增探针尚待构建和下载，请先运行“构建、下载并采集”或加载匹配的契约。"));
        return;
    }
    TraceBridgeCaptureRequest request;
    wxString error;
    if (!BuildCaptureRequest(request, error)) {
        AddLog(TbLogTag::ERR, error);
        wxMessageBox(error, wxT("TraceBridge 采集参数错误"), wxOK | wxICON_ERROR, this);
        return;
    }
    contract_ = request.contract;
    summaryCard_->UpdateFrom(contract_, contractFilePath_);
    JoinWorkerIfAny();
    m_aborted->store(true);
    m_aborted = std::make_shared<std::atomic<bool>>(false);
    progressGauge_->SetValue(0);
    startCaptureButton_->Enable(false);
    cancelCaptureButton_->Enable(true);
    openHwVcdButton_->Enable(false);
    compareSimButton_->Enable(false);
    generateReplayButton_->Enable(false);
    stepper_->Reset();
    stepper_->SetActive(1, wxT("已进入采集流程"));
    stepperStatus_->SetLabel(wxT("开始采集，串口：") + ToWxString(request.serialPort));
    AddLog(TbLogTag::CFG, wxT("开始采集，串口：") + ToWxString(request.serialPort));

    // 注入 AbortToken + 进度回调。回调在 worker 线程调用，通过 QueueEvent marshal。
    request.options.aborted = m_aborted.get();
    request.abortOwner = m_aborted;
    request.options.onProgress = [weakWindow = wxWeakRef<TraceBridgeWindow>(this)](sigflow::debug::DebugAcqStage stage, int pct,
                                         const std::string& msg) {
        // map stage → log tag
        TbLogTag tag = TbLogTag::INFO;
        switch (stage) {
            case sigflow::debug::DebugAcqStage::SyncCalibrate: tag = TbLogTag::CFG; break;
            case sigflow::debug::DebugAcqStage::GetInfo:       tag = TbLogTag::CFG; break;
            case sigflow::debug::DebugAcqStage::MapTrigger:    tag = TbLogTag::CFG; break;
            case sigflow::debug::DebugAcqStage::Configure:     tag = TbLogTag::CFG; break;
            case sigflow::debug::DebugAcqStage::Arm:           tag = TbLogTag::ARM; break;
            case sigflow::debug::DebugAcqStage::PollStatus:    tag = TbLogTag::POL; break;
            case sigflow::debug::DebugAcqStage::ReadSamples:   tag = TbLogTag::RD;  break;
            case sigflow::debug::DebugAcqStage::SaveRaw:       tag = TbLogTag::INFO; break;
            case sigflow::debug::DebugAcqStage::DecodeVcd:     tag = TbLogTag::INFO; break;
            case sigflow::debug::DebugAcqStage::Done:          tag = TbLogTag::OK;  break;
            default: break;
        }
        if (wxTheApp) wxTheApp->CallAfter([weakWindow, stage, pct, msg, tag] {
            if (!weakWindow) return;
            PostStep(weakWindow.get(), sigflow::debug::DebugAcquisition::StageOrder(stage), ToWxString(msg));
            PostProgress(weakWindow.get(), StageOverallPct(stage, pct), ToWxString(msg));
            if (!msg.empty()) PostLog(weakWindow.get(), tag, ToWxString(msg));
        });
    };

    // Dispatch from the UI thread: MainFrame snapshots project/UI state before
    // starting its worker. The local fallback worker remains joinable.
    if (captureStartHandler_) {
        AddLog(TbLogTag::INFO, wxT("已委托外部捕获服务，等待采集结果…"));
        captureStartHandler_(request);
        return;
    }

    worker_ = std::make_unique<std::thread>([this, request = std::move(request)]() {
        try {
            // MainFrame 创建正式 DebugSession；此目录仅作为无主框架时的回退。
            std::string outDir = request.projectPath.empty()
                ? JoinPath(std::string("."), "sigflow_capture_out")
                : JoinPath(JoinPath(JoinPath(request.projectPath, ".sigflow"), "debug"), "capture-fallback");
            std::string error;
            bool ok = false;
            wxString vcdPath;
            // Standalone fallback shares the transport factory and device lease.
            if (request.captureTargetCount != 1) {
                PostResult(this, 0, wxT("重复采集需要连接主窗口的会话服务"));
                return;
            }
            sigflow::debug::DeviceControlLease lease;
            if (!sigflow::debug::DeviceControlLease::Acquire(
                    "tracebridge-hardware", "standalone-capture", lease, error)) {
                PostResult(this, 0, ToWxString(error));
                return;
            }
            const std::uint32_t debugBaud =
                static_cast<std::uint32_t>(request.contract.transport.baud);
            sigflow::debug::TransportOpenResult opened;
            if (!sigflow::debug::OpenDebugTransport({request.serialPort, debugBaud, true, true}, opened, error)) {
                PostLog(this, TbLogTag::ERR, ToWxString(error));
                PostResult(this, 0, ToWxString(error));
                return;
            }
            auto transport = std::move(opened.transport);
            for (const auto& diagnostic : opened.diagnostics)
                PostLog(this, TbLogTag::CFG, ToWxString(diagnostic));
            sigflow::debug::DebugAcquisition acq(*transport, request.contract);
            ok = acq.Acquire(outDir, error, request.options);
            vcdPath = ToWxString(acq.Result().vcdPath);
            if (m_aborted->load()) {
                wxString abortMsg = error.empty() ? wxString(wxT("Aborted")) : ToWxString(error);
                PostResult(this, -1, abortMsg);
                return;
            }
            if (!ok) {
                PostLog(this, TbLogTag::ERR, wxT("采集失败：") + ToWxString(error));
                wxString failMsg = error.empty() ? wxString(wxT("采集失败")) : ToWxString(error);
                PostResult(this, 0, failMsg);
                return;
            }
            PostResult(this, 1, vcdPath.IsEmpty() ? wxString(wxT("采集完成（VCD 路径未返回）")) : vcdPath);
        } catch (const std::exception& ex) {
            PostLog(this, TbLogTag::ERR, wxT("Worker 异常：") + wxString::FromUTF8(ex.what()));
            PostResult(this, 0, wxString::FromUTF8(ex.what()));
        } catch (...) {
            PostLog(this, TbLogTag::ERR, wxT("Worker 未知异常"));
            PostResult(this, 0, wxT("Worker unknown exception"));
        }
    });
}

// ================== 会话管理 + 导出 ==================
void TraceBridgeWindow::RefreshSessionList() {
    if (!sessionChoice_) return;
    sessionChoice_->Clear();
    exportButton_->Enable(false);
    if (projectPath_.IsEmpty()) return;
    sigflow::debug::DebugSessionService service;
    std::vector<sigflow::debug::DebugSessionInfo> sessions;
    std::string error;
    if (!service.List(ToUtf8(projectPath_), sessions, error)) {
        AddLog(TbLogTag::ERR, wxT("枚举会话失败：") + ToWxString(error));
        return;
    }
    for (const auto& s : sessions) {
        wxString label = ToWxString(s.id) + wxT(" [") + ToWxString(ToString(s.state)) + wxT("]");
        sessionChoice_->Append(label, new wxStringClientData(ToWxString(s.id)));
    }
    if (sessionChoice_->GetCount() > 0) {
        sessionChoice_->SetSelection(0);
        exportButton_->Enable(true);
    }
    AddLog(TbLogTag::INFO, wxString::Format(wxT("历史会话：%u 个"),
                                           static_cast<unsigned>(sessionChoice_->GetCount())));
}

void TraceBridgeWindow::OnRefreshSessions(wxCommandEvent&) { RefreshSessionList(); }

void TraceBridgeWindow::OnSessionSelected(wxCommandEvent&) {
    auto* cd = dynamic_cast<wxStringClientData*>(sessionChoice_->GetClientObject(sessionChoice_->GetSelection()));
    if (!cd) return;
    exportButton_->Enable(true);
    const wxString sessionId = cd->GetData();
    LoadSessionVcd(sessionId);
}

void TraceBridgeWindow::LoadSessionVcd(const wxString& sessionId) {
    if (projectPath_.IsEmpty() || sessionId.IsEmpty()) return;
    auto paths = sigflow::debug::DebugSessionService::GetPaths(ToUtf8(projectPath_),
                                                               ToUtf8(sessionId));
    const wxString captureDir = ToWxString(paths.artifacts);
    const wxString vcdPath = wxFileName(captureDir, wxT("capture.vcd")).GetFullPath();
    if (wxFileName::FileExists(vcdPath)) {
        lastCaptureHwVcd_ = vcdPath;
        if (hwWavePanel_) hwWavePanel_->OpenTrace(std::string(vcdPath.ToUTF8().data()));
        openHwVcdButton_->Enable(true);
        compareSimButton_->Enable(true);
        AddLog(TbLogTag::INFO, wxT("已加载会话 VCD：") + vcdPath);
    } else {
        AddLog(TbLogTag::INFO, wxT("该会话无 capture.vcd"));
    }
}

void TraceBridgeWindow::OnExportSession(wxCommandEvent&) {
    if (!sessionChoice_ || sessionChoice_->GetSelection() == wxNOT_FOUND) {
        wxMessageBox(wxT("请先选择一个会话"), wxT("导出"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    auto* cd = dynamic_cast<wxStringClientData*>(sessionChoice_->GetClientObject(sessionChoice_->GetSelection()));
    if (!cd) return;
    const wxString sessionId = cd->GetData();

    wxFileDialog dlg(this, wxT("导出会话包"), wxEmptyString,
                    sessionId + wxT(".zip"), wxT("ZIP files (*.zip)|*.zip|All files (*.*)|*.*"),
                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxString outPath, error;
    if (ExportSessionToZip(sessionId, outPath = dlg.GetPath(), error)) {
        AddLog(TbLogTag::OK, wxT("已导出：") + outPath);
        wxMessageBox(wxT("已导出到：") + outPath, wxT("导出成功"), wxOK | wxICON_INFORMATION, this);
    } else {
        AddLog(TbLogTag::ERR, wxT("导出失败：") + error);
        wxMessageBox(error, wxT("导出失败"), wxOK | wxICON_ERROR, this);
    }
}

bool TraceBridgeWindow::ExportSessionToZip(const wxString& sessionId, wxString& outPath, wxString& error) {
    if (projectPath_.IsEmpty()) { error = wxT("项目路径为空"); return false; }
    auto paths = sigflow::debug::DebugSessionService::GetPaths(ToUtf8(projectPath_),
                                                               ToUtf8(sessionId));
    const wxString sessionRoot = ToWxString(paths.root);
    if (!wxDirExists(sessionRoot)) { error = wxT("会话目录不存在：") + sessionRoot; return false; }

    std::vector<std::pair<std::string, wxString>> files;
    bool invalidFilePath = false;
    std::function<void(const wxString&)> addDir = [&](const wxString& dirPath) {
        wxDir dir(dirPath);
        if (!dir.IsOpened()) return;
        wxString filename;
        bool cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES);
        while (cont) {
            const wxString fullPath = wxFileName(dirPath, filename).GetFullPath();
            std::string relPath = ToUtf8(fullPath.Mid(sessionRoot.Len()));
            std::replace(relPath.begin(), relPath.end(), '\\', '/');
            while (!relPath.empty() && relPath.front() == '/') relPath.erase(relPath.begin());
            if (!IsSafeArchiveEntryName(relPath) || relPath == "archive-manifest.json") {
                invalidFilePath = true;
                return;
            }
            files.emplace_back(std::move(relPath), fullPath);
            cont = dir.GetNext(&filename);
        }
        cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_DIRS);
        while (cont) {
            const wxString subDir = wxFileName(dirPath, filename).GetFullPath();
            addDir(subDir);
            cont = dir.GetNext(&filename);
        }
    };

    addDir(sessionRoot);
    if (invalidFilePath) { error = wxT("会话目录包含不安全文件路径"); return false; }
    if (files.empty()) { error = wxT("会话目录为空"); return false; }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    Json::Value archiveManifest;
    archiveManifest["schema_version"] = "1.0";
    archiveManifest["kind"] = "sigflow-tracebridge-session";
    archiveManifest["session_id"] = ToUtf8(sessionId);
    Json::Value manifestFiles(Json::arrayValue);
    for (const auto& file : files) {
        const wxULongLong size = wxFileName(file.second).GetSize();
        const std::string hash = sigflow::debug::Sha256File(ToUtf8(file.second));
        if (size == wxInvalidSize || hash.empty()) {
            error = wxT("无法读取会话文件：") + file.second;
            return false;
        }
        Json::Value item;
        item["path"] = file.first;
        item["size"] = Json::UInt64(size.GetValue());
        item["sha256"] = hash;
        manifestFiles.append(item);
    }
    archiveManifest["files"] = manifestFiles;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string archiveManifestText = Json::writeString(writer, archiveManifest);

    const wxString temporaryPath = outPath + wxT(".tmp");
    wxRemoveFile(temporaryPath);
    wxFFileOutputStream out(temporaryPath);
    if (!out.IsOk()) { error = wxT("无法创建输出文件：") + temporaryPath; return false; }
    wxZipOutputStream zip(out);
    for (const auto& file : files) {
        wxFFileInputStream input(file.second);
        if (!input.IsOk() || !zip.PutNextEntry(wxString::FromUTF8(file.first.c_str()))) {
            error = wxT("无法写入会话文件：") + file.second;
            zip.Close();
            out.Close();
            wxRemoveFile(temporaryPath);
            return false;
        }
        input.Read(zip);
        if (!input.IsOk() || !zip.IsOk()) {
            error = wxT("读取会话文件失败：") + file.second;
            zip.Close();
            out.Close();
            wxRemoveFile(temporaryPath);
            return false;
        }
    }
    if (!zip.PutNextEntry(wxT("archive-manifest.json"))) {
        error = wxT("无法写入归档完整性清单");
        zip.Close();
        out.Close();
        wxRemoveFile(temporaryPath);
        return false;
    }
    zip.Write(archiveManifestText.data(), archiveManifestText.size());
    if (!zip.Close() || !out.IsOk()) {
        error = wxT("关闭会话归档失败");
        out.Close();
        wxRemoveFile(temporaryPath);
        return false;
    }
    out.Close();
    if (!wxRenameFile(temporaryPath, outPath, true)) {
        error = wxT("无法替换输出归档：") + outPath;
        wxRemoveFile(temporaryPath);
        return false;
    }
    return true;
}

void TraceBridgeWindow::OnImportSession(wxCommandEvent&) {
    if (projectPath_.IsEmpty()) {
        wxMessageBox(wxT("请先打开项目"), wxT("导入"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxFileDialog dialog(this, wxT("导入会话包"), wxEmptyString, wxEmptyString,
                        wxT("ZIP files (*.zip)|*.zip|All files (*.*)|*.*"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    wxString sessionId;
    wxString error;
    if (!ImportSessionFromZip(dialog.GetPath(), sessionId, error)) {
        AddLog(TbLogTag::ERR, wxT("导入失败：") + error);
        wxMessageBox(error, wxT("导入失败"), wxOK | wxICON_ERROR, this);
        return;
    }
    RefreshSessionList();
    for (unsigned int index = 0; index < sessionChoice_->GetCount(); ++index) {
        auto* data = dynamic_cast<wxStringClientData*>(sessionChoice_->GetClientObject(index));
        if (data && data->GetData() == sessionId) {
            sessionChoice_->SetSelection(static_cast<int>(index));
            LoadSessionVcd(sessionId);
            break;
        }
    }
    AddLog(TbLogTag::OK, wxT("已导入并校验会话：") + sessionId);
    wxMessageBox(wxT("会话已导入：") + sessionId, wxT("导入成功"),
                 wxOK | wxICON_INFORMATION, this);
}

bool TraceBridgeWindow::ImportSessionFromZip(const wxString& archivePath,
                                             wxString& sessionId, wxString& error) {
    if (projectPath_.IsEmpty()) { error = wxT("项目路径为空"); return false; }
    wxFFileInputStream input(archivePath);
    if (!input.IsOk()) { error = wxT("无法打开归档：") + archivePath; return false; }
    wxZipInputStream zip(input);
    const std::filesystem::path debugRoot(ToUtf8(
        ToWxString(sigflow::debug::DebugSessionService::DebugRoot(ToUtf8(projectPath_)))));
    std::error_code fsError;
    std::filesystem::create_directories(debugRoot, fsError);
    if (fsError) { error = wxT("无法创建调试会话目录"); return false; }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path staging = debugRoot / (".import-staging-" + std::to_string(stamp));
    std::filesystem::create_directories(staging, fsError);
    if (fsError) { error = wxT("无法创建导入暂存目录"); return false; }
    bool keepStaging = false;
    auto cleanup = [&]() {
        if (!keepStaging) std::filesystem::remove_all(staging, fsError);
    };

    std::set<std::string> archiveFiles;
    constexpr std::uint64_t kMaximumEntryBytes = 512ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kMaximumArchiveBytes = 1024ULL * 1024ULL * 1024ULL;
    constexpr std::size_t kMaximumArchiveFiles = 4096;
    std::uint64_t extractedBytes = 0;
    std::unique_ptr<wxZipEntry> entry;
    while ((entry.reset(zip.GetNextEntry()), entry != nullptr)) {
        const std::string name = NormalizeArchiveEntryName(entry->GetInternalName());
        std::string fileName = name;
        while (!fileName.empty() && fileName.back() == '/') fileName.pop_back();
        if (!IsSafeArchiveEntryName(fileName)) {
            error = wxT("归档包含不安全路径：") + entry->GetInternalName();
            cleanup();
            return false;
        }
        if (entry->IsDir()) continue;
        if (!archiveFiles.insert(fileName).second) {
            error = wxT("归档包含重复文件：") + ToWxString(fileName);
            cleanup();
            return false;
        }
        if (archiveFiles.size() > kMaximumArchiveFiles) {
            error = wxT("归档文件数量过多");
            cleanup();
            return false;
        }
        if (entry->GetSize() != wxInvalidOffset &&
            static_cast<std::uint64_t>(entry->GetSize()) > kMaximumEntryBytes) {
            error = wxT("归档文件过大：") + ToWxString(fileName);
            cleanup();
            return false;
        }
        wxString destination = JoinPath(ToWxString(staging.string()),
                               wxString::FromUTF8(fileName.c_str()));
        // 不要在这里把 '/' 换成 '\\'：JoinPath 已经给出平台正确的分隔符，
        // 强行替换会把 Linux 路径整体揉成一个文件名字符串，
        // 导致 Mkdir/写入落到错误位置、后续 SHA/大小校验失败 → ZIP 会话导入必失败。
        if (!wxFileName::Mkdir(wxFileName(destination).GetPath(), wxS_DIR_DEFAULT,
                               wxPATH_MKDIR_FULL)) {
            error = wxT("无法创建归档文件目录：") + destination;
            cleanup();
            return false;
        }
        wxFFileOutputStream output(destination);
        if (!output.IsOk()) {
            error = wxT("无法写入归档文件：") + destination;
            cleanup();
            return false;
        }
        std::vector<char> buffer(64 * 1024);
        std::uint64_t total = 0;
        while (!zip.Eof()) {
            zip.Read(buffer.data(), buffer.size());
            const std::size_t count = zip.LastRead();
            if (count == 0) break;
            total += count;
            extractedBytes += count;
            output.Write(buffer.data(), count);
            if (total > kMaximumEntryBytes || extractedBytes > kMaximumArchiveBytes ||
                output.LastWrite() != count) {
                error = wxT("归档文件读取或写入失败：") + destination;
                output.Close();
                cleanup();
                return false;
            }
        }
        output.Close();
        if (!zip.IsOk() ||
            (entry->GetSize() != wxInvalidOffset &&
             total != static_cast<std::uint64_t>(entry->GetSize()))) {
            error = wxT("归档文件校验失败：") + destination;
            cleanup();
            return false;
        }
    }
    if (!zip.Eof() || archiveFiles.find("archive-manifest.json") == archiveFiles.end()) {
        error = wxT("归档缺少完整性清单或已损坏");
        cleanup();
        return false;
    }

    Json::Value archiveManifest;
    std::string jsonError;
    if (!ReadJsonFile(staging / "archive-manifest.json", archiveManifest, jsonError) ||
        archiveManifest.get("schema_version", "").asString() != "1.0" ||
        archiveManifest.get("kind", "").asString() != "sigflow-tracebridge-session" ||
        !archiveManifest["session_id"].isString() || !archiveManifest["files"].isArray()) {
        error = ToWxString(jsonError.empty() ? "invalid archive manifest" : jsonError);
        cleanup();
        return false;
    }
    const std::string importedId = archiveManifest["session_id"].asString();
    if (importedId.empty() || importedId.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
        error = wxT("归档会话 ID 不合法");
        cleanup();
        return false;
    }
    std::map<std::string, std::pair<std::uint64_t, std::string>> expectedFiles;
    for (const Json::Value& item : archiveManifest["files"]) {
        if (!item.isObject() || !item["path"].isString() || !item["size"].isUInt64() ||
            !item["sha256"].isString()) {
            error = wxT("归档完整性清单格式错误");
            cleanup();
            return false;
        }
        const std::string path = NormalizeArchiveEntryName(ToWxString(item["path"].asString()));
        if (!IsSafeArchiveEntryName(path) || path == "archive-manifest.json" ||
            !expectedFiles.emplace(path, std::make_pair(item["size"].asUInt64(),
                                                        item["sha256"].asString())).second) {
            error = wxT("归档完整性清单包含重复或不安全路径");
            cleanup();
            return false;
        }
    }
    if (expectedFiles.size() != archiveFiles.size() - 1) {
        error = wxT("归档文件清单数量不一致");
        cleanup();
        return false;
    }
    for (const auto& archiveFile : archiveFiles) {
        if (archiveFile == "archive-manifest.json") continue;
        const auto expected = expectedFiles.find(archiveFile);
        if (expected == expectedFiles.end()) {
            error = wxT("归档存在未登记文件：") + ToWxString(archiveFile);
            cleanup();
            return false;
        }
        const std::filesystem::path path = staging / std::filesystem::path(archiveFile);
        std::error_code sizeError;
        const std::uintmax_t actualSize = std::filesystem::file_size(path, sizeError);
        const std::string actualHash = sigflow::debug::Sha256File(path.string());
        if (sizeError || actualSize != expected->second.first || actualHash != expected->second.second) {
            error = wxT("归档完整性校验失败：") + ToWxString(archiveFile);
            cleanup();
            return false;
        }
    }
    Json::Value sessionManifest;
    if (!ReadJsonFile(staging / "manifest.json", sessionManifest, jsonError) ||
        sessionManifest.get("session_id", "").asString() != importedId ||
        !sessionManifest["state"].isString()) {
        error = wxT("会话 manifest 无效");
        cleanup();
        return false;
    }
    const std::filesystem::path destination = debugRoot / importedId;
    if (std::filesystem::exists(destination)) {
        error = wxT("目标会话已存在：") + ToWxString(importedId);
        cleanup();
        return false;
    }
    std::filesystem::rename(staging, destination, fsError);
    if (fsError) {
        error = wxT("无法安装导入会话：") + ToWxString(fsError.message());
        cleanup();
        return false;
    }
    keepStaging = true;
    sessionId = ToWxString(importedId);
    return true;
}

bool TraceBridgeWindow::SaveCompareJson(const wxString& sessionDir, const wxString& sessionId,
                                        const sigflow::debug::ComparisonResult& r)
{
    if (sessionDir.IsEmpty()) return false;
    if (!wxDirExists(sessionDir)) wxMkdir(sessionDir);
    const wxString path = JoinPath(sessionDir, "compare.json");

    Json::Value root;
    root["schema_version"] = "1.0";
    root["session_id"] = ToUtf8(sessionId);
    root["aligned"] = r.aligned;
    root["quality"] = r.alignment.qualityScore;
    root["total_signals_compared"] = Json::UInt64(r.totalSignalsCompared);
    root["total_diffs"] = Json::UInt64(r.totalDiffs);
    root["summary"] = r.summary;
    root["reference_kind"] = r.referenceKind;
    if (r.referenceKind == "hardware-input-replay") {
        root["replay_consistency"]["verified"] = r.stimulusVerified;
        root["replay_consistency"]["score"] = r.stimulusScore;
    }

    Json::Value alignObj;
    alignObj["valid"] = r.alignment.valid;
    alignObj["time_offset"] = Json::Int64(r.alignment.timeOffset);
    alignObj["anchor_signal"] = r.alignment.anchorSignal;
    alignObj["hw_anchor_time"] = Json::UInt64(r.alignment.hwAnchorTime);
    alignObj["sim_anchor_time"] = Json::UInt64(r.alignment.simAnchorTime);
    alignObj["quality_score"] = r.alignment.qualityScore;
    alignObj["needs_manual_review"] = r.alignment.needsManualReview;
    alignObj["matched_anchors"] = Json::UInt64(r.alignment.matchedAnchors);
    alignObj["transaction_matches"] = Json::UInt64(r.alignment.transactionMatches);
    alignObj["transaction_compared"] = Json::UInt64(r.alignment.transactionCompared);
    alignObj["quality_message"] = r.alignment.qualityMessage;
    alignObj["message"] = r.alignment.message;
    switch (r.alignment.anchorKind) {
        case sigflow::debug::AnchorKind::ResetRelease: alignObj["anchor_kind"] = "ResetRelease"; break;
        case sigflow::debug::AnchorKind::TriggerHit:   alignObj["anchor_kind"] = "TriggerHit"; break;
        case sigflow::debug::AnchorKind::InputTxn:     alignObj["anchor_kind"] = "InputTxn"; break;
    }
    root["alignment"] = alignObj;
    Json::Value anchors;
    anchors["kind"] = alignObj["anchor_kind"];
    anchors["hw_time"] = Json::UInt64(r.alignment.hwAnchorTime);
    anchors["sim_time"] = Json::UInt64(r.alignment.simAnchorTime);
    root["anchors"] = anchors;

    Json::Value diffsArr(Json::arrayValue);
    for (const auto& d : r.firstDiffs) {
        Json::Value diffObj;
        diffObj["signal"] = d.signalName;
        diffObj["hw_time"] = Json::UInt64(d.hwTime);
        diffObj["sim_time"] = Json::UInt64(d.simTime);
        diffObj["expected"] = d.expectedValue;
        diffObj["actual"] = d.actualValue;
        diffObj["confidence"] = d.confidence;
        diffObj["source_path"] = d.location.sourcePath;
        diffObj["source_line"] = d.location.sourceLine;
        diffObj["sftree_path"] = d.location.sftreePath;
        diffObj["canvas_id"] = d.location.canvasId;
        Json::Value candidates(Json::arrayValue);
        for (const auto& candidate : d.upstreamCandidates) {
            candidates.append(candidate.location.signalName);
        }
        diffObj["upstream_candidates"] = candidates;
        diffsArr.append(diffObj);
    }
    root["first_differences"] = diffsArr;
    if (!diffsArr.empty()) root["first_difference"] = diffsArr[0];

    Json::StyledWriter writer;
    const std::string content = writer.write(root);
    wxFFileOutputStream out(path);
    if (!out.IsOk()) return false;
    out.Write(content.c_str(), content.size());
    return out.IsOk();
}
