
#include "FpgaYosysRuntime.h"

#include "jobs/Sha256.h"
#include "platform/PlatformPaths.h"

#include "TargetProfileStore.h"
#include <eda/api/target_profile.hpp>

#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <cstring>
#include <filesystem>
#include <vector>

namespace {

wxString JoinPath(const wxString& directory, const wxString& relativePath)
{
    wxString fullPath = directory;
    if (!fullPath.EndsWith("\\") && !fullPath.EndsWith("/")) {
        fullPath += wxFileName::GetPathSeparator();
    }
    fullPath += relativePath;
    wxFileName fileName(fullPath);
    fileName.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    return fileName.GetFullPath();
}

wxString GetShareDirectory(const wxString& executablePath)
{
    wxFileName executable(executablePath);
    // 两种常见布局：
    //   Windows 打包版：<prefix>/share          —— 数据直接位于 share 下
    //   Linux 发行版：  <prefix>/share/yosys    —— 数据位于 share/yosys 子目录
    // 必须用真实存在的文件来判定，否则会像此前一样解析到 /usr/share，
    // 让 15 项 share:* 检查全部失败（required=true → 预检失败 → 综合无法启动）。
    const wxString prefix = executable.GetPath();
    const wxString shareRoot = sigflow::platform::JoinPath(prefix, "..");
    const wxString candidates[] = {
        sigflow::platform::JoinPath(sigflow::platform::JoinPath(shareRoot, "share"), "yosys"),
        sigflow::platform::JoinPath(shareRoot, "share"),
    };

    const auto normalize = [](const wxString& path) {
        wxFileName fileName(wxFileName::DirName(path));
        fileName.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
        return fileName.GetPath();
    };

    for (const wxString& candidate : candidates) {
        if (wxFileExists(sigflow::platform::JoinPath(candidate, "techmap.v"))) {
            return normalize(candidate);
        }
    }
    return normalize(candidates[1]);
}

wxString Sha256File(const wxString& filePath)
{
    return Sha256FileHex(filePath);
}

bool HasYosysCommand(const wxString& executablePath, const wxString& commandName)
{
    wxArrayString standardOutput;
    wxArrayString standardError;
    const wxString command = wxString::Format("\"%s\" -Q -p \"help %s\"", executablePath, commandName);
    const long exitCode = wxExecute(command, standardOutput, standardError, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
    wxString output;
    for (const wxString& line : standardOutput) {
        output += line + "\n";
    }
    for (const wxString& line : standardError) {
        output += line + "\n";
    }
    return exitCode == 0 && !output.Contains("No such command") && output.Contains(commandName);
}

wxString EscapeJson(wxString value)
{
    value.Replace("\\", "\\\\");
    value.Replace("\"", "\\\"");
    value.Replace("\r", "\\r");
    value.Replace("\n", "\\n");
    return value;
}

void AddFileCheck(FpgaYosysRuntimeReport& report, const wxString& id, const wxString& path,
                  bool required = true)
{
    FpgaYosysRuntimeCheck check;
    check.id = id;
    check.path = path;
    check.required = required;
    check.passed = wxFileExists(path);
    check.message = check.passed ? "File is available."
                                 : (required ? "Required file is missing."
                                             : "Optional file is not present in this Yosys version.");
    if (check.passed) {
        check.sha256 = Sha256File(path);
        if (check.sha256.IsEmpty()) {
            check.passed = false;
            check.message = "Unable to calculate SHA-256.";
        }
    }
    report.checks.push_back(check);
}

void AddCommandCheck(FpgaYosysRuntimeReport& report, const wxString& executablePath,
                     const wxString& commandName)
{
    FpgaYosysRuntimeCheck check;
    check.id = wxString("command:") + commandName;
    check.path = executablePath;
    check.passed = HasYosysCommand(executablePath, commandName);
    check.message = check.passed ? "Yosys command is available." : "Required Yosys command is unavailable.";
    report.checks.push_back(check);
}

// ── P1-5：目标 profile 从 JSON 解析（取代硬编码）；找不到文件时用内置默认 JSON 兜底 ──
const char* kDefaultTangNano9kJson = R"JSON({
  "schema_version": "1.0",
  "id": "tang-nano-9k",
  "version": "1.0.0",
  "display_name": "Sipeed Tang Nano 9K",
  "yosys": { "family": "gw1n" },
  "nextpnr": { "device": "GW1NR-LV9QN88PC6/I5", "family": "GW1N-9C" },
  "openfpgaloader": { "board": "tangnano9k" }
})JSON";

