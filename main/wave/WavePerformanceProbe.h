#pragma once

#include "WavePatternSearch.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sigflow::wave {

enum class WavePerformanceStage {
    FirstScreen,
    Zoom,
    PatternSearch,
    CancelledSearch,
};

struct WavePerformanceSample {
    WavePerformanceStage stage = WavePerformanceStage::FirstScreen;
    bool success = false;
    bool cancelled = false;
    std::uint64_t elapsedUs = 0;
    std::uint64_t transitions = 0;
    std::size_t cacheBytes = 0;
    std::size_t cacheEntries = 0;
    std::uint64_t queryHits = 0;
    std::uint64_t queryMisses = 0;
    std::uint64_t valueHits = 0;
    std::uint64_t valueMisses = 0;
    std::string error;
};

struct WavePerformanceProbeOptions {
    int signalId = -1; // -1 selects the first source signal
    sigflow::trace::TimeValue firstScreenSpan = 0; // 0 = up to 10,000 ticks
    sigflow::trace::TimeValue zoomSpan = 0;        // 0 = first screen / 8
    std::size_t searchMaxCandidates = 250000;
    std::size_t cancelAfterChecks = 1;
};

struct WavePerformanceReport {
    std::string sourcePath;
    std::uint64_t sourceBytes = 0;
    sigflow::trace::TraceTimeRange timeRange;
    WavePerformanceProbeOptions conditions;
    int signalId = -1;
    std::vector<WavePerformanceSample> samples;
};

bool RunWavePerformanceProbe(sigflow::trace::TraceSource& source,
                             const WavePerformanceProbeOptions& options,
                             WavePerformanceReport& report,
                             std::string& error);

std::string GenerateWavePerformanceReportJson(const WavePerformanceReport& report);

} // namespace sigflow::wave
