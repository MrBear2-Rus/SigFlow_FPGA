#pragma once

#include "MonitorBackpressure.h"
#include "MonitorService.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sigflow::debug {

struct MonitorIncidentSummary {
    std::string snapshotId;
    std::string incidentId;
    std::string triggerType;
    std::string message;
    std::string path;
};

// Read-only view of one persisted monitor directory.  It deliberately keeps
// the monitor services' lifecycle and retention APIs separate: loading a
// summary never starts, pauses, resumes, or deletes a monitor.
struct MonitorSummary {
    MonitorPlan plan;
    MonitorCheckpoint checkpoint;
    std::vector<MonitorEvent> recentEvents;
    std::vector<MonitorIncidentSummary> incidents;
    MonitorStorageSample storage;
    bool storageSampled = false;
    bool eventLogValid = false;
    std::uint64_t eventCount = 0;
    std::uint64_t eventSpanMs = 0;
    std::string coverageStartUtc;
    std::string coverageEndUtc;
    std::uint64_t totalBytes = 0;
    std::vector<std::string> diagnostics;
};

class MonitorSummaryService {
public:
    static bool Load(const std::string& manifestPath,
                     MonitorSummary& summary,
                     std::string& error,
                     std::size_t maxEvents = 40);
};

} // namespace sigflow::debug
