
#define _WIN32_WINNT 0x0601
#define WINVER       0x0601
#include <windows.h>
#include "FpgaYosysRuntime.h"

#include <bcrypt.h>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr size_t kHashBufferSize = 64 * 1024;

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
    wxFileName shareDirectory = wxFileName::DirName(executable.GetPath() + "\\..\\share");
    shareDirectory.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    return shareDirectory.GetPath();
}

wxString Sha256File(const wxString& filePath)
{
    wxFile file(filePath, wxFile::read);
    if (!file.IsOpened()) {
        return wxString();
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashObjectLength = 0;
    DWORD hashLength = 0;
    DWORD bytesReturned = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        return wxString();
    }

    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashObjectLength), sizeof(hashObjectLength),
                               &bytesReturned, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                   reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                                   &bytesReturned, 0);
    }

    std::vector<unsigned char> hashObject(hashObjectLength);
    std::vector<unsigned char> hashValue(hashLength);
    if (status >= 0) {
        status = BCryptCreateHash(algorithm, &hash, hashObject.data(), hashObjectLength, nullptr, 0, 0);
    }

    std::vector<unsigned char> buffer(kHashBufferSize);
    while (status >= 0) {
        const wxFileOffset bytesRead = file.Read(buffer.data(), buffer.size());
        if (bytesRead == wxInvalidOffset) {
            status = -1;
            break;
        }
        if (bytesRead == 0) {
            break;
        }
        status = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(bytesRead), 0);
    }

    if (status >= 0) {
        status = BCryptFinishHash(hash, hashValue.data(), hashLength, 0);
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (status < 0) {
        return wxString();
    }

    wxString result;
    for (unsigned char byte : hashValue) {
        result += wxString::Format("%02x", byte);
    }
    return result;
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

void AddFileCheck(FpgaYosysRuntimeReport& report, const wxString& id, const wxString& path)
{
    FpgaYosysRuntimeCheck check;
    check.id = id;
    check.path = path;
    check.passed = wxFileExists(path);
    check.message = check.passed ? "File is available." : "Required file is missing.";
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

} // namespace

const FpgaTargetProfile& GetTangNano9kTargetProfile()
{
    static const FpgaTargetProfile profile = {
        "tang-nano-9k",
        "1.0.0",
        "Sipeed Tang Nano 9K",
        "GW1NR-LV9QN88PC6/I5",
        "GW1N-9C",
        "gw1n",
        "tangnano9k",
    };
    return profile;
}

bool ResolveFpgaTargetProfile(const wxString& profileId, FpgaTargetProfile& profile,
                              wxString& errorMessage)
{
    const wxString normalizedId = profileId.IsEmpty() ? wxString("tang-nano-9k") : profileId.Lower();
    if (normalizedId == "tang-nano-9k") {
        profile = GetTangNano9kTargetProfile();
        return true;
    }

    errorMessage = wxString("Unsupported FPGA target profile: ") + profileId +
                   ". Supported profile: tang-nano-9k.";
    return false;
}

FpgaYosysRuntimeReport ValidateYosysRuntime(const wxString& executablePath)
{
    FpgaYosysRuntimeReport report;
    report.executablePath = executablePath;
    report.shareDirectory = GetShareDirectory(executablePath);

    AddFileCheck(report, "yosys-executable", executablePath);
    const wxString executableDirectory = wxFileName(executablePath).GetPath();
    AddFileCheck(report, "yosys-abc", JoinPath(executableDirectory, "yosys-abc.exe"));

    const std::vector<wxString> shareFiles = {
        "gowin/cells_sim.v",
        "gowin/cells_xtra_gw1n.v",
        "gowin/arith_map.v",
        "gowin/brams.txt",
        "gowin/brams_map.v",
        "gowin/cells_latch.v",
        "gowin/cells_map.v",
        "gowin/dsp_map.v",
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
