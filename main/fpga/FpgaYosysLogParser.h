#pragma once

#include <wx/string.h>

#include <vector>

enum class YosysLogSeverity {
    Info,
    Warning,
    Error,
};

struct YosysLogEvent {
    int sequence = 0;
    int lineNumber = 0;
    YosysLogSeverity severity = YosysLogSeverity::Info;
    wxString stage;
    wxString ruleId;
    wxString rawLine;
    wxString sourceFile;
    int sourceLine = 0;
    int sourceColumn = 0;
    wxString evidence;
    wxString suggestion;
    bool retryable = false;
};

struct YosysLogStage {
    wxString name;
    wxString status = "Unknown";
    int firstLine = 0;
    int lastLine = 0;
};

struct YosysResourceItem {
    wxString type;
    int count = 0;
};

struct YosysResourceUsage {
    int totalCells = -1;
    int lutCount = 0;
    int dffCount = 0;
    int ibufCount = 0;
    int obufCount = 0;
    std::vector<YosysResourceItem> cellTypes;
};

struct YosysLogRecord {
    wxString jobId;
    wxString status;
    wxString toolVersion;
    wxString strategy;
    int durationMs = 0;
    int warningCount = 0;
    int errorCount = 0;
    wxString failedStage;
    wxString rootCause;
    wxString rootSuggestion;
    YosysResourceUsage resources;
    std::vector<YosysLogStage> stages;
    std::vector<YosysLogEvent> events;
    wxString combinedLogPath;
    wxString stdoutLogPath;
    wxString stderrLogPath;
    wxString artifactPath;
    wxString jobManifestPath;
    wxString inputManifestPath;
    wxString runtimeManifestPath;
    wxString scriptPath;
    wxString artifactManifestPath;
};

wxString ToString(YosysLogSeverity severity);

class FpgaYosysLogParser {
public:
    YosysLogRecord Parse(const wxString& combinedLog) const;

private:
    static wxString DetectStage(const wxString& line);
    static YosysLogSeverity DetectSeverity(const wxString& line);
    static void Classify(YosysLogEvent& event);
    static void ExtractSourceLocation(YosysLogEvent& event);
    static void UpdateResourceUsage(YosysLogRecord& record, const wxString& line,
                                    bool& inCellStatistics);
};
