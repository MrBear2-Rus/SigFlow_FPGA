#include "CaptureRunService.h"
#include "DebugArtifactIO.h"

#include <json/json.h>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sigflow {
namespace debug {

namespace {

std::string NowUtc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string JsonString(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, value) + "\n";
}

std::string JsonLine(const Json::Value& value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value) + "\n";
}

Json::Value IssueJson(const DiagnosticIssue& issue)
{
    Json::Value value(Json::objectValue);
    value["code"] = issue.code;
    value["stage"] = issue.stage;
    value["severity"] = ToString(issue.severity);
    value["observed"] = issue.observed;
    value["action"] = issue.action;
    Json::Value evidence(Json::arrayValue);
    for (const std::string& item : issue.evidence) evidence.append(item);
    value["evidence"] = evidence;
    return value;
}

Json::Value DiagnosticEntryJson(const DiagnosticIssue& issue,
                                const std::string& attemptId = std::string())
{
    Json::Value value = IssueJson(issue);
    if (!attemptId.empty()) value["attempt_id"] = attemptId;
    return value;
}

Json::Value PlanJson(const CaptureRunPlan& plan)
{
    Json::Value value(Json::objectValue);
    value["run_id"] = plan.runId;
    value["output_root"] = plan.outputRoot;
    value["device_id"] = plan.deviceId;
    value["contract_snapshot"] = plan.contractSnapshot;
    value["runtime_config_hash"] = plan.runtimeConfigHash;
    value["target_count"] = static_cast<Json::UInt64>(plan.targetCount);
    value["max_attempts"] = static_cast<Json::UInt64>(plan.maxAttempts);
    value["stop_after_failures"] = static_cast<Json::UInt64>(plan.stopAfterFailures);
    value["total_timeout_ms"] = plan.totalTimeoutMs;
    value["attempt_timeout_ms"] = plan.attemptTimeoutMs;
    value["inter_attempt_delay_ms"] = plan.interAttemptDelayMs;
    return value;
}

Json::Value WindowJson(const CaptureRunWindow& window)
{
    Json::Value value(Json::objectValue);
    value["capture_id"] = window.captureId;
    value["session_id"] = window.sessionId;
    value["raw_path"] = window.rawPath;
    value["vcd_path"] = window.vcdPath;
    value["completeness"] = window.completeness;
    value["identity"] = window.identity;
    value["sample_count"] = static_cast<Json::UInt64>(window.sampleCount);
    value["trigger_index"] = static_cast<Json::UInt64>(window.triggerIndex);
    value["metadata_path"] = window.metadataPath;
    if (!window.metadataPath.empty()) {
        Json::Value timebase(Json::objectValue);
        timebase["time_domain"] = window.timeDomain;
        timebase["vcd_timescale"] = window.vcdTimescale;
        timebase["sample_clock_hz"] = Json::UInt64(window.sampleClockHz);
        timebase["decimation"] = window.decimation;
        timebase["trigger_observed"] = window.triggerObserved;
        timebase["trigger_time"] = window.triggerObserved ? Json::Value(Json::UInt64(window.triggerTime)) : Json::Value();
        value["timebase"] = timebase;
    }
    return value;
}

