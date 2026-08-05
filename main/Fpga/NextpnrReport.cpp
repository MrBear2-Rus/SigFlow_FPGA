// NextpnrReport.cpp

#include "NextpnrReport.h"
#include <wx/file.h>
#include <wx/datetime.h>
#include <wx/string.h>

// 构造函数
NextpnrReport::NextpnrReport() { }

// Terminal 摘要格式化
wxString NextpnrReport::FormatSummary(const NextpnrRunRecord& record)
{
    wxString summary;
    summary += wxT("\n[nextpnr analysis]\n");

    // 结果状态
    summary += record.errorCount == 0
        ? wxT("Result: succeeded.\n")
        : wxT("Result: failed.\n");

    // 器件信息
    const wxString deviceStr = record.deviceName.IsEmpty()
        ? wxString(wxT("unknown")) : record.deviceName;
    const wxString familyStr = record.familyName.IsEmpty()
        ? wxString(wxT("GW1N-9C")) : record.familyName;
    summary += wxString::Format(wxT("Target: %s, family %s.\n"), deviceStr, familyStr);

    // 阶段状态
    auto stageLabel = [](bool done) -> wxString {
        return done ? wxString(wxT("completed")) : wxString(wxT("not reached"));
    };
    summary += wxString::Format(
        wxT("Packing: %s; placement: %s; routing: %s.\n"),
        stageLabel(record.packCompleted),
        stageLabel(record.placeCompleted),
        stageLabel(record.routeCompleted));

    // 时序
    summary += FormatTimingSummary(record);

    // 资源
    summary += FormatResourceSummary(record);

    // 错误/告警计数
    if (record.errorCount > 0 || record.warningCount > 0) {
        summary += wxString::Format(wxT("%d warnings, %d errors.\n"),
                                     record.warningCount, record.errorCount);
    }

    // 错误详情和下一步建议
    summary += FormatErrorsSummary(record);

    return summary;
}

// 时序段
wxString NextpnrReport::FormatTimingSummary(const NextpnrRunRecord& record)
{
    if (record.maxFrequencyMHz <= 0.0) return wxEmptyString;

    return wxString::Format(
        wxT("Max frequency: %.2f MHz (%s at requested clock).\n"),
        record.maxFrequencyMHz,
        record.timingPassed ? wxT("PASS") : wxT("FAIL"));
}

// 资源利用率段
wxString NextpnrReport::FormatResourceSummary(const NextpnrRunRecord& record)
{
    if (record.resources.empty()) return wxEmptyString;

    wxString text = wxT("Resources:\n");
    for (const auto& kv : record.resources) {
        // 仅展示有用量的资源（利用率 > 0 或总量 >= 100 的全局资源）
        if (kv.second.percent > 0.0 || kv.second.total >= 100) {
            text += wxString::Format(wxT("  %-16s %4d / %-6d (%3.0f%%)\n"),
                                     kv.first, kv.second.used,
                                     kv.second.total, kv.second.percent);
        }
    }
    return text;
}

// 错误详情段（三层：逐条 -> 根因 -> 下一步）
wxString NextpnrReport::FormatErrorsSummary(const NextpnrRunRecord& record)
{
    if (record.errorCount <= 0) {
        return wxT("Next step: PnR complete. Run FPGA > Program Board to generate bitstream.\n");
    }

    wxString text;

    // ── 第一层：逐条错误详情 ──
    if (!record.classifiedErrors.empty()) {
        text += wxT("\n--- Error Details ---\n");
        int idx = 1;
        for (const auto& e : record.classifiedErrors) {
            text += wxString::Format(wxT("  [%d] %s\n"), idx, e.chineseDesc);
            if (!e.extractedDetail.IsEmpty()) {
                text += wxString::Format(wxT("      Detail: %s\n"), e.extractedDetail);
            }
            text += wxString::Format(wxT("      Source: line %d\n"), e.lineNumber);
            text += wxT("      Action: ") + e.suggestion + wxT("\n\n");
            ++idx;
        }
    }

    // ── 第二层：根因诊断 ──
    text += wxT("--- Root Cause ---\n");
    if (!record.packCompleted) {
        text += wxT("Packing did not complete. Check:\n");
        text += wxT("  - JSON netlist from Yosys is valid\n");
        text += wxT("  - --device and --vopt family match the target board\n");
        text += wxT("  - chipdb file exists under share/himbaechel/gowin/\n");
    } else if (!record.placeCompleted) {
        text += wxT("Placement did not complete. Check:\n");
        text += wxT("  - Resource usage fits within device capacity\n");
        text += wxT("  - CST constraints are physically valid (no pin conflicts)\n");
    } else if (!record.routeCompleted) {
        text += wxT("Routing did not complete. Check:\n");
        text += wxT("  - All top-level ports have IO_LOC bindings in CST\n");
        text += wxT("  - No pin assignments create routing congestion\n");
        text += wxT("  - Clock constraints are realistic for the target device\n");
    } else {
        text += wxT("PnR stages completed but errors reported.\n");
    }

    // ── 第三层：精确下一步建议 ──
    text += wxT("\n--- Suggested Next Action ---\n");
    if (!record.classifiedErrors.empty()) {
        const auto& first = record.classifiedErrors[0];
        if (first.chineseDesc.Contains(wxT("Unconstrained")) ||
            first.extractedDetail.Contains(wxT("IO"))) {
            text += wxT("Open FPGA > Pin Constraints editor, bind all unconstrained ports,\n");
            text += wxT("then re-run Place and Route.\n");
        } else if (first.chineseDesc.Contains(wxT("JSON")) ||
                   first.chineseDesc.Contains(wxT("网表"))) {
            text += wxT("Re-run FPGA > Synthesis to regenerate the JSON netlist.\n");
        } else if (first.chineseDesc.Contains(wxT("CST")) ||
                   first.chineseDesc.Contains(wxT("约束"))) {
            text += wxT("Fix the CST constraint file and re-run Place and Route.\n");
        } else if (first.chineseDesc.Contains(wxT("芯片")) ||
                   first.chineseDesc.Contains(wxT("device"))) {
            text += wxT("Correct the --device parameter in sigflow.project > fpga.nextpnr_args.\n");
        } else {
            text += wxT("Review the error details above and fix the underlying issue.\n");
        }
    }

    return text;
}

