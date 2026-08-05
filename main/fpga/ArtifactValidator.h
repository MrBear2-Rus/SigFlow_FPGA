#pragma once

#include <cstdint>

#include <wx/string.h>

enum class ArtifactValidationStatus {
    NotValidated,
    Valid,
    MissingFile,
    TooSmall,
    ReadFailed,
    InvalidJson,
    MissingModules,
    MissingTopModule,
    MissingRequiredField,
    HashFailed,
};

struct ArtifactValidationOptions {
    std::uint64_t minimumFileSizeBytes = 32;
};

struct NetlistArtifactReport {
    ArtifactValidationStatus status = ArtifactValidationStatus::NotValidated;

    wxString artifactId;
    wxString path;
    wxString expectedTopModule;
    wxString failedField;
    wxString message;
    wxString validatedAt;

    std::uint64_t sizeBytes = 0;
    std::uint64_t portCount = 0;
    std::uint64_t cellCount = 0;
    std::uint64_t netnameCount = 0;

    wxString sha256;
};

wxString ToString(ArtifactValidationStatus status);

class ArtifactValidator {
public:
    explicit ArtifactValidator(ArtifactValidationOptions options = {});

    bool ValidateYosysJson(const wxString& jsonPath, const wxString& expectedTopModule,
                           NetlistArtifactReport& report) const;
    bool WriteManifest(const wxString& manifestPath, const NetlistArtifactReport& report,
                       wxString& errorMessage) const;

private:
    ArtifactValidationOptions m_options;
};