Json::Value AttemptJson(std::size_t number, const CaptureAttemptResult& attempt)
{
    Json::Value value(Json::objectValue);
    value["attempt_number"] = static_cast<Json::UInt64>(number);
    value["attempt_id"] = attempt.attemptId;
    value["started_at"] = attempt.startedAtUtc;
    value["completed_at"] = attempt.completedAtUtc;
    value["elapsed_ms"] = static_cast<Json::UInt64>(attempt.elapsedMs);
    value["state"] = ToString(attempt.state);
    value["error"] = attempt.error;
    value["retry_allowed"] = attempt.retryAllowed;
    Json::Value timing(Json::objectValue);
    timing["arm_requested_at"] = attempt.timing.armRequestedAtUtc;
    timing["wait_started_at"] = attempt.timing.waitStartedAtUtc;
    timing["capture_ready_at"] = attempt.timing.captureReadyAtUtc;
    timing["readback_started_at"] = attempt.timing.readbackStartedAtUtc;
    timing["readback_completed_at"] = attempt.timing.readbackCompletedAtUtc;
    timing["artifacts_completed_at"] = attempt.timing.artifactsCompletedAtUtc;
    timing["wait_ms"] = Json::UInt64(attempt.timing.waitMs);
    timing["readback_ms"] = Json::UInt64(attempt.timing.readbackMs);
    timing["artifact_ms"] = Json::UInt64(attempt.timing.artifactMs);
    timing["gap_before_ms"] = Json::UInt64(attempt.timing.gapBeforeMs);
    timing["gap_before_basis"] = attempt.timing.gapBeforeBasis;
    timing["continuity"] = attempt.timing.continuity;
    value["timing"] = timing;
    Json::Value transport(Json::objectValue);
    transport["read_calls"] = Json::UInt64(attempt.transportStats.readCalls);
    transport["read_bytes"] = Json::UInt64(attempt.transportStats.readBytes);
    transport["read_elapsed_us"] = Json::UInt64(attempt.transportStats.readElapsedUs);
    transport["read_timeouts"] = Json::UInt64(attempt.transportStats.readTimeouts);
    transport["read_disconnects"] = Json::UInt64(attempt.transportStats.readDisconnects);
    transport["write_calls"] = Json::UInt64(attempt.transportStats.writeCalls);
    transport["write_bytes"] = Json::UInt64(attempt.transportStats.writeBytes);
    transport["write_elapsed_us"] = Json::UInt64(attempt.transportStats.writeElapsedUs);
    transport["write_timeouts"] = Json::UInt64(attempt.transportStats.writeTimeouts);
    transport["write_disconnects"] = Json::UInt64(attempt.transportStats.writeDisconnects);
    transport["discard_calls"] = Json::UInt64(attempt.transportStats.discardCalls);
    value["transport_stats"] = transport;
    Json::Value protocol(Json::objectValue);
    protocol["command_calls"] = Json::UInt64(attempt.protocolStats.commandCalls);
    protocol["attempts"] = Json::UInt64(attempt.protocolStats.attempts);
    protocol["retries"] = Json::UInt64(attempt.protocolStats.retries);
    protocol["response_timeouts"] = Json::UInt64(attempt.protocolStats.responseTimeouts);
    protocol["write_failures"] = Json::UInt64(attempt.protocolStats.writeFailures);
    protocol["malformed_frames"] = Json::UInt64(attempt.protocolStats.malformedFrames);
    protocol["crc_errors"] = Json::UInt64(attempt.protocolStats.crcErrors);
    protocol["sequence_mismatches"] = Json::UInt64(attempt.protocolStats.sequenceMismatches);
    protocol["invalid_responses"] = Json::UInt64(attempt.protocolStats.invalidResponses);
    value["protocol_stats"] = protocol;
    value["window"] = WindowJson(attempt.window);
    Json::Value issues(Json::arrayValue);
    for (const DiagnosticIssue& issue : attempt.issues) issues.append(IssueJson(issue));
    value["issues"] = issues;
    return value;
}

Json::Value ResultJson(const CaptureRunResult& result)
{
    Json::Value value(Json::objectValue);
    value["schema_version"] = "1.0";
    value["run_id"] = result.runId;
    value["state"] = ToString(result.state);
    value["attempts"] = static_cast<Json::UInt64>(result.attempts);
    value["completed_windows"] = static_cast<Json::UInt64>(result.completedWindows);
    value["timeout_count"] = static_cast<Json::UInt64>(result.timeoutCount);
    value["failure_count"] = static_cast<Json::UInt64>(result.failureCount);
    Json::Value issues(Json::arrayValue);
    for (const DiagnosticIssue& issue : result.issues) issues.append(IssueJson(issue));
    value["issues"] = issues;
    return value;
}

