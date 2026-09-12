#pragma once

#include "FpgaYosysLogParser.h"

#include <wx/string.h>

class FpgaYosysReport {
public:
    static wxString GenerateJson(const YosysLogRecord& record);
    static wxString GenerateSummary(const YosysLogRecord& record);
    static bool Save(const YosysLogRecord& record, const wxString& jsonPath,
                     const wxString& summaryPath, wxString& errorMessage);
};
