#pragma once

#include "MonitorService.h"
#include "MonitorBackpressure.h"

#include <cstdint>
#include <functional>
#include <string>

namespace sigflow::debug {

enum class MonitorScheduleMode {
    Idle,
    Waiting,
    Backoff,
    Paused,
    Stopped,
    Failed,
};

enum class MonitorCycleStatus {
    Completed,
    DeviceUnavailable,
    IdentityMismatch,
    Cancelled,
    Failed,
};

const char* ToString(MonitorScheduleMode mode);
const char* ToString(MonitorCycleStatus status);

struct MonitorCycleResult {
    MonitorCycleStatus status = MonitorCycleStatus::Failed;
    std::string captureId;
    std::string observedIdentity;
    std::string message;
    bool retryable = true;
};

struct MonitorSchedulerOptions {
    std::uint64_t maxBackoffMs = 60000;
    std::uint32_t maxConsecutiveFailures = 3;
    MonitorBackpressureProbe backpressureProbe;
};

struct MonitorSchedulerSnapshot {
    MonitorScheduleMode mode = MonitorScheduleMode::Idle;
    MonitorState persistedState = MonitorState::Created;
    std::uint64_t completedCycles = 0;
    std::uint32_t consecutiveFailures = 0;
    std::uint64_t nextDueMs = 0;
    std::uint64_t backoffMs = 0;
    std::string expectedIdentity;
    std::string observedIdentity;
};

using MonitorCaptureCallback = std::function<MonitorCycleResult()>;
using MonitorIdentityProbe =
    std::function<bool(const std::string& expectedIdentity,
                      std::string& observedIdentity, std::string& error)>;

// 单步调度器：调用方负责线程和时钟，服务负责持久化事件、生命周期、退避
// 与身份门禁。它不拥有传输，也不会隐式打开、重试 ARM 或抢占设备。
class MonitorScheduler {
public:
    bool Start(const MonitorPlan& plan, const MonitorSchedulerOptions& options,
               const std::string& expectedIdentity, std::string& error);

    // nowMs 使用调用方单调时钟。未到 nextDueMs 时返回 true 且不调用 callback。
    bool Tick(std::uint64_t nowMs, const MonitorCaptureCallback& callback,
              std::string& error);

    bool Pause(std::string& error);
    bool Resume(const MonitorIdentityProbe& identityProbe, std::string& error);
    bool Stop(std::string& error);

    MonitorSchedulerSnapshot Snapshot() const { return snapshot_; }

private:
    bool AppendSchedulerEvent(const std::string& type, const std::string& message,
                              const std::string& captureId, std::uint64_t nowMs,
                              std::string& error);
    bool PersistState(MonitorState state, std::string& error);
    static std::uint64_t NextBackoff(std::uint64_t intervalMs,
                                     std::uint32_t failureCount,
                                     std::uint64_t maxBackoffMs);

    MonitorPlan plan_;
    MonitorSchedulerOptions options_;
    MonitorSchedulerSnapshot snapshot_;
    bool started_ = false;
};

} // namespace sigflow::debug
