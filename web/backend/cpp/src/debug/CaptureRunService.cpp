#include "CaptureRunService.h"
#include "eda-core/JobPaths.h"

#include <eda/api/Types.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace eda::debug {

namespace {

std::string NowUtc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

Json IssueJson(const DiagnosticIssue& issue) {
    Json evidence = Json::array();
    for (const auto& item : issue.evidence)
        evidence.push_back(item);
    return {{"code", issue.code},
            {"stage", issue.stage},
            {"severity", ToString(issue.severity)},
            {"observed", issue.observed},
            {"action", issue.action},
            {"evidence", std::move(evidence)}};
}

Json PlanJson(const CaptureRunPlan& plan) {
    return {{"run_id", plan.runId},
            {"output_root", plan.outputRoot},
            {"device_id", plan.deviceId},
            {"contract_snapshot", plan.contractSnapshot},
            {"runtime_config_hash", plan.runtimeConfigHash},
            {"target_count", plan.targetCount},
            {"max_attempts", plan.maxAttempts},
            {"stop_after_failures", plan.stopAfterFailures},
            {"total_timeout_ms", plan.totalTimeoutMs},
            {"attempt_timeout_ms", plan.attemptTimeoutMs},
            {"inter_attempt_delay_ms", plan.interAttemptDelayMs}};
}

Json WindowJson(const CaptureRunWindow& window) {
    Json value = {{"capture_id", window.captureId},
                  {"session_id", window.sessionId},
                  {"raw_path", window.rawPath},
                  {"vcd_path", window.vcdPath},
                  {"completeness", window.completeness},
                  {"identity", window.identity},
                  {"data_hash", window.dataHash},
                  {"duplicate_of", window.duplicateOf},
                  {"classification", window.classification},
                  {"sample_count", window.sampleCount},
                  {"trigger_index", window.triggerIndex},
                  {"metadata_path", window.metadataPath}};
    if (!window.metadataPath.empty()) {
        value["timebase"] = {
            {"time_domain", window.timeDomain},
            {"vcd_timescale", window.vcdTimescale},
            {"sample_clock_hz", window.sampleClockHz},
            {"decimation", window.decimation},
            {"trigger_observed", window.triggerObserved},
            {"trigger_time", window.triggerObserved ? Json(window.triggerTime) : Json(nullptr)}};
    }
    return value;
}

Json AttemptJson(std::size_t number, const CaptureAttemptResult& attempt) {
    const auto& timing = attempt.timing;
    const auto& transport = attempt.transportStats;
    const auto& protocol = attempt.protocolStats;
    Json issues = Json::array();
    for (const auto& issue : attempt.issues)
        issues.push_back(IssueJson(issue));
    return {{"attempt_number", number},
            {"attempt_id", attempt.attemptId},
            {"started_at", attempt.startedAtUtc},
            {"completed_at", attempt.completedAtUtc},
            {"elapsed_ms", attempt.elapsedMs},
            {"state", ToString(attempt.state)},
            {"error", attempt.error},
            {"retry_allowed", attempt.retryAllowed},
            {"timing",
             {{"arm_requested_at", timing.armRequestedAtUtc},
              {"wait_started_at", timing.waitStartedAtUtc},
              {"capture_ready_at", timing.captureReadyAtUtc},
              {"readback_started_at", timing.readbackStartedAtUtc},
              {"readback_completed_at", timing.readbackCompletedAtUtc},
              {"artifacts_completed_at", timing.artifactsCompletedAtUtc},
              {"wait_ms", timing.waitMs},
              {"readback_ms", timing.readbackMs},
              {"artifact_ms", timing.artifactMs},
              {"gap_before_ms", timing.gapBeforeMs},
              {"gap_before_basis", timing.gapBeforeBasis},
              {"continuity", timing.continuity}}},
            {"transport_stats",
             {{"read_calls", transport.readCalls},
              {"read_bytes", transport.readBytes},
              {"read_elapsed_us", transport.readElapsedUs},
              {"read_timeouts", transport.readTimeouts},
              {"read_disconnects", transport.readDisconnects},
              {"write_calls", transport.writeCalls},
              {"write_bytes", transport.writeBytes},
              {"write_elapsed_us", transport.writeElapsedUs},
              {"write_timeouts", transport.writeTimeouts},
              {"write_disconnects", transport.writeDisconnects},
              {"discard_calls", transport.discardCalls}}},
            {"protocol_stats",
             {{"command_calls", protocol.commandCalls},
              {"attempts", protocol.attempts},
              {"retries", protocol.retries},
              {"response_timeouts", protocol.responseTimeouts},
              {"write_failures", protocol.writeFailures},
              {"malformed_frames", protocol.malformedFrames},
              {"crc_errors", protocol.crcErrors},
              {"sequence_mismatches", protocol.sequenceMismatches},
              {"invalid_responses", protocol.invalidResponses}}},
            {"window", WindowJson(attempt.window)},
            {"issues", std::move(issues)}};
}

Json ResultJson(const CaptureRunResult& result) {
    Json issues = Json::array();
    for (const auto& issue : result.issues)
        issues.push_back(IssueJson(issue));
    return {{"schema_version", "1.0"},
            {"run_id", result.runId},
            {"state", ToString(result.state)},
            {"attempts", result.attempts},
            {"completed_windows", result.completedWindows},
            {"timeout_count", result.timeoutCount},
            {"failure_count", result.failureCount},
            {"issues", std::move(issues)}};
}

Json DiagnosticsJson(const CaptureRunPlan& plan, const CaptureRunResult& result) {
    Json runIssues = Json::array();
    Json attemptIssues = Json::array();
    for (const auto& issue : result.issues)
        runIssues.push_back(IssueJson(issue));
    for (std::size_t index = 0; index < result.attemptResults.size(); ++index) {
        const auto& attempt = result.attemptResults[index];
        for (const auto& issue : attempt.issues) {
            auto entry = IssueJson(issue);
            entry["attempt_id"] = attempt.attemptId;
            entry["attempt_number"] = index + 1;
            attemptIssues.push_back(std::move(entry));
        }
    }
    return {{"schema_version", "1.0"},
            {"run_id", result.runId},
            {"state", ToString(result.state)},
            {"generated_at", NowUtc()},
            {"plan_id", plan.runId},
            {"summary",
             {{"attempts", result.attempts},
              {"completed_windows", result.completedWindows},
              {"timeout_count", result.timeoutCount},
              {"failure_count", result.failureCount}}},
            {"issues", std::move(runIssues)},
            {"attempt_issues", std::move(attemptIssues)}};
}

std::filesystem::path RunRoot(const CaptureRunPlan& plan) {
    return std::filesystem::absolute(plan.outputRoot).lexically_normal() / plan.runId;
}

bool NormalizeArtifactPath(const std::filesystem::path& root, std::string& value,
                           std::string& error) {
    namespace fs = std::filesystem;
    if (value.empty())
        return true;
    if (value.find('\\') != std::string::npos ||
        (value.size() > 1 && std::isalpha(static_cast<unsigned char>(value[0])) &&
         value[1] == ':')) {
        error = "capture artifact path must use a run-relative POSIX path";
        return false;
    }
    try {
        const fs::path checked = web::CheckedPath(root, fs::path(value), true);
        if (!fs::is_regular_file(checked)) {
            error = "capture artifact path is not a regular file";
            return false;
        }
        const auto relative = checked.lexically_relative(fs::absolute(root).lexically_normal());
        if (relative.empty() || relative == "." || relative.is_absolute()) {
            error = "capture artifact path is not relative to the run directory";
            return false;
        }
        value = relative.generic_string();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool NormalizeWindowArtifacts(const std::filesystem::path& root, CaptureAttemptResult& attempt,
                              std::string& error) {
    const bool claimsComplete = attempt.state == CaptureAttemptState::Captured &&
                                attempt.window.completeness == "complete" &&
                                !attempt.window.captureId.empty();
    if (claimsComplete && (attempt.window.rawPath.empty() || attempt.window.vcdPath.empty())) {
        error = "complete capture requires raw and VCD artifacts";
        return false;
    }
    return NormalizeArtifactPath(root, attempt.window.rawPath, error) &&
           NormalizeArtifactPath(root, attempt.window.vcdPath, error) &&
           NormalizeArtifactPath(root, attempt.window.metadataPath, error);
}

bool StoreJson(const std::filesystem::path& root, const std::filesystem::path& relative,
               const Json& value, std::string& error) {
    try {
        web::AtomicJson(root, relative, value);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool AppendJsonLine(const std::filesystem::path& root, const Json& value, std::string& error) {
    const std::string line = value.dump() + "\n";
    int fd = -1;
    try {
        const auto path = web::CheckedPath(root, "windows.jsonl", true);
        fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            throw std::runtime_error("unable to open windows.jsonl: " +
                                     std::string(std::strerror(errno)));
        std::size_t offset = 0;
        while (offset < line.size()) {
            const auto written = write(fd, line.data() + offset, line.size() - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                throw std::runtime_error("unable to append capture attempt");
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(fd) != 0)
            throw std::runtime_error("unable to sync capture attempt");
        if (close(fd) != 0) {
            fd = -1;
            throw std::runtime_error("unable to close windows.jsonl");
        }
        fd = -1;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        if (fd >= 0)
            close(fd);
        error = exception.what();
        return false;
    }
}

bool CreateAttemptLog(const std::filesystem::path& root, std::string& error) {
    try {
        const auto path = web::CheckedPath(root, "windows.jsonl", false);
        const int fd =
            open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
            throw std::runtime_error("unable to create windows.jsonl: " +
                                     std::string(std::strerror(errno)));
        const int syncStatus = fsync(fd);
        const int savedErrno = errno;
        close(fd);
        if (syncStatus != 0)
            throw std::runtime_error("unable to sync windows.jsonl: " +
                                     std::string(std::strerror(savedErrno)));
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

}  // namespace

const char* ToString(CaptureRunState state) {
    switch (state) {
    case CaptureRunState::Created:
        return "Created";
    case CaptureRunState::Validating:
        return "Validating";
    case CaptureRunState::Running:
        return "Running";
    case CaptureRunState::Paused:
        return "Paused";
    case CaptureRunState::Stopping:
        return "Stopping";
    case CaptureRunState::Completed:
        return "Completed";
    case CaptureRunState::Cancelled:
        return "Cancelled";
    case CaptureRunState::Failed:
        return "Failed";
    }
    return "Unknown";
}

const char* ToString(CaptureAttemptState state) {
    switch (state) {
    case CaptureAttemptState::Planned:
        return "Planned";
    case CaptureAttemptState::Running:
        return "Running";
    case CaptureAttemptState::Captured:
        return "Captured";
    case CaptureAttemptState::TimedOut:
        return "TimedOut";
    case CaptureAttemptState::Failed:
        return "Failed";
    case CaptureAttemptState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

const char* ToString(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Info:
        return "info";
    case DiagnosticSeverity::Warning:
        return "warning";
    case DiagnosticSeverity::Error:
        return "error";
    case DiagnosticSeverity::Blocking:
        return "blocking";
    }
    return "error";
}

std::string CaptureNowUtc() {
    return NowUtc();
}

bool CaptureRunService::ValidatePlan(const CaptureRunPlan& plan, std::string& error) {
    // Portable single directory component; also excludes Windows reserved names,
    // drive paths, trailing dots and case-insensitive device-name aliases.
    if (plan.runId.size() > 128 || plan.runId.rfind("run-", 0) != 0 || plan.runId.size() <= 4 ||
        plan.runId.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos) {
        error = "capture run runId must start with run- and contain only letters, digits, - or _ "
                "(max 128)";
        return false;
    }
    if (plan.outputRoot.empty()) {
        error = "capture run outputRoot is empty";
        return false;
    }
    if (plan.targetCount == 0) {
        error = "capture run targetCount must be greater than zero";
        return false;
    }
    if (plan.attemptTimeoutMs == 0) {
        error = "capture run attemptTimeoutMs must be greater than zero";
        return false;
    }
    if (plan.totalTimeoutMs == 0) {
        error = "capture run totalTimeoutMs must be greater than zero";
        return false;
    }
    if (plan.maxAttempts != 0 && plan.maxAttempts < plan.targetCount) {
        error = "capture run maxAttempts must cover targetCount";
        return false;
    }
    if (plan.stopAfterFailures != 0 && plan.stopAfterFailures > plan.maxAttempts &&
        plan.maxAttempts != 0) {
        error = "capture run stopAfterFailures exceeds maxAttempts";
        return false;
    }
    return true;
}

std::string CaptureRunService::EnsureRunId(const std::string& requested) {
    if (!requested.empty())
        return requested;
    static std::atomic<std::uint64_t> sequence{1};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return "run-" + std::to_string(now) + "-" + std::to_string(sequence.fetch_add(1));
}

std::string CaptureRunService::MakeAttemptId(const std::string& runId, std::size_t number) {
    return runId + "-attempt-" + std::to_string(number);
}

bool CaptureRunService::IsComplete(const CaptureAttemptResult& attempt) {
    return attempt.state == CaptureAttemptState::Captured &&
           attempt.window.completeness == "complete" && !attempt.window.captureId.empty();
}

bool CaptureRunService::IsTimeout(const CaptureAttemptResult& attempt) {
    return attempt.state == CaptureAttemptState::TimedOut ||
           attempt.error.find("timeout") != std::string::npos ||
           attempt.error.find("Timeout") != std::string::npos ||
           attempt.error.find("超时") != std::string::npos;
}

void CaptureRunService::AddIssue(CaptureRunResult& result, const std::string& code,
                                 const std::string& stage, DiagnosticSeverity severity,
                                 const std::string& observed, const std::string& action) {
    result.issues.push_back({code, stage, severity, observed, action, {}});
}

bool CaptureRunService::PersistStart(const CaptureRunPlan& plan, const CaptureRunResult& result,
                                     std::string& error) {
    namespace fs = std::filesystem;
    const fs::path outputRoot = fs::absolute(plan.outputRoot).lexically_normal();
    try {
        web::CheckedPath(outputRoot, ".", false);
        fs::create_directories(outputRoot);
        const fs::path root = web::CheckedPath(outputRoot, plan.runId, false);
        std::error_code ec;
        if (!fs::create_directory(root, ec)) {
            error = ec ? "unable to reserve run directory: " + ec.message()
                       : "capture run directory already exists: " + root.string();
            return false;
        }
        Json manifest = {{"schema_version", "1.0"},
                         {"created_at", result.createdAtUtc},
                         {"plan", PlanJson(plan)},
                         {"result", ResultJson(result)}};
        if (!StoreJson(root, "run.json", manifest, error) ||
            !StoreJson(root, "diagnostics.json", DiagnosticsJson(plan, result), error))
            return false;
        return CreateAttemptLog(root, error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool CaptureRunService::PersistUpdate(const CaptureRunPlan& plan, const CaptureRunResult& result,
                                      std::string& error) {
    try {
        const auto root = RunRoot(plan);
        web::CheckedPath(root, ".", true);
        Json manifest = {{"schema_version", "1.0"},
                         {"created_at", result.createdAtUtc},
                         {"updated_at", NowUtc()},
                         {"plan", PlanJson(plan)},
                         {"result", ResultJson(result)}};
        if (!StoreJson(root, "run.json", manifest, error))
            return false;
        return StoreJson(root, "diagnostics.json", DiagnosticsJson(plan, result), error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool CaptureRunService::AppendAttempt(const CaptureRunPlan& plan, std::size_t attemptNumber,
                                      const CaptureAttemptResult& attempt, std::string& error) {
    try {
        const auto root = RunRoot(plan);
        web::CheckedPath(root, ".", true);
        return AppendJsonLine(root, AttemptJson(attemptNumber, attempt), error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool CaptureRunService::WaitIfPaused(const CaptureRunPlan& plan, CaptureRunResult& result,
                                     std::chrono::steady_clock::time_point& deadline,
                                     std::string& error) {
    if (!pauseRequested_.load() || cancelled_.load())
        return true;
    const auto pausedAt = std::chrono::steady_clock::now();
    result.state = CaptureRunState::Paused;
    if (!PersistUpdate(plan, result, error)) {
        result.state = CaptureRunState::Failed;
        AddIssue(result, "RUN_PERSIST_FAILED", "persisting", DiagnosticSeverity::Blocking, error,
                 "检查暂停状态写入和磁盘空间");
        return false;
    }
    std::unique_lock<std::mutex> lock(pauseMutex_);
    pauseCv_.wait(lock, [this] { return !pauseRequested_.load() || cancelled_.load(); });
    if (cancelled_.load())
        return true;
    deadline += std::chrono::steady_clock::now() - pausedAt;
    result.state = CaptureRunState::Running;
    if (!PersistUpdate(plan, result, error)) {
        result.state = CaptureRunState::Failed;
        AddIssue(result, "RUN_PERSIST_FAILED", "persisting", DiagnosticSeverity::Blocking, error,
                 "检查恢复状态写入和磁盘空间");
        return false;
    }
    return true;
}

bool CaptureRunService::Run(const CaptureRunPlan& inputPlan, const CaptureAttemptCallback& attempt,
                            CaptureRunResult& result, std::string& error) {
    error.clear();
    std::unique_lock<std::mutex> runLock(runMutex_, std::try_to_lock);
    if (!runLock.owns_lock()) {
        result = CaptureRunResult();
        result.runId = inputPlan.runId;
        result.state = CaptureRunState::Failed;
        error = "capture run service is already running";
        AddIssue(result, "CAPTURE_RUN_BUSY", "validating", DiagnosticSeverity::Blocking, error,
                 "等待当前 run 结束后再复用该 service");
        return false;
    }
    result = CaptureRunResult();
    CaptureRunPlan plan = inputPlan;
    plan.runId = EnsureRunId(plan.runId);
    result.runId = plan.runId;
    result.createdAtUtc = NowUtc();
    cancelled_.store(false);
    pauseRequested_.store(false);

    result.state = CaptureRunState::Validating;
    if (!ValidatePlan(plan, error)) {
        result.state = CaptureRunState::Failed;
        AddIssue(result, "CAPTURE_RUN_INVALID", "validating", DiagnosticSeverity::Blocking, error,
                 "修正采集次数、超时和输出目录后重试");
        return false;
    }
    if (!attempt) {
        error = "capture run attempt callback is empty";
        result.state = CaptureRunState::Failed;
        AddIssue(result, "CAPTURE_RUN_NO_EXECUTOR", "validating", DiagnosticSeverity::Blocking,
                 error, "连接单次 DebugAcquisition 执行器后重试");
        return false;
    }

    const std::size_t derivedMax = plan.targetCount > (std::numeric_limits<std::size_t>::max() / 3)
                                       ? std::numeric_limits<std::size_t>::max()
                                       : plan.targetCount * 3;
    if (plan.maxAttempts == 0)
        plan.maxAttempts = std::max(plan.targetCount, derivedMax);

    if (!PersistStart(plan, result, error)) {
        result.state = CaptureRunState::Failed;
        AddIssue(result, "RUN_PERSIST_FAILED", "persisting", DiagnosticSeverity::Blocking, error,
                 "检查采集目录权限和磁盘空间");
        return false;
    }

    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(plan.totalTimeoutMs);
    result.state = CaptureRunState::Running;
    if (!PersistUpdate(plan, result, error)) {
        result.state = CaptureRunState::Failed;
        AddIssue(result, "RUN_PERSIST_FAILED", "persisting", DiagnosticSeverity::Blocking, error,
                 "检查运行状态写入和磁盘空间");
        return false;
    }
    std::size_t consecutiveFailures = 0;
    bool cancelledByAttempt = false;
    bool blockedByAttempt = false;
    bool deadlineExceeded = false;
    bool havePreviousWindow = false;
    std::unordered_map<std::string, std::string> dataHashOwners;
    std::chrono::steady_clock::time_point previousWindowCompleted;
    while (result.completedWindows < plan.targetCount && result.attempts < plan.maxAttempts) {
        if (!WaitIfPaused(plan, result, deadline, error))
            return false;
        if (cancelled_.load())
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
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
        const auto attemptDeadline =
            std::min(deadline, attemptStarted + std::chrono::milliseconds(plan.attemptTimeoutMs));
        const auto remainingTimeout =
            std::chrono::duration_cast<std::chrono::milliseconds>(attemptDeadline - attemptStarted)
                .count();
        const CaptureAttemptContext context{
            plan,
            plan.runId,
            MakeAttemptId(plan.runId, number),
            number,
            remainingTimeout > 0 ? static_cast<std::uint32_t>(remainingTimeout) : 0u,
            cancelled_,
            attemptDeadline};
        try {
            current = attempt(context);
        } catch (const std::exception& ex) {
            current.state = CaptureAttemptState::Failed;
            current.error = std::string("attempt callback exception: ") + ex.what();
        } catch (...) {
            current.state = CaptureAttemptState::Failed;
            current.error = "attempt callback unknown exception";
        }
        std::string artifactPathError;
        if (!NormalizeWindowArtifacts(RunRoot(plan), current, artifactPathError)) {
            current.state = CaptureAttemptState::Failed;
            current.retryAllowed = false;
            current.error = "capture artifact validation failed: " + artifactPathError;
            current.issues.push_back({"CAPTURE_ARTIFACT_PATH_INVALID",
                                      "persisting",
                                      DiagnosticSeverity::Blocking,
                                      artifactPathError,
                                      "将完整采集产物写入当前 run 目录，并使用相对路径",
                                      {}});
        }
        // These fields belong to the scheduler, never to the callback.
        current.attemptId = attemptId;
        current.startedAtUtc = startedAtUtc;
        current.completedAtUtc = NowUtc();
        current.elapsedMs =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - attemptStarted)
                                           .count());
        if (havePreviousWindow) {
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 attemptStarted - previousWindowCompleted)
                                 .count();
            current.timing.gapBeforeMs = gap > 0 ? static_cast<std::uint64_t>(gap) : 0;
            current.timing.gapBeforeBasis = "host_observed";
            current.timing.continuity = "disconnected";
        }
        const auto attemptCompleted = std::chrono::steady_clock::now();
        if (cancelled_.load() && current.state != CaptureAttemptState::Cancelled) {
            current.state = CaptureAttemptState::Cancelled;
            current.error = "Cancelled";
        } else if (attemptCompleted >= attemptDeadline &&
                   current.state != CaptureAttemptState::Cancelled) {
            current.state = CaptureAttemptState::TimedOut;
            if (current.error.empty())
                current.error = "capture attempt timeout exceeded";
        }
        if (!cancelled_.load() && attemptCompleted >= deadline) {
            deadlineExceeded = true;
            AddIssue(result, "CAPTURE_RUN_TIMEOUT", "acquiring", DiagnosticSeverity::Error,
                     "attempt returned after the run deadline; committed evidence is retained",
                     "检查耗时阶段并调整总期限；阻塞 I/O 仍需遵守后端命令超时");
        }

        if (IsComplete(current)) {
            ++result.completedWindows;
            consecutiveFailures = 0;
            previousWindowCompleted = std::chrono::steady_clock::now();
            havePreviousWindow = true;
            if (current.window.classification == "unknown") {
                current.window.classification =
                    current.window.triggerObserved ? "triggered" : "complete_untriggered";
            }
            if (!current.window.dataHash.empty()) {
                const auto [owner, inserted] =
                    dataHashOwners.emplace(current.window.dataHash, current.attemptId);
                if (!inserted) {
                    current.window.duplicateOf = owner->second;
                    current.window.classification = "duplicate";
                }
            }
        } else if (current.state == CaptureAttemptState::Cancelled || cancelled_.load()) {
            current.state = CaptureAttemptState::Cancelled;
            current.window.classification = "cancelled";
        } else if (IsTimeout(current)) {
            ++result.timeoutCount;
            ++consecutiveFailures;
            current.window.classification = "timeout";
        } else {
            ++result.failureCount;
            ++consecutiveFailures;
            current.window.classification = "failed";
        }
        if (!IsComplete(current) && !current.error.empty())
            error = current.error;
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
        if (current.state == CaptureAttemptState::Cancelled)
            cancelledByAttempt = true;
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
            blockedByAttempt || deadlineExceeded)
            break;
        if (!IsComplete(current) && !current.retryAllowed) {
            AddIssue(result, "CAPTURE_RETRY_UNSAFE", "acquiring", DiagnosticSeverity::Blocking,
                     "executor has not confirmed a safe state for another ARM",
                     "检查设备状态并重新开始采集，避免对未知布防状态自动重试");
            break;
        }
        if (result.completedWindows >= plan.targetCount || result.attempts >= plan.maxAttempts)
            break;
        if (plan.stopAfterFailures != 0 && consecutiveFailures >= plan.stopAfterFailures) {
            AddIssue(result, "CAPTURE_FAILURE_LIMIT", "acquiring", DiagnosticSeverity::Error,
                     "连续采集失败达到 stopAfterFailures", "检查设备、触发条件和链路后重试");
            break;
        }
        const auto delayUntil =
            std::min(deadline, std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(plan.interAttemptDelayMs));
        while (!cancelled_.load() && std::chrono::steady_clock::now() < delayUntil) {
            std::this_thread::sleep_until(std::min(delayUntil, std::chrono::steady_clock::now() +
                                                                   std::chrono::milliseconds(10)));
        }
    }

    result.state = CaptureRunState::Stopping;
    if (cancelled_.load() || cancelledByAttempt) {
        result.state = CaptureRunState::Cancelled;
        AddIssue(result, "USER_CANCELLED", "acquiring", DiagnosticSeverity::Warning,
                 "用户取消了重复采集计划", "已提交窗口仍可从 run 目录打开");
        error = "Cancelled";
    } else if (!blockedByAttempt && !deadlineExceeded &&
               result.completedWindows >= plan.targetCount) {
        result.state = CaptureRunState::Completed;
        error.clear();
    } else {
        result.state = CaptureRunState::Failed;
        if (deadlineExceeded)
            error = "capture run total timeout exceeded";
        if (error.empty())
            error = "capture run ended before targetCount was reached";
        AddIssue(result, "CAPTURE_RUN_INCOMPLETE", "acquiring", DiagnosticSeverity::Error, error,
                 "查看每次 attempt 的诊断并调整计划");
    }
    std::string persistError;
    if (!PersistUpdate(plan, result, persistError)) {
        if (error.empty())
            error = persistError;
        result.state = CaptureRunState::Failed;
        return false;
    }
    return result.state == CaptureRunState::Completed;
}

}  // namespace eda::debug
