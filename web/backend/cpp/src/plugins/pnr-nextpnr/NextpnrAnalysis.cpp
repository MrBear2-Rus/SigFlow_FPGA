#include "NextpnrAnalysis.h"
#include "NextpnrLogModel.h"
#include <algorithm>
#include <exception>

namespace eda::pnr {
Json AnalyzeNextpnrLog(const std::string& output, bool outputTruncated, const std::string& device,
                       const std::string& family, int exitCode) {
    constexpr std::size_t maxBytes = 4 * 1024 * 1024;
    const bool byteTruncated = output.size() > maxBytes;
    Json result{{"tool", "nextpnr"}, {"device", device},
                {"family", family},  {"exit_code", exitCode},
                {"parsed", false},   {"log_truncated", outputTruncated || byteTruncated}};
    if (output.empty())
        return result;
    try {
        NextpnrRunRecord record;
        record.deviceName = device;
        record.familyName = family;
        NextpnrLogParser parser;
        const auto bounded = output.substr(0, std::min(output.size(), maxBytes));
        // Tool output may not be valid UTF-8. Replace invalid bytes before putting
        // extracted fields into the JSON report; the raw log is preserved separately.
        const auto sanitized =
            Json::parse(Json(bounded).dump(-1, ' ', false, Json::error_handler_t::replace))
                .get<std::string>();
        if (!parser.Parse(sanitized, "", record))
            return result;
        result["parsed"] = true;
        result["log_truncated"] = outputTruncated || byteTruncated || record.logTruncated;
        result["version"] = record.toolVersion.empty() ? Json(nullptr) : Json(record.toolVersion);
        result["device"] = record.deviceName;
        result["stages_observed"] = {{"pack", record.packCompleted},
                                     {"place", record.placeCompleted},
                                     {"route", record.routeCompleted}};
        result["timing"] = nullptr;
        if (!record.clockName.empty())
            result["timing"] = {{"clock", record.clockName},
                                {"max_freq_mhz", record.maxFrequencyMHz},
                                {"passed", record.timingPassed}};
        result["resources"] = Json::object();
        for (const auto& [name, usage] : record.resources)
            result["resources"][name] = {
                {"used", usage.used}, {"total", usage.total}, {"percent", usage.percent}};
        result["warnings"] = record.warningCount;
        result["errors"] = record.errorCount;
        result["classified_errors"] = Json::array();
        for (const auto& error : record.classifiedErrors)
            result["classified_errors"].push_back({{"line", error.lineNumber},
                                                   {"message", error.chineseDesc},
                                                   {"detail", error.extractedDetail},
                                                   {"suggestion", error.suggestion}});
    } catch (const std::exception&) {
        // Malformed tool text is an analysis failure, not a tool execution failure.
        result["parsed"] = false;
        result["analysis_error"] = "unable to parse nextpnr output";
    }
    return result;
}
}  // namespace eda::pnr
