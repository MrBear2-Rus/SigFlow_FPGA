#pragma once

#include "FpgaYosysRuntime.h"

#include <wx/string.h>

#include <vector>

enum class FpgaYosysSynthesisStrategy {
    Baseline,
    Debug,
    ResourceOptimized,
};

struct FpgaYosysStrategyInfo {
    wxString id;
    wxString version;
    wxString displayName;
};

struct FpgaYosysScriptRequest {
    std::vector<wxString> sourceFiles;
    wxString topModule;
    FpgaTargetProfile targetProfile;
    FpgaYosysSynthesisStrategy strategy = FpgaYosysSynthesisStrategy::Baseline;
    wxString outputJsonPath;
};

struct FpgaYosysScriptResult {
    bool success = false;
    wxString script;
    wxString errorMessage;
};

const FpgaYosysStrategyInfo& GetFpgaYosysStrategyInfo(FpgaYosysSynthesisStrategy strategy);
bool ParseFpgaYosysSynthesisStrategy(const wxString& value,
                                     FpgaYosysSynthesisStrategy& strategy);

class FpgaYosysScriptGenerator {
public:
    FpgaYosysScriptResult Generate(const FpgaYosysScriptRequest& request) const;
};