std::filesystem::path FindTargetProfilesDirectory()
{
    const wxString executableDirectory =
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    const auto probe = [](const wxString& directory) -> wxString {
        const wxString candidate =
            directory + wxFileName::GetPathSeparator() + "main" + wxFileName::GetPathSeparator() +
            "fpga" + wxFileName::GetPathSeparator() + "target-profiles";
        return wxDirExists(candidate) ? candidate : wxString();
    };
    wxString found = sigflow::platform::WalkUpDirectories(executableDirectory, 8, probe);
    if (found.IsEmpty()) {
        found = sigflow::platform::WalkUpDirectories(wxGetCwd(), 8, probe);
    }
    if (found.IsEmpty()) return {};
    return sigflow::platform::Utf8Path(found);
}

void ApplyTargetProfile(const eda::TargetProfile& source, FpgaTargetProfile& target)
{
    target.id = wxString::FromUTF8(source.id.c_str());
    target.version = wxString::FromUTF8(source.version.c_str());
    target.displayName = wxString::FromUTF8(source.displayName.c_str());
    target.device = wxString::FromUTF8(source.device.c_str());
    target.family = wxString::FromUTF8(source.family.c_str());
    target.yosysFamily = wxString::FromUTF8(source.yosysFamily.c_str());
    target.programmerBoard = wxString::FromUTF8(source.programmerBoard.c_str());
}

bool LoadTargetProfileById(const wxString& id, FpgaTargetProfile& profile,
                           wxString& errorMessage)
{
    const std::string idUtf8 = sigflow::platform::Utf8String(id);
    eda::TargetProfile parsed;
    std::string error;

    const std::filesystem::path directory = FindTargetProfilesDirectory();
    if (!directory.empty() &&
        eda::target::TargetProfileStore::LoadById(directory, idUtf8, parsed, error)) {
        ApplyTargetProfile(parsed, profile);
        return true;
    }
    if (idUtf8 == "tang-nano-9k" &&
        eda::target::TargetProfileStore::Parse(kDefaultTangNano9kJson, parsed, error)) {
        ApplyTargetProfile(parsed, profile);
        return true;
    }
    errorMessage = wxString::FromUTF8(error.c_str());
    return false;
}

} // namespace

const FpgaTargetProfile& GetTangNano9kTargetProfile()
{
    static const FpgaTargetProfile profile = [] {
        FpgaTargetProfile resolved;
        wxString ignored;
        LoadTargetProfileById("tang-nano-9k", resolved, ignored);
        return resolved;
    }();
    return profile;
}

bool ResolveFpgaTargetProfile(const wxString& profileId, FpgaTargetProfile& profile,
                              wxString& errorMessage)
{
    const wxString normalizedId = profileId.IsEmpty() ? wxString("tang-nano-9k") : profileId.Lower();
    if (LoadTargetProfileById(normalizedId, profile, errorMessage)) {
        return true;
    }

    errorMessage = wxString("Unsupported FPGA target profile: ") + profileId +
                   ". Supported profile: tang-nano-9k. (" + errorMessage + ")";
    return false;
}

