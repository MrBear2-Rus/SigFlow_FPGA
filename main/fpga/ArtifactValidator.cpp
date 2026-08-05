#define _WIN32_WINNT 0x0601
#define WINVER       0x0601

#include <windows.h>

#include "ArtifactValidator.h"

#include <bcrypt.h>
#include <json/json.h>

#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <cstring>
#include <memory>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr size_t kHashBufferSize = 64 * 1024;
const wxString kYosysNetlistArtifactId = "yosys-netlist-json";

wxString NowUtc()
{
    return wxDateTime::UNow().FormatISOCombined('T') + "Z";
}

bool Fail(NetlistArtifactReport& report, ArtifactValidationStatus status,
          const wxString& message, const wxString& failedField = wxString())
{
    report.status = status;
    report.message = message;
    report.failedField = failedField;
    return false;
}

void InitializeReport(const wxString& jsonPath, const wxString& expectedTopModule,
                      NetlistArtifactReport& report)
{
    report = NetlistArtifactReport();
    report.artifactId = kYosysNetlistArtifactId;
    report.expectedTopModule = expectedTopModule;
    report.validatedAt = NowUtc();

    wxFileName normalizedPath(jsonPath);
    normalizedPath.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    report.path = normalizedPath.GetFullPath();
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

Json::Value ToJson(const NetlistArtifactReport& report)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["artifact_id"] = report.artifactId.ToStdString();
    root["status"] = ToString(report.status).ToStdString();
    root["path"] = report.path.ToStdString();
    root["expected_top_module"] = report.expectedTopModule.ToStdString();
    root["failed_field"] = report.failedField.ToStdString();
    root["message"] = report.message.ToStdString();
    root["validated_at"] = report.validatedAt.ToStdString();
    root["size_bytes"] = Json::UInt64(report.sizeBytes);
    root["sha256"] = report.sha256.ToStdString();

    Json::Value summary(Json::objectValue);
    summary["ports"] = Json::UInt64(report.portCount);
    summary["cells"] = Json::UInt64(report.cellCount);
    summary["netnames"] = Json::UInt64(report.netnameCount);
    root["summary"] = summary;
    return root;
}

bool WriteJsonAtomically(const wxString& path, const Json::Value& value, wxString& errorMessage)
{
    const wxString temporaryPath = path + ".tmp";
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const wxString content = wxString::FromUTF8(Json::writeString(writer, value)) + "\n";
    const wxScopedCharBuffer utf8 = content.ToUTF8();

    wxFile file(temporaryPath, wxFile::write);
    if (!file.IsOpened() || !utf8.data() ||
        file.Write(utf8.data(), utf8.length()) != static_cast<wxFileOffset>(utf8.length())) {
        file.Close();
        wxRemoveFile(temporaryPath);
        errorMessage = wxString("Unable to write artifact manifest: ") + path;
        return false;
    }
    file.Close();

    if (!wxRenameFile(temporaryPath, path, true)) {
        wxRemoveFile(temporaryPath);
        errorMessage = wxString("Unable to replace artifact manifest: ") + path;
        return false;
    }
    return true;
}

} // namespace

wxString ToString(ArtifactValidationStatus status)
{
    switch (status) {
    case ArtifactValidationStatus::NotValidated: return "NotValidated";
    case ArtifactValidationStatus::Valid: return "Valid";
    case ArtifactValidationStatus::MissingFile: return "MissingFile";
    case ArtifactValidationStatus::TooSmall: return "TooSmall";
    case ArtifactValidationStatus::ReadFailed: return "ReadFailed";
    case ArtifactValidationStatus::InvalidJson: return "InvalidJson";
    case ArtifactValidationStatus::MissingModules: return "MissingModules";
    case ArtifactValidationStatus::MissingTopModule: return "MissingTopModule";
    case ArtifactValidationStatus::MissingRequiredField: return "MissingRequiredField";
    case ArtifactValidationStatus::HashFailed: return "HashFailed";
    }
    return "Unknown";
}

ArtifactValidator::ArtifactValidator(ArtifactValidationOptions options)
    : m_options(options)
{
}

