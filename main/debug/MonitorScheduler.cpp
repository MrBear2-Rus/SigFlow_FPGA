#include "MonitorScheduler.h"

#include <algorithm>
#include <limits>

namespace sigflow::debug {

const char* ToString(MonitorScheduleMode mode)
{
    switch (mode) {
        case MonitorScheduleMode::Idle: return "Idle";
        case MonitorScheduleMode::Waiting: return "Waiting";
        case MonitorScheduleMode::Backoff: return "Backoff";
        case MonitorScheduleMode::Paused: return "Paused";
        case MonitorScheduleMode::Stopped: return "Stopped";
        case MonitorScheduleMode::Failed: return "Failed";
    }
    return "Idle";
}

const char* ToString(MonitorCycleStatus status)
{
    switch (status) {
        case MonitorCycleStatus::Completed: return "Completed";
        case MonitorCycleStatus::DeviceUnavailable: return "DeviceUnavailable";
        case MonitorCycleStatus::IdentityMismatch: return "IdentityMismatch";
        case MonitorCycleStatus::Cancelled: return "Cancelled";
        case MonitorCycleStatus::Failed: return "Failed";
    }
    return "Failed";
}

bool MonitorScheduler::Start(const MonitorPlan& plan,
                             const MonitorSchedulerOptions& options,
                             const std::string& expectedIdentity,
                             std::string& error)
{
    error.clear();
    if (options.maxBackoffMs == 0) {
        error = "monitor scheduler maxBackoffMs must be greater than zero";
        return false;
    }
    if (options.maxConsecutiveFailures == 0) {
        error = "monitor scheduler maxConsecutiveFailures must be greater than zero";
        return false;
    }
    MonitorCheckpoint checkpoint;
    if (!MonitorService::LoadCheckpoint(plan, checkpoint, error)) return false;
    if (checkpoint.state == MonitorState::Stopped || checkpoint.state == MonitorState::Failed) {
        error = "monitor scheduler cannot resume a terminal monitor; create a new plan";
        return false;
    }

    plan_ = plan;
    options_ = options;
    snapshot_ = MonitorSchedulerSnapshot();
    snapshot_.persistedState = checkpoint.state;
    snapshot_.expectedIdentity = expectedIdentity;
    if (checkpoint.state == MonitorState::Created) {
        if (!MonitorService::TransitionState(plan_, MonitorState::Running, error)) return false;
        snapshot_.persistedState = MonitorState::Running;
    }
    snapshot_.mode = snapshot_.persistedState == MonitorState::Paused
                         ? MonitorScheduleMode::Paused
                         : MonitorScheduleMode::Waiting;
    snapshot_.nextDueMs = 0;
    started_ = true;
    return true;
}

bool MonitorScheduler::AppendSchedulerEvent(const std::string& type,
                                            const std::string& message,
                                            const std::string& captureId,
                                            std::uint64_t nowMs,
                                            std::string& error)
{
    MonitorEvent event;
    return MonitorService::AppendEvent(plan_, type, message, captureId, nowMs,
                                       event, error);
}

bool MonitorScheduler::PersistState(MonitorState state, std::string& error)
{
    if (!MonitorService::TransitionState(plan_, state, error)) return false;
    snapshot_.persistedState = state;
    return true;
}

std::uint64_t MonitorScheduler::NextBackoff(std::uint64_t intervalMs,
                                            std::uint32_t failureCount,
                                            std::uint64_t maxBackoffMs)
{
    std::uint64_t result = std::max<std::uint64_t>(1, intervalMs);
    for (std::uint32_t i = 1; i < failureCount; ++i) {
        if (result >= maxBackoffMs || result > std::numeric_limits<std::uint64_t>::max() / 2)
            return maxBackoffMs;
        result *= 2;
    }
    return std::min(result, maxBackoffMs);
}

bool MonitorScheduler::Tick(std::uint64_t nowMs, const MonitorCaptureCallback& callback,
                            std::string& error)
{
    error.clear();
    if (!started_) {
        error = "monitor scheduler is not started";
        return false;
    }
    if (!callback) {
        error = "monitor scheduler capture callback is missing";
        return false;
    }
    if (snapshot_.mode == MonitorScheduleMode::Paused ||
        snapshot_.mode == MonitorScheduleMode::Stopped ||
        snapshot_.mode == MonitorScheduleMode::Failed) {
        return true;
    }
    if (nowMs < snapshot_.nextDueMs) return true;

    if (options_.backpressureProbe) {
        const MonitorBackpressureResult storage = options_.backpressureProbe();
        if (storage.state == MonitorBackpressureState::Blocked) {
            const std::string reason = storage.reason.empty()
                                           ? "monitor storage backpressure blocked capture"
                                           : storage.reason;
            if (!AppendSchedulerEvent("monitor.backpressure",
                                      storage.code + ": " + reason, "", nowMs, error))
                return false;
            if (!PersistState(MonitorState::Paused, error)) return false;
            snapshot_.mode = MonitorScheduleMode::Paused;
            return true;
        }
    }

    MonitorCycleResult cycle;
    try {
        cycle = callback();
    } catch (...) {
        cycle.status = MonitorCycleStatus::Failed;
        cycle.message = "monitor capture callback threw an exception";
        cycle.retryable = false;
    }
    const std::string message = cycle.message.empty() ? ToString(cycle.status) : cycle.message;

    switch (cycle.status) {
        case MonitorCycleStatus::Completed:
            if (cycle.captureId.empty()) {
                error = "completed monitor cycle has no capture id";
                return false;
            }
            if (!AppendSchedulerEvent("capture.completed", message, cycle.captureId,
                                      nowMs, error)) return false;
            snapshot_.completedCycles++;
            snapshot_.consecutiveFailures = 0;
            snapshot_.backoffMs = 0;
            snapshot_.mode = MonitorScheduleMode::Waiting;
            snapshot_.nextDueMs = nowMs > std::numeric_limits<std::uint64_t>::max() - plan_.intervalMs
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : nowMs + plan_.intervalMs;
            return true;

        case MonitorCycleStatus::DeviceUnavailable:
        case MonitorCycleStatus::IdentityMismatch:
            snapshot_.observedIdentity = cycle.observedIdentity;
            if (!AppendSchedulerEvent(cycle.status == MonitorCycleStatus::DeviceUnavailable
                                           ? "device.unavailable" : "identity.changed",
                                       message, cycle.captureId, nowMs, error)) return false;
            if (!PersistState(MonitorState::Paused, error)) return false;
            snapshot_.mode = MonitorScheduleMode::Paused;
            return true;

        case MonitorCycleStatus::Cancelled:
            if (!AppendSchedulerEvent("monitor.cancelled", message, cycle.captureId,
                                      nowMs, error)) return false;
            if (!PersistState(MonitorState::Stopped, error)) return false;
            snapshot_.mode = MonitorScheduleMode::Stopped;
            return true;

        case MonitorCycleStatus::Failed:
            snapshot_.consecutiveFailures++;
            if (!AppendSchedulerEvent("capture.failed", message, cycle.captureId,
                                      nowMs, error)) return false;
            if (!cycle.retryable ||
                snapshot_.consecutiveFailures >= options_.maxConsecutiveFailures) {
                if (!PersistState(MonitorState::Failed, error)) return false;
                snapshot_.mode = MonitorScheduleMode::Failed;
                return true;
            }
            snapshot_.backoffMs = NextBackoff(plan_.intervalMs,
                                              snapshot_.consecutiveFailures,
                                              options_.maxBackoffMs);
            snapshot_.nextDueMs = nowMs > std::numeric_limits<std::uint64_t>::max() - snapshot_.backoffMs
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : nowMs + snapshot_.backoffMs;
            snapshot_.mode = MonitorScheduleMode::Backoff;
            return AppendSchedulerEvent("monitor.retry_scheduled",
                                        "retry in " + std::to_string(snapshot_.backoffMs) + " ms",
                                        cycle.captureId, nowMs, error);
    }
    error = "unknown monitor cycle status";
    return false;
}

bool MonitorScheduler::Pause(std::string& error)
{
    error.clear();
    if (!started_ || snapshot_.mode == MonitorScheduleMode::Stopped ||
        snapshot_.mode == MonitorScheduleMode::Failed) {
        error = "monitor scheduler cannot be paused in its current state";
        return false;
    }
    if (snapshot_.mode == MonitorScheduleMode::Paused) return true;
    if (!PersistState(MonitorState::Paused, error)) return false;
    snapshot_.mode = MonitorScheduleMode::Paused;
    return true;
}

bool MonitorScheduler::Resume(const MonitorIdentityProbe& identityProbe, std::string& error)
{
    error.clear();
    if (!started_ || snapshot_.mode != MonitorScheduleMode::Paused) {
        error = "monitor scheduler is not paused";
        return false;
    }
    if (!identityProbe) {
        error = "identity probe is required before resuming a monitor";
        return false;
    }
    std::string observed;
    if (!identityProbe(snapshot_.expectedIdentity, observed, error)) {
        snapshot_.observedIdentity = observed;
        return false;
    }
    snapshot_.observedIdentity = observed;
    if (!snapshot_.expectedIdentity.empty() && observed != snapshot_.expectedIdentity) {
        error = "monitor identity changed; remain paused";
        return false;
    }
    if (!PersistState(MonitorState::Running, error)) return false;
    snapshot_.mode = MonitorScheduleMode::Waiting;
    snapshot_.nextDueMs = 0;
    snapshot_.consecutiveFailures = 0;
    snapshot_.backoffMs = 0;
    return true;
}

bool MonitorScheduler::Stop(std::string& error)
{
    error.clear();
    if (!started_) {
        error = "monitor scheduler is not started";
        return false;
    }
    if (snapshot_.mode == MonitorScheduleMode::Stopped) return true;
    if (snapshot_.mode == MonitorScheduleMode::Failed) return true;
    if (!PersistState(MonitorState::Stopped, error)) return false;
    snapshot_.mode = MonitorScheduleMode::Stopped;
    return true;
}

} // namespace sigflow::debug