Json::Value DiagnosticsJson(const CaptureRunPlan& plan, const CaptureRunResult& result)
{
    Json::Value value(Json::objectValue);
    value["schema_version"] = "1.0";
    value["run_id"] = result.runId;
    value["state"] = ToString(result.state);
    value["generated_at"] = NowUtc();
    value["plan_id"] = plan.runId;
    Json::Value summary(Json::objectValue);
    summary["attempts"] = static_cast<Json::UInt64>(result.attempts);
    summary["completed_windows"] = static_cast<Json::UInt64>(result.completedWindows);
    summary["timeout_count"] = static_cast<Json::UInt64>(result.timeoutCount);
    summary["failure_count"] = static_cast<Json::UInt64>(result.failureCount);
    value["summary"] = summary;
    Json::Value runIssues(Json::arrayValue);
    for (const DiagnosticIssue& issue : result.issues) runIssues.append(IssueJson(issue));
    value["issues"] = runIssues;
    Json::Value attemptIssues(Json::arrayValue);
    for (std::size_t index = 0; index < result.attemptResults.size(); ++index) {
        const CaptureAttemptResult& attempt = result.attemptResults[index];
        for (const DiagnosticIssue& issue : attempt.issues) {
            Json::Value entry = DiagnosticEntryJson(issue, attempt.attemptId);
            entry["attempt_number"] = static_cast<Json::UInt64>(index + 1);
            attemptIssues.append(entry);
        }
    }
    value["attempt_issues"] = attemptIssues;
    return value;
}

bool WriteAtomic(const std::filesystem::path& path, const std::string& content,
                 std::string& error)
{
    return WriteTextFileAtomic(path, content, error);
}

} // namespace

const char* ToString(CaptureRunState state)
{
    switch (state) {
        case CaptureRunState::Created: return "Created";
        case CaptureRunState::Validating: return "Validating";
        case CaptureRunState::Running: return "Running";
        case CaptureRunState::Stopping: return "Stopping";
        case CaptureRunState::Completed: return "Completed";
        case CaptureRunState::Cancelled: return "Cancelled";
        case CaptureRunState::Failed: return "Failed";
    }
    return "Unknown";
}

