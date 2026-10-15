#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sigflow::debug {

enum class MonitorState {
    Created,
    Running,
    Paused,
    Stopped,
    Failed,
};

const char* ToString(MonitorState state);

struct MonitorPlan {
    std::string monitorId;
    std::string outputRoot;
    std::string captureRunId;
    std::uint64_t intervalMs = 1000;
    std::uint64_t maxRuntimeMs = 0;
    std::uint64_t maxDiskBytes = 0;
};

struct MonitorCheckpoint {
    std::string monitorId;
    MonitorState state = MonitorState::Created;
    std::uint64_t nextSequence = 1;
    std::string lastEventId;
    std::string lastCaptureId;
    std::string updatedAtUtc;
};

struct MonitorEvent {
    std::uint64_t sequence = 0;
    std::string eventId;
    std::string type;
    std::string observedAtUtc;
    std::uint64_t monotonicMs = 0;
    std::string captureId;
    std::string message;
};

// Retention is deliberately host-side. It only considers complete terminal
// monitor directories and never guesses that an active or malformed directory
// is safe to delete.
struct MonitorRetentionPolicy {
    std::uint64_t maxDiskBytes = 0; // 0 = unlimited
    std::uint64_t maxMonitorCount = 0; // 0 = unlimited
};

struct MonitorRetentionAction {
    std::string monitorId;
    std::uint64_t bytes = 0;
    std::string reason;
};

class MonitorService {
public:
    static bool Create(const MonitorPlan& plan, std::string& error);

    static bool AppendEvent(const MonitorPlan& plan,
                            const std::string& type,
                            const std::string& message,
                            const std::string& captureId,
                            std::uint64_t monotonicMs,
                            MonitorEvent& out,
                            std::string& error);

    static bool SaveCheckpoint(const MonitorPlan& plan,
                               const MonitorCheckpoint& checkpoint,
                               std::string& error);

    // Explicit lifecycle transition used by a manual start/pause/stop UI.
    // No transition is implicit after restart and terminal states cannot be
    // resumed without creating a new monitor plan.
    static bool TransitionState(const MonitorPlan& plan,
                                MonitorState nextState,
                                std::string& error);

    static bool LoadCheckpoint(const MonitorPlan& plan,
                               MonitorCheckpoint& checkpoint,
                               std::string& error);

    static bool ValidateEventLog(const MonitorPlan& plan,
                                 std::uint64_t* nextSequence,
                                 std::string& error);

    // Build a deterministic deletion plan under outputRoot. IDs in
    // protectedMonitorIds (and all active/malformed directories) are retained.
    static bool BuildRetentionPlan(const std::string& outputRoot,
                                   const MonitorRetentionPolicy& policy,
                                   const std::vector<std::string>& protectedMonitorIds,
                                   std::vector<MonitorRetentionAction>& actions,
                                   std::uint64_t& totalBytes,
                                   std::string& error);

    // Apply only actions produced by BuildRetentionPlan. The implementation
    // rechecks each checkpoint and path containment before removing anything.
    static bool ApplyRetentionPlan(const std::string& outputRoot,
                                   const std::vector<MonitorRetentionAction>& actions,
                                   std::string& error);

private:
    static bool ValidatePlan(const MonitorPlan& plan, std::string& error);
};

} // namespace sigflow::debug
