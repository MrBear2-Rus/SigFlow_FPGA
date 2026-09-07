#pragma once

#include "DebugContract.h"
#include "WaveformComparator.h"

#include <string>
#include <vector>

extern "C" {
#include "vcd.h"
}

namespace sigflow {
namespace debug {

struct DebugBehaviorEvent {
    std::string kind;
    timestamp_t time = 0;
    std::string signalName;
    std::string value;
    std::string message;
};

struct DebugBehaviorSummary {
    std::string schemaVersion = "1.0";
    bool deterministic = true;
    std::vector<DebugBehaviorEvent> events;
};

class DebugBehaviorSummaryBuilder {
public:
    static DebugBehaviorSummary Extract(vcd_t* capture,
                                        const DebugContract& contract,
                                        const ComparisonResult& comparison);
    static std::string GenerateJson(const DebugBehaviorSummary& summary);
    static std::string GenerateMarkdown(const DebugBehaviorSummary& summary);
    static bool Save(const DebugBehaviorSummary& summary,
                     const std::string& jsonPath,
                     const std::string& markdownPath,
                     std::string& error);
};

} // namespace debug
} // namespace sigflow