// JSON 报告生成
wxString NextpnrReport::GenerateJson(const NextpnrRunRecord& record)
{
    wxString json;
    json += wxT("{\n");
    json += wxT("  \"tool\": \"")    + EscapeJson(record.toolName)    + wxT("\",\n");
    json += wxT("  \"version\": \"") + EscapeJson(record.toolVersion) + wxT("\",\n");
    json += wxT("  \"device\": \"")  + EscapeJson(record.deviceName)  + wxT("\",\n");
    json += wxT("  \"family\": \"")  + EscapeJson(record.familyName)  + wxT("\",\n");
    json += wxT("  \"exit_code\": ") + wxString::Format(wxT("%d"), record.exitCode) + wxT(",\n");
    json += wxT("  \"stages\": {\n");
    json += wxT("    \"pack\": ")  + wxString(record.packCompleted ? wxT("true") : wxT("false")) + wxT(",\n");
    json += wxT("    \"place\": ") + wxString(record.placeCompleted ? wxT("true") : wxT("false")) + wxT(",\n");
    json += wxT("    \"route\": ") + wxString(record.routeCompleted ? wxT("true") : wxT("false")) + wxT("\n");
    json += wxT("  },\n");
    json += wxT("  \"timing\": {\n");
    json += wxT("    \"clock\": \"")       + EscapeJson(record.clockName)                       + wxT("\",\n");
    json += wxT("    \"max_freq_mhz\": ")  + wxString::Format(wxT("%.2f"), record.maxFrequencyMHz) + wxT(",\n");
    json += wxT("    \"passed\": ")        + wxString(record.timingPassed ? wxT("true") : wxT("false")) + wxT("\n");
    json += wxT("  },\n");
    json += wxT("  \"resources\": {\n");
    bool first = true;
    for (const auto& kv : record.resources) {
        if (!first) json += wxT(",\n");
        first = false;
        json += wxT("    \"") + EscapeJson(kv.first) + wxT("\": {\n");
        json += wxT("      \"used\": ")    + wxString::Format(wxT("%d"), kv.second.used)    + wxT(",\n");
        json += wxT("      \"total\": ")   + wxString::Format(wxT("%d"), kv.second.total)   + wxT(",\n");
        json += wxT("      \"percent\": ") + wxString::Format(wxT("%.1f"), kv.second.percent) + wxT("\n");
        json += wxT("    }");
    }
    json += wxT("\n  },\n");
    json += wxT("  \"warnings\": ") + wxString::Format(wxT("%d"), record.warningCount) + wxT(",\n");
    json += wxT("  \"errors\": ")   + wxString::Format(wxT("%d"), record.errorCount)   + wxT("\n");
    json += wxT("}\n");

    return json;
}

// 保存 JSON 报告到文件
bool NextpnrReport::SaveReport(const NextpnrRunRecord& record, const wxString& filePath)
{
    wxString json = GenerateJson(record);
    wxFile file(filePath, wxFile::write);
    if (!file.IsOpened()) return false;
    const wxScopedCharBuffer utf8 = json.ToUTF8();
    file.Write(utf8.data(), utf8.length());
    file.Close();
    return true;
}

// JSON 字符串转义
wxString NextpnrReport::EscapeJson(const wxString& text)
{
    wxString escaped = text;
    escaped.Replace(wxT("\\"), wxT("\\\\"));
    escaped.Replace(wxT("\""), wxT("\\\""));
    escaped.Replace(wxT("\n"), wxT("\\n"));
    escaped.Replace(wxT("\r"), wxT("\\r"));
    escaped.Replace(wxT("\t"), wxT("\\t"));
    return escaped;
}
