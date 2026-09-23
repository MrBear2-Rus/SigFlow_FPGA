#include "NextpnrReportGen.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>

namespace eda {
namespace pnr {
namespace {

std::string Format(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

NextpnrReport::NextpnrReport() = default;

std::string NextpnrReport::FormatSummary(const NextpnrRunRecord& record) {
    std::string summary;
    summary += "\n[nextpnr analysis]\n";
    summary += record.errorCount == 0 ? "Result: succeeded.\n" : "Result: failed.\n";

    const std::string device = record.deviceName.empty() ? "unknown" : record.deviceName;
    const std::string family = record.familyName.empty() ? "GW1N-9C" : record.familyName;
    summary += Format("Target: %s, family %s.\n", device.c_str(), family.c_str());

    const auto stageLabel = [](bool done) { return done ? "reached" : "not reached"; };
    summary += Format("Packing: %s; placement: %s; routing: %s.\n",
                      stageLabel(record.packCompleted), stageLabel(record.placeCompleted),
                      stageLabel(record.routeCompleted));

    summary += FormatTimingSummary(record);
    summary += FormatResourceSummary(record);
    if (record.errorCount > 0 || record.warningCount > 0) {
        summary += Format("%d warnings, %d errors.\n", record.warningCount, record.errorCount);
    }
    summary += FormatErrorsSummary(record);
    return summary;
}

std::string NextpnrReport::FormatTimingSummary(const NextpnrRunRecord& record) {
    if (record.maxFrequencyMHz <= 0.0) return {};
    return Format("Max frequency: %.2f MHz (%s at requested clock).\n", record.maxFrequencyMHz,
                  record.timingPassed ? "PASS" : "FAIL");
}

std::string NextpnrReport::FormatResourceSummary(const NextpnrRunRecord& record) {
    if (record.resources.empty()) return {};
    std::string text = "Resources:\n";
    for (const auto& kv : record.resources) {
        if (kv.second.percent > 0.0 || kv.second.total >= 100) {
            text += Format("  %-16s %4d / %-6d (%3.0f%%)\n", kv.first.c_str(), kv.second.used,
                           kv.second.total, kv.second.percent);
        }
    }
    return text;
}

std::string NextpnrReport::FormatErrorsSummary(const NextpnrRunRecord& record) {
    if (record.errorCount <= 0) {
        return "Next step: PnR complete. Run FPGA > Program Board to generate bitstream.\n";
    }

    std::string text;
    if (!record.classifiedErrors.empty()) {
        text += "\n--- Error Details ---\n";
        int index = 1;
        for (const auto& error : record.classifiedErrors) {
            text += Format("  [%d] %s\n", index, error.chineseDesc.c_str());
            if (!error.extractedDetail.empty()) {
                text += Format("      Detail: %s\n", error.extractedDetail.c_str());
            }
            text += Format("      Source: line %d\n", error.lineNumber);
            text += "      Action: " + error.suggestion + "\n\n";
            ++index;
        }
    }

    text += "--- Root Cause ---\n";
    if (!record.packCompleted) {
        text += "Packing did not complete. Check:\n";
        text += "  - JSON netlist from Yosys is valid\n";
        text += "  - --device and --vopt family match the target board\n";
        text += "  - chipdb file exists under share/himbaechel/gowin/\n";
    } else if (!record.placeCompleted) {
        text += "Placement did not complete. Check:\n";
        text += "  - Resource usage fits within device capacity\n";
        text += "  - CST constraints are physically valid (no pin conflicts)\n";
    } else if (!record.routeCompleted) {
        text += "Routing did not complete. Check:\n";
        text += "  - All top-level ports have IO_LOC bindings in CST\n";
        text += "  - No pin assignments create routing congestion\n";
        text += "  - Clock constraints are realistic for the target device\n";
    } else {
        text += "PnR stages completed but errors reported.\n";
    }

    text += "\n--- Suggested Next Action ---\n";
    if (!record.classifiedErrors.empty()) {
        const auto& first = record.classifiedErrors[0];
        if (Contains(first.chineseDesc, "Unconstrained") || Contains(first.extractedDetail, "IO")) {
            text += "Open FPGA > Pin Constraints editor, bind all unconstrained ports,\n";
            text += "then re-run Place and Route.\n";
        } else if (Contains(first.chineseDesc, "JSON") || Contains(first.chineseDesc, "netlist")) {
            text += "Re-run FPGA > Synthesis to regenerate the JSON netlist.\n";
        } else if (Contains(first.chineseDesc, "CST") || Contains(first.chineseDesc, "constraint")) {
            text += "Fix the CST constraint file and re-run Place and Route.\n";
        } else if (Contains(first.chineseDesc, "device") || Contains(first.chineseDesc, "Device")) {
            text += "Correct the --device parameter in sigflow.project > fpga.nextpnr_args.\n";
        } else {
            text += "Review the error details above and fix the underlying issue.\n";
        }
    }
    return text;
}

std::string NextpnrReport::GenerateJson(const NextpnrRunRecord& record) {
    std::string json;
    json += "{\n";
    json += "  \"tool\": \"" + EscapeJson(record.toolName) + "\",\n";
    json += "  \"version\": \"" + EscapeJson(record.toolVersion) + "\",\n";
    json += "  \"device\": \"" + EscapeJson(record.deviceName) + "\",\n";
    json += "  \"family\": \"" + EscapeJson(record.familyName) + "\",\n";
    json += "  \"exit_code\": " + std::to_string(record.exitCode) + ",\n";
    json += "  \"stages\": {\n";
    json += std::string("    \"pack\": ") + (record.packCompleted ? "true" : "false") + ",\n";
    json += std::string("    \"place\": ") + (record.placeCompleted ? "true" : "false") + ",\n";
    json += std::string("    \"route\": ") + (record.routeCompleted ? "true" : "false") + "\n";
    json += "  },\n";
    json += "  \"timing\": {\n";
    json += "    \"clock\": \"" + EscapeJson(record.clockName) + "\",\n";
    json += "    \"max_freq_mhz\": " + Format("%.2f", record.maxFrequencyMHz) + ",\n";
    json += std::string("    \"passed\": ") + (record.timingPassed ? "true" : "false") + "\n";
    json += "  },\n";
    json += "  \"resources\": {\n";
    bool first = true;
    for (const auto& kv : record.resources) {
        if (!first) json += ",\n";
        first = false;
        json += "    \"" + EscapeJson(kv.first) + "\": {\n";
        json += "      \"used\": " + std::to_string(kv.second.used) + ",\n";
        json += "      \"total\": " + std::to_string(kv.second.total) + ",\n";
        json += "      \"percent\": " + Format("%.1f", kv.second.percent) + "\n";
        json += "    }";
    }
    json += "\n  },\n";
    json += "  \"warnings\": " + std::to_string(record.warningCount) + ",\n";
    json += "  \"errors\": " + std::to_string(record.errorCount) + "\n";
    json += "}\n";
    return json;
}

bool NextpnrReport::SaveReport(const NextpnrRunRecord& record, const std::string& filePath) {
    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << GenerateJson(record);
    return static_cast<bool>(out);
}

std::string NextpnrReport::EscapeJson(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped += c; break;
        }
    }
    return escaped;
}

} // namespace pnr
} // namespace eda
