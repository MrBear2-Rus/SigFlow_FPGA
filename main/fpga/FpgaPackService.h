#pragma once

#include <cstdint>

#include <wx/string.h>

struct FpgaPackRequest {
    wxString pnrJsonPath;
    wxString bitstreamPath;
    wxString executablePath;
    wxString device;
};

struct FpgaPackReport {
    bool success = false;
    int exitCode = -1;
    wxString message;
    wxString completedAt;

    wxString inputPnrJsonPath;
    std::uint64_t inputPnrJsonSizeBytes = 0;
    wxString inputPnrJsonSha256;

    wxString bitstreamPath;
    std::uint64_t bitstreamSizeBytes = 0;
    wxString bitstreamSha256;

    wxString executablePath;
    wxString executableSha256;
    wxString device;
};

class FpgaPackService {
public:
    bool ValidateInput(const wxString& pnrJsonPath, wxString& errorMessage) const;
    bool Finalize(const FpgaPackRequest& request, int exitCode, FpgaPackReport& report) const;
    bool WriteManifest(const wxString& manifestPath, const FpgaPackReport& report,
                       wxString& errorMessage) const;
};