const char* ToString(CaptureAttemptState state)
{
    switch (state) {
        case CaptureAttemptState::Planned: return "Planned";
        case CaptureAttemptState::Running: return "Running";
        case CaptureAttemptState::Captured: return "Captured";
        case CaptureAttemptState::TimedOut: return "TimedOut";
        case CaptureAttemptState::Failed: return "Failed";
        case CaptureAttemptState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

const char* ToString(DiagnosticSeverity severity)
{
    switch (severity) {
        case DiagnosticSeverity::Info: return "info";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Error: return "error";
        case DiagnosticSeverity::Blocking: return "blocking";
    }
    return "error";
}

std::string CaptureNowUtc()
{
    return NowUtc();
}

bool CaptureRunService::ValidatePlan(const CaptureRunPlan& plan, std::string& error)
{
    // Portable single directory component; also excludes Windows reserved names,
    // drive paths, trailing dots and case-insensitive device-name aliases.
    if (plan.runId.size() > 128 || plan.runId.rfind("run-", 0) != 0 ||
        plan.runId.size() <= 4 ||
        plan.runId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) {
        error = "capture run runId must start with run- and contain only letters, digits, - or _ (max 128)";
        return false;
    }
    if (plan.outputRoot.empty()) { error = "capture run outputRoot is empty"; return false; }
    if (plan.targetCount == 0) { error = "capture run targetCount must be greater than zero"; return false; }
    if (plan.attemptTimeoutMs == 0) { error = "capture run attemptTimeoutMs must be greater than zero"; return false; }
    if (plan.totalTimeoutMs == 0) { error = "capture run totalTimeoutMs must be greater than zero"; return false; }
    if (plan.maxAttempts != 0 && plan.maxAttempts < plan.targetCount) {
        error = "capture run maxAttempts must cover targetCount";
        return false;
    }
    if (plan.stopAfterFailures != 0 && plan.stopAfterFailures > plan.maxAttempts && plan.maxAttempts != 0) {
        error = "capture run stopAfterFailures exceeds maxAttempts";
        return false;
    }
    return true;
}

std::string CaptureRunService::EnsureRunId(const std::string& requested)
{
    if (!requested.empty()) return requested;
    static std::atomic<std::uint64_t> sequence{1};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "run-" + std::to_string(now) + "-" + std::to_string(sequence.fetch_add(1));
}

std::string CaptureRunService::MakeAttemptId(const std::string& runId, std::size_t number)
{
    return runId + "-attempt-" + std::to_string(number);
}

bool CaptureRunService::IsComplete(const CaptureAttemptResult& attempt)
{
    return attempt.state == CaptureAttemptState::Captured &&
           attempt.window.completeness == "complete" &&
           !attempt.window.captureId.empty();
}

bool CaptureRunService::IsTimeout(const CaptureAttemptResult& attempt)
{
    return attempt.state == CaptureAttemptState::TimedOut ||
           attempt.error.find("timeout") != std::string::npos ||
           attempt.error.find("Timeout") != std::string::npos ||
           attempt.error.find("超时") != std::string::npos;
}

void CaptureRunService::AddIssue(CaptureRunResult& result, const std::string& code,
                                 const std::string& stage, DiagnosticSeverity severity,
                                 const std::string& observed, const std::string& action)
{
    result.issues.push_back({code, stage, severity, observed, action, {}});
}

bool CaptureRunService::PersistStart(const CaptureRunPlan& plan,
                                     const CaptureRunResult& result, std::string& error)
{
    const std::filesystem::path root = std::filesystem::path(plan.outputRoot) / plan.runId;
    std::error_code ec;
    std::filesystem::create_directories(root.parent_path(), ec);
    if (ec || !std::filesystem::create_directory(root, ec)) {
        error = ec ? "unable to reserve run directory: " + ec.message()
                   : "capture run directory already exists: " + root.string();
        return false;
    }
    Json::Value manifest(Json::objectValue);
    manifest["schema_version"] = "1.0";
    manifest["created_at"] = result.createdAtUtc;
    manifest["plan"] = PlanJson(plan);
    manifest["result"] = ResultJson(result);
    if (!WriteAtomic(root / "run.json", JsonString(manifest), error)) return false;
    if (!WriteAtomic(root / "diagnostics.json", JsonString(DiagnosticsJson(plan, result)), error)) return false;
    std::ofstream windows(root / "windows.jsonl", std::ios::trunc);
    if (!windows) { error = "unable to create windows.jsonl"; return false; }
    return true;
}

bool CaptureRunService::PersistUpdate(const CaptureRunPlan& plan,
                                      const CaptureRunResult& result, std::string& error)
{
    const std::filesystem::path root = std::filesystem::path(plan.outputRoot) / plan.runId;
    Json::Value manifest(Json::objectValue);
    manifest["schema_version"] = "1.0";
    manifest["created_at"] = result.createdAtUtc;
    manifest["updated_at"] = NowUtc();
    manifest["plan"] = PlanJson(plan);
    manifest["result"] = ResultJson(result);
    if (!WriteAtomic(root / "run.json", JsonString(manifest), error)) return false;
    return WriteAtomic(root / "diagnostics.json", JsonString(DiagnosticsJson(plan, result)), error);
}

bool CaptureRunService::AppendAttempt(const CaptureRunPlan& plan, std::size_t attemptNumber,
                                      const CaptureAttemptResult& attempt, std::string& error)
{
    const std::filesystem::path root = std::filesystem::path(plan.outputRoot) / plan.runId;
    std::ofstream out(root / "windows.jsonl", std::ios::app);
    if (!out) { error = "unable to append windows.jsonl"; return false; }
    out << JsonLine(AttemptJson(attemptNumber, attempt));
    out.flush();
    if (!out) { error = "unable to append capture attempt"; return false; }
    return true;
}

bool CaptureRunService::Run(const CaptureRunPlan& inputPlan,
                            const CaptureAttemptCallback& attempt,
                            CaptureRunResult& result, std::string& error)
{
    error.clear();
    result = CaptureRunResult();
    CaptureRunPlan plan = inputPlan;
    plan.runId = EnsureRunId(plan.runId);
    result.runId = plan.runId;
    result.createdAtUtc = NowUtc();
    cancelled_.store(false);

    result.state = CaptureRunState::Validating;
    if (!ValidatePlan(plan, error)) {
        result.state = CaptureRunState::Failed;
        AddIssue(result, "CAPTURE_RUN_INVALID", "validating", DiagnosticSeverity::Blocking,
                 error, "修正采集次数、超时和输出目录后重试");
        return false;
    }
    if (!attempt) {
        error = "capture run attempt callback is empty";
        result.state = CaptureRunState::Failed;
        AddIssue(result, "CAPTURE_RUN_NO_EXECUTOR", "validating", DiagnosticSeverity::Blocking,
                 error, "连接单次 DebugAcquisition 执行器后重试");
        return false;
    }

    const std::size_t derivedMax = plan.targetCount >
        (std::numeric_limits<std::size_t>::max() / 3) ?
        std::numeric_limits<std::size_t>::max() : plan.targetCount * 3;
    if (plan.maxAttempts == 0) plan.maxAttempts = std::max(plan.targetCount, derivedMax);

    if (!PersistStart(plan, result, error)) {
        result.state = CaptureRunState::Failed;
        AddIssue(result, "RUN_PERSIST_FAILED", "persisting", DiagnosticSeverity::Blocking,
                 error, "检查采集目录权限和磁盘空间");
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(plan.totalTimeoutMs);
    result.state = CaptureRunState::Running;
    std::size_t consecutiveFailures = 0;
    bool cancelledByAttempt = false;
    bool blockedByAttempt = false;
    bool deadlineExceeded = false;
    bool havePreviousWindow = false;
    std::chrono::steady_clock::time_point previousWindowCompleted;
    while (result.completedWindows < plan.targetCount && result.attempts < plan.maxAttempts) {
        if (cancelled_.load()) break;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed >= plan.totalTimeoutMs) {
            deadlineExceeded = true;
            AddIssue(result, "CAPTURE_RUN_TIMEOUT", "acquiring", DiagnosticSeverity::Error,
                     "run 总时间超过 totalTimeoutMs", "降低窗口数量或增大总超时");
            break;
        }

        ++result.attempts;
        const std::size_t number = result.attempts;
        CaptureAttemptResult current;
        const auto attemptId = MakeAttemptId(plan.runId, number);
        const auto startedAtUtc = NowUtc();
        current.state = CaptureAttemptState::Running;
        const auto attemptStarted = std::chrono::steady_clock::now();
        const auto elapsedBeforeAttempt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        const auto remainingTimeout = elapsedBeforeAttempt >= plan.totalTimeoutMs
            ? 0u
            : static_cast<std::uint32_t>(plan.totalTimeoutMs - elapsedBeforeAttempt);
        const CaptureAttemptContext context{plan, plan.runId, MakeAttemptId(plan.runId, number),
                                            number, remainingTimeout, cancelled_, deadline};
        try {
            current = attempt(context);
        } catch (const std::exception& ex) {
            current.state = CaptureAttemptState::Failed;
            current.error = std::string("attempt callback exception: ") + ex.what();
        } catch (...) {
            current.state = CaptureAttemptState::Failed;
            current.error = "attempt callback unknown exception";
        }
        // These fields belong to the scheduler, never to the callback.
        current.attemptId = attemptId;
        current.startedAtUtc = startedAtUtc;
        current.completedAtUtc = NowUtc();
        current.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - attemptStarted).count());
        if (havePreviousWindow) {
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                attemptStarted - previousWindowCompleted).count();
            current.timing.gapBeforeMs = gap > 0 ? static_cast<std::uint64_t>(gap) : 0;
            current.timing.gapBeforeBasis = "host_observed";
            current.timing.continuity = "disconnected";
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            deadlineExceeded = true;
            AddIssue(result, "CAPTURE_RUN_TIMEOUT", "acquiring", DiagnosticSeverity::Error,
                     "attempt returned after the run deadline; committed evidence is retained",
                     "检查耗时阶段并调整总期限；阻塞 I/O 仍需遵守后端命令超时");
        }
        if (cancelled_.load() && current.state == CaptureAttemptState::Running) {
            current.state = CaptureAttemptState::Cancelled;
            current.error = "Cancelled";
        }

        if (IsComplete(current)) {
            ++result.completedWindows;
            consecutiveFailures = 0;
            previousWindowCompleted = std::chrono::steady_clock::now();
            havePreviousWindow = true;
        } else if (current.state == CaptureAttemptState::Cancelled || cancelled_.load()) {
            current.state = CaptureAttemptState::Cancelled;
        } else if (IsTimeout(current)) {
            ++result.timeoutCount;
            ++consecutiveFailures;
        } else {
            ++result.failureCount;
            ++consecutiveFailures;
        }
        if (!IsComplete(current) && !current.error.empty()) error = current.error;
        result.attemptResults.push_back(current);
        // Keep a run-level diagnostic index while retaining the complete
        // per-attempt issue list in windows.jsonl.
        result.issues.insert(result.issues.end(), current.issues.begin(), current.issues.end());
        blockedByAttempt = std::any_of(current.issues.begin(), current.issues.end(),
                                       [](const DiagnosticIssue& issue) {
                                           return issue.severity == DiagnosticSeverity::Blocking;
                                       });
        if (blockedByAttempt) {
            AddIssue(result, "CAPTURE_RUN_BLOCKED", "acquiring", DiagnosticSeverity::Blocking,
                     current.error.empty() ? "attempt reported a blocking diagnostic"
                                           : current.error,
                     "修复阻断诊断后重新开始新的采集计划");
        }
        if (current.state == CaptureAttemptState::Cancelled) cancelledByAttempt = true;
        std::string persistError;
        if (!AppendAttempt(plan, number, current, persistError) ||
            !PersistUpdate(plan, result, persistError)) {
            error = persistError;
            AddIssue(result, "RUN_PERSIST_FAILED", "persisting", DiagnosticSeverity::Blocking,
                     persistError, "保留当前已提交窗口并检查磁盘空间");
            result.state = CaptureRunState::Failed;
            return false;
        }
        if (current.state == CaptureAttemptState::Cancelled || cancelled_.load() ||
            blockedByAttempt || deadlineExceeded) break;
        if (!IsComplete(current) && !current.retryAllowed) {
            AddIssue(result, "CAPTURE_RETRY_UNSAFE", "acquiring", DiagnosticSeverity::Blocking,
                     "executor has not confirmed a safe state for another ARM",
                     "检查设备状态并重新开始采集，避免对未知布防状态自动重试");
            break;
        }
        if (result.completedWindows >= plan.targetCount || result.attempts >= plan.maxAttempts) break;
        if (plan.stopAfterFailures != 0 && consecutiveFailures >= plan.stopAfterFailures) {
            AddIssue(result, "CAPTURE_FAILURE_LIMIT", "acquiring", DiagnosticSeverity::Error,
                     "连续采集失败达到 stopAfterFailures", "检查设备、触发条件和链路后重试");
            break;
        }
        const auto delayUntil = std::min(deadline, std::chrono::steady_clock::now() +
            std::chrono::milliseconds(plan.interAttemptDelayMs));
        while (!cancelled_.load() && std::chrono::steady_clock::now() < delayUntil) {
            std::this_thread::sleep_until(std::min(delayUntil,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
        }
    }

    result.state = CaptureRunState::Stopping;
    if (cancelled_.load() || cancelledByAttempt) {
        result.state = CaptureRunState::Cancelled;
        AddIssue(result, "USER_CANCELLED", "acquiring", DiagnosticSeverity::Warning,
                 "用户取消了重复采集计划", "已提交窗口仍可从 run 目录打开");
        error = "Cancelled";
    } else if (!blockedByAttempt && !deadlineExceeded && result.completedWindows >= plan.targetCount) {
        result.state = CaptureRunState::Completed;
        error.clear();
    } else {
        result.state = CaptureRunState::Failed;
        if (deadlineExceeded) error = "capture run total timeout exceeded";
        if (error.empty()) error = "capture run ended before targetCount was reached";
        AddIssue(result, "CAPTURE_RUN_INCOMPLETE", "acquiring", DiagnosticSeverity::Error,
                 error, "查看每次 attempt 的诊断并调整计划");
    }
    std::string persistError;
    if (!PersistUpdate(plan, result, persistError)) {
        if (error.empty()) error = persistError;
        result.state = CaptureRunState::Failed;
        return false;
    }
    return result.state == CaptureRunState::Completed;
}

} // namespace debug
} // namespace sigflow
