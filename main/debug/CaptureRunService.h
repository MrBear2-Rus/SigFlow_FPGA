#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "TransportMetrics.h"
#include "DebugProtocol.h"

namespace sigflow {
namespace debug {

// TB-32 共享的稳定诊断严重性。诊断码本身由调用方保存，避免 UI 文案成为接口。
enum class DiagnosticSeverity {
    Info,
    Warning,
    Error,
    Blocking,
};

enum class CaptureRunState {
    Created,
    Validating,
    Running,
    Paused,
    Stopping,
    Completed,
    Cancelled,
    Failed,
};

enum class CaptureAttemptState {
    Planned,
    Running,
    Captured,
    TimedOut,
    Failed,
    Cancelled,
};

struct DiagnosticIssue {
    std::string code;       // 稳定机器码，例如 CAPTURE_TIMEOUT / DEVICE_BUSY
    std::string stage;      // validating / acquiring / persisting / analysing
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string observed;   // 已观察事实，不写推测结论
    std::string action;     // 建议的下一步动作
    std::vector<std::string> evidence;
};

struct CaptureRunPlan {
    std::string runId;
    std::string outputRoot;
    std::string deviceId;
    std::string contractSnapshot;
    std::string runtimeConfigHash;

    // 首版采用“取得 targetCount 个完整窗口”的语义。
    std::size_t targetCount = 1;
    std::size_t maxAttempts = 0;       // 0 = 由服务按 targetCount 推导有界上限
    std::size_t stopAfterFailures = 3; // 连续失败上限；0 = 不使用该条件
    std::uint32_t totalTimeoutMs = 60000;
    std::uint32_t attemptTimeoutMs = 5000;
    std::uint32_t interAttemptDelayMs = 0;
};

struct CaptureRunWindow {
    std::string captureId;
    std::string sessionId;
    std::string rawPath;
    std::string vcdPath;
    std::string completeness = "unknown"; // complete / partial / corrupt / unknown
    std::string identity = "unknown";      // device_verified / host_bound / unknown
    // SHA-256 of canonical sample words.  It compares data within the same
    // run/configuration without treating equal waveforms as the same fault.
    std::string dataHash;
    std::string duplicateOf;               // prior attempt ID in this run
    std::string classification = "unknown"; // triggered / complete_untriggered / duplicate / ...
    std::uint64_t sampleCount = 0;
    std::uint64_t triggerIndex = 0;
    std::string metadataPath;
    std::string timeDomain = "unknown";
    std::string vcdTimescale;
    std::uint64_t sampleClockHz = 0;
    std::uint32_t decimation = 0;
    std::uint64_t triggerTime = 0;
    bool triggerObserved = false;
};

// TB-06-05：阶段时间只描述宿主观测，不把串口往返时间伪装成 FPGA 绝对时间。
// gap_before_ms 是上一完整窗口产物提交到本次 attempt 开始之间的宿主间隔。
struct CaptureAttemptTiming {
    std::string armRequestedAtUtc;
    std::string waitStartedAtUtc;
    std::string captureReadyAtUtc;
    std::string readbackStartedAtUtc;
    std::string readbackCompletedAtUtc;
    std::string artifactsCompletedAtUtc;
    std::uint64_t waitMs = 0;
    std::uint64_t readbackMs = 0;
    std::uint64_t artifactMs = 0;
    std::uint64_t gapBeforeMs = 0;
    std::string gapBeforeBasis = "unknown"; // host_observed / estimated / unknown
    std::string continuity = "unknown";     // continuous / disconnected / unknown
};

struct CaptureAttemptResult {
    std::string attemptId;
    std::string startedAtUtc;
    std::string completedAtUtc;
    std::uint64_t elapsedMs = 0;
    CaptureAttemptState state = CaptureAttemptState::Failed;
    CaptureRunWindow window;
    std::string error;
    CaptureAttemptTiming timing;
    TransportStats transportStats;
    ProtocolStats protocolStats;
    // Failure retries require the executor to prove that another ARM is safe.
    bool retryAllowed = false;
    std::vector<DiagnosticIssue> issues;
};

struct CaptureAttemptContext {
    const CaptureRunPlan& plan;
    std::string runId;
    std::string attemptId;
    std::size_t attemptNumber = 0;
    std::uint32_t remainingTimeoutMs = 0;
    const std::atomic<bool>& cancelled;
    std::chrono::steady_clock::time_point deadline;

