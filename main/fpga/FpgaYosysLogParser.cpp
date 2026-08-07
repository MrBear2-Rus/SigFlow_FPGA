#include "FpgaYosysLogParser.h"

#include <wx/regex.h>
#include <wx/tokenzr.h>

#include <algorithm>

namespace {

wxString Lower(const wxString& value)
{
    wxString result = value;
    result.MakeLower();
    return result;
}

bool ContainsAny(const wxString& line, std::initializer_list<const char*> values)
{
    const wxString lower = Lower(line);
    for (const char* value : values) {
        if (lower.Contains(wxString::FromUTF8(value))) {
            return true;
        }
    }
    return false;
}

} // namespace

wxString ToString(YosysLogSeverity severity)
{
    switch (severity) {
    case YosysLogSeverity::Info: return "INFO";
    case YosysLogSeverity::Warning: return "WARNING";
    case YosysLogSeverity::Error: return "ERROR";
    }
    return "INFO";
}

YosysLogRecord FpgaYosysLogParser::Parse(const wxString& combinedLog) const
{
    YosysLogRecord record;
    wxString currentStage = "startup";
    int lineNumber = 0;
    int sequence = 0;
    bool inCellStatistics = false;

    wxStringTokenizer lines(combinedLog, "\n", wxTOKEN_RET_EMPTY_ALL);
    while (lines.HasMoreTokens()) {
        ++lineNumber;
        wxString line = lines.GetNextToken();
        line.Trim(true).Trim(false);
        if (line.IsEmpty()) {
            continue;
        }

        if (record.toolVersion.IsEmpty()) {
            wxRegEx version("[Yy]osys[[:space:]]+([0-9][^[:space:],]*)");
            if (version.Matches(line)) {
                record.toolVersion = version.GetMatch(line, 1);
            }
        }
        UpdateResourceUsage(record, line, inCellStatistics);

        const wxString detectedStage = DetectStage(line);
        if (!detectedStage.IsEmpty()) {
            currentStage = detectedStage;
            auto stage = std::find_if(record.stages.begin(), record.stages.end(),
                [&currentStage](const YosysLogStage& item) { return item.name == currentStage; });
            if (stage == record.stages.end()) {
                record.stages.push_back({ currentStage, "Running", lineNumber, lineNumber });
            } else {
                stage->lastLine = lineNumber;
                stage->status = "Running";
            }
        }

        YosysLogEvent event;
        event.sequence = ++sequence;
        event.lineNumber = lineNumber;
        event.severity = DetectSeverity(line);
        event.stage = currentStage;
        event.rawLine = line;
        event.evidence = line;
        Classify(event);
        ExtractSourceLocation(event);
        if (event.severity != YosysLogSeverity::Info || event.ruleId != "INFO") {
            record.events.push_back(event);
        }
        if (event.severity == YosysLogSeverity::Warning) {
            ++record.warningCount;
        } else if (event.severity == YosysLogSeverity::Error) {
            ++record.errorCount;
            if (record.failedStage.IsEmpty()) {
                record.failedStage = event.stage;
            }
            if (record.rootCause.IsEmpty()) {
                record.rootCause = event.ruleId;
                record.rootSuggestion = event.suggestion;
            }
        }
    }

    for (YosysLogStage& stage : record.stages) {
        stage.status = record.errorCount > 0 && stage.name == record.failedStage
            ? "Failed" : "Observed";
    }
    return record;
}

void FpgaYosysLogParser::UpdateResourceUsage(YosysLogRecord& record, const wxString& line,
                                              bool& inCellStatistics)
{
    wxRegEx totalCells("^Number of cells:[[:space:]]*([0-9]+)");
    if (totalCells.Matches(line)) {
        long count = 0;
        if (totalCells.GetMatch(line, 1).ToLong(&count)) {
            record.resources.totalCells = static_cast<int>(count);
        }
        inCellStatistics = true;
        return;
    }

    if (!inCellStatistics) {
        return;
    }
    if (line.StartsWith("Number of ") || line.StartsWith("=== ")) {
        inCellStatistics = false;
        return;
    }

    wxRegEx cellType("^([^[:space:]]+)[[:space:]]+([0-9]+)$");
    if (!cellType.Matches(line)) {
        return;
    }

    long count = 0;
    if (!cellType.GetMatch(line, 2).ToLong(&count)) {
        return;
    }
    const wxString type = cellType.GetMatch(line, 1);
    record.resources.cellTypes.push_back({ type, static_cast<int>(count) });
    wxString upper = type;
    upper.MakeUpper();
    if (upper.Contains("LUT")) record.resources.lutCount += static_cast<int>(count);
    if (upper.Contains("DFF")) record.resources.dffCount += static_cast<int>(count);
    if (upper.Contains("IBUF")) record.resources.ibufCount += static_cast<int>(count);
    if (upper.Contains("OBUF")) record.resources.obufCount += static_cast<int>(count);
}