bool ArtifactValidator::ValidateYosysJson(const wxString& jsonPath, const wxString& expectedTopModule,
                                           NetlistArtifactReport& report) const
{
    InitializeReport(jsonPath, expectedTopModule, report);
    if (expectedTopModule.IsEmpty()) {
        return Fail(report, ArtifactValidationStatus::MissingTopModule,
                    "Expected top module name is empty.");
    }
    if (report.path.IsEmpty() || !wxFileExists(report.path)) {
        return Fail(report, ArtifactValidationStatus::MissingFile,
                    "Yosys JSON artifact does not exist.");
    }

    wxFileName fileName(report.path);
    const wxULongLong fileSize = fileName.GetSize();
    if (fileSize == wxInvalidSize) {
        return Fail(report, ArtifactValidationStatus::ReadFailed,
                    "Unable to obtain Yosys JSON artifact size.");
    }
    report.sizeBytes = static_cast<std::uint64_t>(fileSize.GetValue());
    if (report.sizeBytes < m_options.minimumFileSizeBytes) {
        return Fail(report, ArtifactValidationStatus::TooSmall,
                    wxString::Format("Yosys JSON artifact is only %llu bytes; minimum is %llu bytes.",
                                     static_cast<unsigned long long>(report.sizeBytes),
                                     static_cast<unsigned long long>(m_options.minimumFileSizeBytes)));
    }

    wxFile file(report.path, wxFile::read);
    wxString content;
    if (!file.IsOpened() || !file.ReadAll(&content)) {
        file.Close();
        return Fail(report, ArtifactValidationStatus::ReadFailed,
                    "Unable to read Yosys JSON artifact.");
    }
    file.Close();

    const wxScopedCharBuffer utf8 = content.ToUTF8();
    if (!utf8.data() || utf8.length() == 0) {
        return Fail(report, ArtifactValidationStatus::InvalidJson,
                    "Yosys JSON artifact is not valid UTF-8 JSON text.");
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseErrors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(utf8.data(), utf8.data() + utf8.length(), &root, &parseErrors)) {
        return Fail(report, ArtifactValidationStatus::InvalidJson,
                    wxString("Unable to parse Yosys JSON artifact: ") + wxString::FromUTF8(parseErrors));
    }
    if (!root.isObject() || !root.isMember("modules") || !root["modules"].isObject()) {
        return Fail(report, ArtifactValidationStatus::MissingModules,
                    "Yosys JSON artifact must contain an object field 'modules'.", "modules");
    }

    const Json::Value& modules = root["modules"];
    const wxScopedCharBuffer topModuleUtf8 = expectedTopModule.ToUTF8();
    if (!topModuleUtf8.data() || !modules.isMember(topModuleUtf8.data()) ||
        !modules[topModuleUtf8.data()].isObject()) {
        return Fail(report, ArtifactValidationStatus::MissingTopModule,
                    wxString("Expected top module '") + expectedTopModule +
                        "' was not found in the Yosys JSON artifact.",
                    "modules." + expectedTopModule);
    }

    const Json::Value& topModule = modules[topModuleUtf8.data()];
    for (const char* requiredField : { "ports", "cells", "netnames" }) {
        const wxString fieldName = wxString::FromUTF8(requiredField);
        if (!topModule.isMember(requiredField) || !topModule[requiredField].isObject()) {
            return Fail(report, ArtifactValidationStatus::MissingRequiredField,
                        wxString("Yosys JSON top module '") + expectedTopModule +
                            "' is missing required object field '" + fieldName + "'.",
                        "modules." + expectedTopModule + "." + fieldName);
        }
    }

    report.portCount = static_cast<std::uint64_t>(topModule["ports"].size());
    report.cellCount = static_cast<std::uint64_t>(topModule["cells"].size());
    report.netnameCount = static_cast<std::uint64_t>(topModule["netnames"].size());
    report.sha256 = Sha256File(report.path);
    if (report.sha256.IsEmpty()) {
        return Fail(report, ArtifactValidationStatus::HashFailed,
                    "Unable to calculate SHA-256 for Yosys JSON artifact.");
    }

    report.status = ArtifactValidationStatus::Valid;
    report.message = "Yosys JSON artifact is valid.";
    return true;
}

bool ArtifactValidator::WriteManifest(const wxString& manifestPath,
                                      const NetlistArtifactReport& report,
                                      wxString& errorMessage) const
{
    return WriteJsonAtomically(manifestPath, ToJson(report), errorMessage);
}
