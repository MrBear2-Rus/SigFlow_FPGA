#pragma once

#include <wx/string.h>

#include <vector>

struct FpgaTargetProfile {
    wxString id;
    wxString version;
    wxString displayName;
    wxString device;
    wxString family;
    wxString yosysFamily;
    wxString programmerBoard;
};

struct FpgaYosysRuntimeCheck {
    wxString id;
    wxString path;
    wxString message;
    wxString sha256;
    bool required = true;
    bool passed = false;
};

struct FpgaYosysRuntimeReport {
    wxString executablePath;
    wxString shareDirectory;
    std::vector<FpgaYosysRuntimeCheck> checks;
    bool valid = false;

    wxString FormatForTerminal() const;
};

const FpgaTargetProfile& GetTangNano9kTargetProfile();
bool ResolveFpgaTargetProfile(const wxString& profileId, FpgaTargetProfile& profile,
                              wxString& errorMessage);

FpgaYosysRuntimeReport ValidateYosysRuntime(const wxString& executablePath);
bool WriteYosysRuntimeManifest(const FpgaYosysRuntimeReport& report, const wxString& manifestPath,
                               wxString& errorMessage);