    std::uint32_t RemainingBudgetMs() const {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        return remaining > 0 ? static_cast<std::uint32_t>(remaining) : 0;
    }
};

struct CaptureRunResult {
    std::string runId;
    std::string createdAtUtc;
    CaptureRunState state = CaptureRunState::Created;
    std::size_t attempts = 0;
    std::size_t completedWindows = 0;
    std::size_t timeoutCount = 0;
    std::size_t failureCount = 0;
    std::vector<CaptureAttemptResult> attemptResults;
    std::vector<DiagnosticIssue> issues;
};

using CaptureAttemptCallback =
    std::function<CaptureAttemptResult(const CaptureAttemptContext&)>;

const char* ToString(CaptureRunState state);
const char* ToString(CaptureAttemptState state);
const char* ToString(DiagnosticSeverity severity);
std::string CaptureNowUtc();

// 板外重复采集调度器（TB-06）。它不拥有 ITransport，也不创建 RTL；调用方把
// 单次 DebugAcquisition 包装成 callback 后，可在已有 GUI worker 或 CLI 线程运行。
class CaptureRunService {
public:
    CaptureRunService() = default;

    CaptureRunService(const CaptureRunService&) = delete;
    CaptureRunService& operator=(const CaptureRunService&) = delete;

    // 同一实例一次只允许一个 Run。Run 返回后可复用实例；Cancel 可由另一个线程调用。
    bool Run(const CaptureRunPlan& plan, const CaptureAttemptCallback& attempt,
             CaptureRunResult& result, std::string& error);

    void Cancel() { cancelled_.store(true); pauseCv_.notify_all(); }
    // A pause is observed only between attempts.  The active callback is
    // never interrupted, so no new ARM is issued while a run is paused.
    void Pause() { pauseRequested_.store(true); }
    void Resume() { pauseRequested_.store(false); pauseCv_.notify_all(); }
    bool IsCancelled() const { return cancelled_.load(); }
    bool IsPaused() const { return pauseRequested_.load(); }

private:
    static bool ValidatePlan(const CaptureRunPlan& plan, std::string& error);
    static std::string EnsureRunId(const std::string& requested);
    static std::string MakeAttemptId(const std::string& runId, std::size_t number);
    static bool IsComplete(const CaptureAttemptResult& attempt);
    static bool IsTimeout(const CaptureAttemptResult& attempt);
    static void AddIssue(CaptureRunResult& result, const std::string& code,
                         const std::string& stage, DiagnosticSeverity severity,
                         const std::string& observed, const std::string& action);
    static bool PersistStart(const CaptureRunPlan& plan, const CaptureRunResult& result,
                             std::string& error);
    static bool PersistUpdate(const CaptureRunPlan& plan, const CaptureRunResult& result,
                              std::string& error);
    static bool AppendAttempt(const CaptureRunPlan& plan, std::size_t attemptNumber,
                              const CaptureAttemptResult& attempt, std::string& error);
    bool WaitIfPaused(const CaptureRunPlan& plan, CaptureRunResult& result,
                      std::chrono::steady_clock::time_point& deadline,
                      std::string& error);

    std::atomic<bool> cancelled_{false};
    std::atomic<bool> pauseRequested_{false};
    mutable std::mutex pauseMutex_;
    std::condition_variable pauseCv_;
};

} // namespace debug
} // namespace sigflow