wxString FpgaYosysLogParser::DetectStage(const wxString& line)
{
    const wxString lower = Lower(line);
    if (lower.Contains("read_verilog") || lower.Contains("parsing verilog")) return "read_verilog";
    if (lower.Contains("hierarchy") || lower.Contains("top module")) return "hierarchy";
    if (lower.Contains("proc_" ) || lower.Contains("proc pass")) return "proc";
    if (lower.Contains("synth_gowin") || lower.Contains("gowin")) return "synth_gowin";
    if (lower.Contains("abc9") || lower.Contains("executing abc")) return "abc9";
    if (lower.Contains("write_json") || lower.Contains("writing json")) return "write_json";
    return wxEmptyString;
}

YosysLogSeverity FpgaYosysLogParser::DetectSeverity(const wxString& line)
{
    if (ContainsAny(line, { "error:", "error ", "syntax error", "fatal:", "failed" })) {
        return YosysLogSeverity::Error;
    }
    if (ContainsAny(line, { "warning:", "warning " })) {
        return YosysLogSeverity::Warning;
    }
    return YosysLogSeverity::Info;
}

void FpgaYosysLogParser::Classify(YosysLogEvent& event)
{
    const wxString lower = Lower(event.rawLine);
    event.ruleId = "INFO";
    event.suggestion = "";
    if (event.severity == YosysLogSeverity::Warning) {
        event.ruleId = "YOSYS_WARNING";
        event.suggestion = "检查该警告涉及的 RTL 和综合策略。";
        return;
    }
    if (event.severity != YosysLogSeverity::Error) {
        return;
    }
    if (lower.Contains("syntax error") || lower.Contains("parse error")) {
        event.ruleId = "YOSYS_RTL_SYNTAX";
        event.suggestion = "检查报告中的源文件、行号和 Verilog/SystemVerilog 语法。";
    } else if (lower.Contains("top module") && (lower.Contains("not found") || lower.Contains("not exist"))) {
        event.ruleId = "YOSYS_TOP_MISSING";
        event.suggestion = "确认 build.top_module 与 RTL 中的 module 名称一致。";
    } else if (lower.Contains("undefined module") || lower.Contains("blackbox")) {
        event.ruleId = "YOSYS_UNDEFINED_MODULE";
        event.suggestion = "补充缺失的 RTL/library 文件，或检查实例化模块名称。";
    } else if (lower.Contains("cells_sim.v") || lower.Contains("share") || lower.Contains("command not found")) {
        event.ruleId = "YOSYS_RUNTIME_MISSING";
        event.suggestion = "运行 Runtime 预检，恢复缺失的 Yosys share 文件或命令。";
    } else if (lower.Contains("unsupported") || lower.Contains("not supported")) {
        event.ruleId = "YOSYS_UNSUPPORTED_FEATURE";
        event.suggestion = "将 RTL 改写为目标器件支持的结构，或切换受控综合策略。";
    } else if (lower.Contains("abc") || lower.Contains("aiger")) {
        event.ruleId = "YOSYS_ABC_FAILURE";
        event.retryable = true;
        event.suggestion = "保留任务证据，尝试 debug 或 resource_optimized 策略并比较结果。";
    } else if (lower.Contains("write_json") || lower.Contains("cannot open")) {
        event.ruleId = "YOSYS_OUTPUT_FAILURE";
        event.suggestion = "检查任务目录权限、磁盘空间和输出路径。";
    } else {
        event.ruleId = "UNKNOWN_YOSYS_ERROR";
        event.suggestion = "打开完整日志，根据首个错误定位根因；必要时保留任务目录提交平台问题。";
    }
}

void FpgaYosysLogParser::ExtractSourceLocation(YosysLogEvent& event)
{
    wxRegEx location("^(.*):([0-9]+)(?::([0-9]+))?:");
    if (!location.Matches(event.rawLine)) {
        return;
    }
    event.sourceFile = location.GetMatch(event.rawLine, 1);
    long value = 0;
    if (location.GetMatch(event.rawLine, 2).ToLong(&value)) event.sourceLine = static_cast<int>(value);
    if (location.GetMatch(event.rawLine, 3).ToLong(&value)) event.sourceColumn = static_cast<int>(value);
}