FpgaYosysRuntimeReport ValidateYosysRuntime(const wxString& executablePath)
{
    FpgaYosysRuntimeReport report;
    report.executablePath = executablePath;
    report.shareDirectory = GetShareDirectory(executablePath);

    AddFileCheck(report, "yosys-executable", executablePath);
    const wxString executableDirectory = wxFileName(executablePath).GetPath();
    // 不能硬编码 ".exe"：Linux 上 yosys-abc 没有扩展名，
    // 判成缺失会让预检整体失败（required=true），综合永远启动不了。
    AddFileCheck(report, "yosys-abc",
                 JoinPath(executableDirectory,
                          sigflow::platform::WithExecutableSuffix("yosys-abc")));

    const std::vector<wxString> shareFiles = {
        "gowin/cells_sim.v",
        "gowin/cells_xtra_gw1n.v",
        "gowin/arith_map.v",
        "gowin/brams.txt",
        "gowin/brams_map.v",
        "gowin/cells_map.v",
        "gowin/lutrams.txt",
        "gowin/lutrams_map.v",
        "mul2dsp.v",
        "techmap.v",
        "abc9_map.v",
        "abc9_model.v",
        "abc9_unmap.v",
    };
    for (const wxString& relativePath : shareFiles) {
        AddFileCheck(report, wxString("share:") + relativePath,
                     JoinPath(report.shareDirectory, relativePath));
    }

    // 版本相关的 techlib 文件：存在就校验，不存在只提示，**不影响 valid**。
    // yosys 0.4x 起已从 techlibs/gowin 移除 cells_latch.v 与 dsp_map.v
    //（功能并入其它 map 文件），而上面的清单是按更早的 yosys（随包分发的 0.38）
    // 写死的。若继续当作必需项，用新版 yosys 时预检必然失败，
    // 结果是综合被永久拦住 —— 但工具链其实完全可用。
    const std::vector<wxString> optionalShareFiles = {
        "gowin/cells_latch.v",
        "gowin/dsp_map.v",
    };
    for (const wxString& relativePath : optionalShareFiles) {
        AddFileCheck(report, wxString("share:") + relativePath,
                     JoinPath(report.shareDirectory, relativePath), false);
    }

    if (wxFileExists(executablePath)) {
        for (const wxString& command : { "synth_gowin", "chtype", "write_xaiger", "read_aiger", "write_json" }) {
            AddCommandCheck(report, executablePath, command);
        }
    }

    report.valid = true;
    for (const FpgaYosysRuntimeCheck& check : report.checks) {
        if (check.required && !check.passed) {
            report.valid = false;
            break;
        }
    }
    return report;
}

wxString FpgaYosysRuntimeReport::FormatForTerminal() const
{
    wxString text = wxString("[Yosys preflight] ") + (valid ? "passed" : "failed") + "\n";
    text += wxString("Executable: ") + executablePath + "\n";
    text += wxString("Share directory: ") + shareDirectory + "\n";
    for (const FpgaYosysRuntimeCheck& check : checks) {
        text += check.passed ? "  [OK] " : "  [FAIL] ";
        text += check.id + wxString(": ") + check.message;
        if (!check.path.IsEmpty()) {
            text += wxString(" (") + check.path + ")";
        }
        if (!check.sha256.IsEmpty()) {
            text += wxString("\n    SHA-256: ") + check.sha256;
        }
        text += "\n";
    }
    return text;
}

bool WriteYosysRuntimeManifest(const FpgaYosysRuntimeReport& report, const wxString& manifestPath,
                               wxString& errorMessage)
{
    wxFile file(manifestPath, wxFile::write);
    if (!file.IsOpened()) {
        errorMessage = wxString("Unable to create runtime manifest: ") + manifestPath;
        return false;
    }

    wxString content = "{\n";
    content += "  \"schema_version\": \"1.0\",\n";
    content += wxString("  \"valid\": ") + wxString(report.valid ? "true" : "false") + ",\n";
    content += wxString("  \"executable\": \"") + EscapeJson(report.executablePath) + "\",\n";
    content += wxString("  \"share_directory\": \"") + EscapeJson(report.shareDirectory) + "\",\n";
    content += "  \"checks\": [\n";
    for (size_t index = 0; index < report.checks.size(); ++index) {
        const FpgaYosysRuntimeCheck& check = report.checks[index];
        content += wxString("    {\"id\": \"") + EscapeJson(check.id) + "\", \"required\": " +
                   wxString(check.required ? "true" : "false") + ", \"passed\": " +
                   wxString(check.passed ? "true" : "false") + ", \"path\": \"" +
                   EscapeJson(check.path) + "\", \"sha256\": \"" + EscapeJson(check.sha256) +
                   "\", \"message\": \"" + EscapeJson(check.message) + "\"}";
        content += index + 1 == report.checks.size() ? "\n" : ",\n";
    }
    content += "  ]\n}\n";

    const wxScopedCharBuffer utf8 = content.ToUTF8();
    const char* data = utf8.data();
    const size_t length = data ? std::strlen(data) : 0;
    const bool written = file.Write(data, length) == static_cast<wxFileOffset>(length);
    file.Close();
    if (!written) {
        errorMessage = wxString("Unable to write runtime manifest: ") + manifestPath;
    }
    return written;
}
