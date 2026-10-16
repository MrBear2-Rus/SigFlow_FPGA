#include "WavePerformanceProbe.h"

#include "../trace/TraceCache.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <utility>

namespace sigflow::wave {
namespace {

using sigflow::trace::TimeValue;

const char* StageName(WavePerformanceStage stage)
{
    switch (stage) {
        case WavePerformanceStage::FirstScreen: return "first_screen";
        case WavePerformanceStage::Zoom: return "zoom";
        case WavePerformanceStage::PatternSearch: return "pattern_search";
        case WavePerformanceStage::CancelledSearch: return "cancelled_search";
    }
    return "unknown";
}

void AddCacheStats(sigflow::trace::TraceSource& source, WavePerformanceSample& sample)
{
    const auto* cache = dynamic_cast<const sigflow::trace::CachingTraceSource*>(&source);
    if (!cache) return;
    const auto stats = cache->Stats();
    sample.cacheBytes = cache->CacheBytes();
    sample.cacheEntries = cache->CacheEntries();
    sample.queryHits = stats.queryHits;
    sample.queryMisses = stats.queryMisses;
    sample.valueHits = stats.valueHits;
    sample.valueMisses = stats.valueMisses;
}

TimeValue SaturatingAdd(TimeValue value, TimeValue delta)
{
    if (value > std::numeric_limits<TimeValue>::max() - delta) {
        return std::numeric_limits<TimeValue>::max();
    }
    return value + delta;
}

WavePerformanceSample RunQuery(sigflow::trace::TraceSource& source,
                               const sigflow::trace::SignalInfo& signal,
                               WavePerformanceStage stage,
                               TimeValue begin, TimeValue end)
{
    WavePerformanceSample sample;
    sample.stage = stage;
    const auto started = std::chrono::steady_clock::now();
    std::vector<sigflow::trace::Transition> transitions;
    std::string queryError;
    sample.success = source.Query(signal, begin, end, transitions, queryError);
    sample.transitions = transitions.size();
    sample.error = queryError;
    sample.elapsedUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    AddCacheStats(source, sample);
    return sample;
}

Json::Value SampleJson(const WavePerformanceSample& sample)
{
    Json::Value value(Json::objectValue);
    value["stage"] = StageName(sample.stage);
    value["success"] = sample.success;
    value["cancelled"] = sample.cancelled;
    value["elapsed_us"] = Json::UInt64(sample.elapsedUs);
    value["transitions"] = Json::UInt64(sample.transitions);
    value["cache_bytes"] = Json::UInt64(sample.cacheBytes);
    value["cache_entries"] = Json::UInt64(sample.cacheEntries);
    value["query_hits"] = Json::UInt64(sample.queryHits);
    value["query_misses"] = Json::UInt64(sample.queryMisses);
    value["value_hits"] = Json::UInt64(sample.valueHits);
    value["value_misses"] = Json::UInt64(sample.valueMisses);
    if (!sample.error.empty()) value["error"] = sample.error;
    return value;
}

} // namespace

bool RunWavePerformanceProbe(sigflow::trace::TraceSource& source,
                             const WavePerformanceProbeOptions& options,
                             WavePerformanceReport& report,
                             std::string& error)
{
    report = WavePerformanceReport();
    error.clear();
    report.sourcePath = source.Path();
    report.timeRange = source.TimeRange();
    report.conditions = options;
    if (!report.timeRange.valid || report.timeRange.end < report.timeRange.begin) {
        error = "performance probe requires a valid trace time range";
        return false;
    }
    std::error_code fileError;
    report.sourceBytes = std::filesystem::file_size(report.sourcePath, fileError);

    if (source.Signals().empty()) {
        error = "performance probe requires at least one signal";
        return false;
    }
    report.signalId = options.signalId < 0 ? source.Signals().front().id : options.signalId;
    const sigflow::trace::SignalInfo* signal = source.SignalById(report.signalId);
    if (!signal) {
        error = "performance probe signal id is not present in source";
        return false;
    }

    const TimeValue begin = report.timeRange.begin;
    const TimeValue available = report.timeRange.end - begin;
    const TimeValue firstSpan = options.firstScreenSpan == 0
        ? std::min<TimeValue>(available, 10000) : options.firstScreenSpan;
    const TimeValue firstEnd = std::min(report.timeRange.end, SaturatingAdd(begin, firstSpan));
    report.samples.push_back(RunQuery(source, *signal, WavePerformanceStage::FirstScreen,
                                      begin, firstEnd));

    const TimeValue zoomSpan = options.zoomSpan == 0
        ? std::max<TimeValue>(1, firstSpan / 8) : options.zoomSpan;
    const TimeValue zoomBegin = begin + (firstEnd - begin) / 2;
    const TimeValue zoomEnd = std::min(report.timeRange.end, SaturatingAdd(zoomBegin, zoomSpan));
    report.samples.push_back(RunQuery(source, *signal, WavePerformanceStage::Zoom,
                                      zoomBegin, zoomEnd));

    WavePatternSpec pattern;
    pattern.criteria.push_back({report.signalId, WavePatternKind::AnyEdge, {}});
    WavePatternSearchOptions searchOptions;
    searchOptions.maxCandidates = options.searchMaxCandidates;
    const auto searchStarted = std::chrono::steady_clock::now();
    std::string searchError;
    const auto matches = WavePatternCollect(source, pattern, begin, firstEnd,
                                            searchOptions, searchError);
    WavePerformanceSample searchSample;
    searchSample.stage = WavePerformanceStage::PatternSearch;
    searchSample.success = searchError.empty();
    searchSample.transitions = matches.size();
    searchSample.error = searchError;
    searchSample.elapsedUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - searchStarted).count());
    AddCacheStats(source, searchSample);
    report.samples.push_back(std::move(searchSample));

    WavePatternSearchOptions cancelOptions;
    cancelOptions.maxCandidates = options.searchMaxCandidates;
    const std::size_t cancelAfter = std::max<std::size_t>(1, options.cancelAfterChecks);
    std::size_t checks = 0;
    cancelOptions.shouldCancel = [&checks, cancelAfter] { return ++checks >= cancelAfter; };
    const auto cancelStarted = std::chrono::steady_clock::now();
    std::string cancelError;
    const auto cancelledMatches = WavePatternCollect(source, pattern, begin, firstEnd,
                                                     cancelOptions, cancelError);
    WavePerformanceSample cancelSample;
    cancelSample.stage = WavePerformanceStage::CancelledSearch;
    cancelSample.cancelled = cancelError == "pattern search cancelled";
    cancelSample.success = cancelSample.cancelled;
    cancelSample.transitions = cancelledMatches.size();
    cancelSample.error = cancelError;
    cancelSample.elapsedUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - cancelStarted).count());
    AddCacheStats(source, cancelSample);
    report.samples.push_back(std::move(cancelSample));

    return true;
}

std::string GenerateWavePerformanceReportJson(const WavePerformanceReport& report)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["source_path"] = report.sourcePath;
    root["source_bytes"] = Json::UInt64(report.sourceBytes);
    root["signal_id"] = report.signalId;
    root["time_range_begin"] = Json::UInt64(report.timeRange.begin);
    root["time_range_end"] = Json::UInt64(report.timeRange.end);
    root["time_range_valid"] = report.timeRange.valid;
    root["conditions"] = Json::Value(Json::objectValue);
    root["conditions"]["first_screen_span"] = Json::UInt64(report.conditions.firstScreenSpan);
    root["conditions"]["zoom_span"] = Json::UInt64(report.conditions.zoomSpan);
    root["conditions"]["search_max_candidates"] = Json::UInt64(report.conditions.searchMaxCandidates);
    root["conditions"]["cancel_after_checks"] = Json::UInt64(report.conditions.cancelAfterChecks);
    root["samples"] = Json::Value(Json::arrayValue);
    for (const auto& sample : report.samples) root["samples"].append(SampleJson(sample));

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

} // namespace sigflow::wave
