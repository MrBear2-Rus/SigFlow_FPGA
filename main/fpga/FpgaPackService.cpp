#define _WIN32_WINNT 0x0601
#define WINVER       0x0601

#include "FpgaPackService.h"

#include <windows.h>
#include <bcrypt.h>
#include <json/json.h>

#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr size_t kHashBufferSize = 64 * 1024;
constexpr std::uint64_t kMinimumBitstreamSizeBytes = 32;

wxString NormalizePath(const wxString& path)
{
    wxFileName fileName(path);
    fileName.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    return fileName.GetFullPath();
}

wxString NowUtc()
{
    return wxDateTime::UNow().FormatISOCombined('T') + "Z";
}

bool GetFileSize(const wxString& path, std::uint64_t& sizeBytes)
{
    wxFileName fileName(path);
    const wxULongLong size = fileName.GetSize();
    if (size == wxInvalidSize) {
        return false;
    }
    sizeBytes = static_cast<std::uint64_t>(size.GetValue());
    return true;
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
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&hashObjectLength), sizeof(hashObjectLength),
                                   &bytesReturned, 0);
    }
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
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    if (status < 0) {
        return wxString();
    }

    wxString result;
    for (unsigned char byte : hashValue) {
        result += wxString::Format("%02x", byte);
    }
    return result;
}

Json::Value ToJson(const FpgaPackReport& report)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["tool"] = "gowin_pack";
    root["success"] = report.success;
    root["exit_code"] = report.exitCode;
    root["message"] = report.message.ToStdString();
    root["completed_at"] = report.completedAt.ToStdString();
    root["device"] = report.device.ToStdString();

    Json::Value input(Json::objectValue);
    input["pnr_json"] = report.inputPnrJsonPath.ToStdString();
    input["size_bytes"] = Json::UInt64(report.inputPnrJsonSizeBytes);
    input["sha256"] = report.inputPnrJsonSha256.ToStdString();
    root["input"] = input;

    Json::Value output(Json::objectValue);
    output["fs"] = report.bitstreamPath.ToStdString();
    output["size_bytes"] = Json::UInt64(report.bitstreamSizeBytes);
    output["sha256"] = report.bitstreamSha256.ToStdString();
    root["output"] = output;

    Json::Value executable(Json::objectValue);
    executable["path"] = report.executablePath.ToStdString();
    executable["sha256"] = report.executableSha256.ToStdString();
    root["executable"] = executable;
    return root;
}

} // namespace

bool FpgaPackService::ValidateInput(const wxString& pnrJsonPath, wxString& errorMessage) const
{
    const wxString normalizedPath = NormalizePath(pnrJsonPath);
    if (normalizedPath.IsEmpty() || !wxFileExists(normalizedPath)) {
        errorMessage = "PnR JSON artifact does not exist: " + normalizedPath;
        return false;
    }

    std::uint64_t sizeBytes = 0;
    if (!GetFileSize(normalizedPath, sizeBytes) || sizeBytes == 0) {
        errorMessage = "PnR JSON artifact is empty or cannot be read: " + normalizedPath;
        return false;
    }

    return true;
}

bool FpgaPackService::Finalize(const FpgaPackRequest& request, int exitCode,
                               FpgaPackReport& report) const
{
    report = FpgaPackReport();
    report.exitCode = exitCode;
    report.completedAt = NowUtc();
    report.inputPnrJsonPath = NormalizePath(request.pnrJsonPath);
    report.bitstreamPath = NormalizePath(request.bitstreamPath);
    report.executablePath = NormalizePath(request.executablePath);
    report.device = request.device;

    if (wxFileExists(report.inputPnrJsonPath)) {
        GetFileSize(report.inputPnrJsonPath, report.inputPnrJsonSizeBytes);
        report.inputPnrJsonSha256 = Sha256File(report.inputPnrJsonPath);
    }
    if (wxFileExists(report.executablePath)) {
        report.executableSha256 = Sha256File(report.executablePath);
    }

    if (exitCode != 0) {
        report.message = wxString::Format("gowin_pack exited with code %d.", exitCode);
        return false;
    }
    if (!wxFileExists(report.bitstreamPath)) {
        report.message = "gowin_pack reported success but did not create the .fs artifact.";
        return false;
    }
    if (!GetFileSize(report.bitstreamPath, report.bitstreamSizeBytes) ||
        report.bitstreamSizeBytes < kMinimumBitstreamSizeBytes) {
        report.message = "The generated .fs artifact is too small or cannot be read.";
        return false;
    }
    report.bitstreamSha256 = Sha256File(report.bitstreamPath);
    if (report.inputPnrJsonSha256.IsEmpty() || report.executableSha256.IsEmpty() ||
        report.bitstreamSha256.IsEmpty()) {
        report.message = "Unable to calculate SHA-256 for a pack input or output artifact.";
        return false;
    }

    report.success = true;
    report.message = "Apicula .fs artifact is valid.";
    return true;
}

bool FpgaPackService::WriteManifest(const wxString& manifestPath, const FpgaPackReport& report,
                                    wxString& errorMessage) const
{
    const wxString temporaryPath = manifestPath + ".tmp";
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const wxString content = wxString::FromUTF8(Json::writeString(writer, ToJson(report))) + "\n";
    const wxScopedCharBuffer utf8 = content.ToUTF8();

    wxFile file(temporaryPath, wxFile::write);
    if (!file.IsOpened() || !utf8.data() ||
        file.Write(utf8.data(), utf8.length()) != static_cast<wxFileOffset>(utf8.length())) {
        file.Close();
        wxRemoveFile(temporaryPath);
        errorMessage = "Unable to write pack manifest: " + manifestPath;
        return false;
    }
    file.Close();

    if (!wxRenameFile(temporaryPath, manifestPath, true)) {
        wxRemoveFile(temporaryPath);
        errorMessage = "Unable to replace pack manifest: " + manifestPath;
        return false;
    }
    return true;
}
