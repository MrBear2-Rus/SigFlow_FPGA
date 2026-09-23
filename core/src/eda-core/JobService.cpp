#include "JobService.h"

#include "Platform.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace eda {
namespace {

const char* ToString(JobState state) {
    switch (state) {
        case JobState::Created:            return "Created";
        case JobState::Validating:         return "Validating";
        case JobState::Queued:             return "Queued";
        case JobState::Running:            return "Running";
        case JobState::ValidatingArtifact: return "ValidatingArtifact";
        case JobState::Succeeded:          return "Succeeded";
        case JobState::Failed:             return "Failed";
        case JobState::Cancelled:          return "Cancelled";
        case JobState::TimedOut:           return "TimedOut";
    }
    return "Unknown";
}

bool IsTerminal(JobState state) {
    return state == JobState::Succeeded || state == JobState::Failed ||
           state == JobState::Cancelled || state == JobState::TimedOut;
}

std::string NowUtc() { return platform::UtcTimestamp(); }

std::string NowCompactUtc() { return platform::UtcCompactTimestamp(); }

Json RecordToJson(const JobRecord& record) {
    Json j;
    j["schema_version"] = "eda.job.v1";
    j["job_id"] = record.id;
    j["retry_of"] = record.retryOf;
    j["job_type"] = record.request.jobType;
    j["plugin_id"] = record.request.pluginId;
    j["project_id"] = record.request.projectId;
    j["state"] = ToString(record.state);
    j["created_at"] = record.createdAt;
    j["updated_at"] = record.updatedAt;
    j["exit_code"] = record.exitCode;
    j["parameters"] = record.request.params;
    Json transitions = Json::array();
    for (const auto& transition : record.transitions) {
        transitions.push_back(Json{{"state", ToString(transition.state)},
                                   {"timestamp", transition.timestamp},
                                   {"op", transition.op},
                                   {"reason", transition.reason},
                                   {"exit_code", transition.exitCode}});
    }
    j["transitions"] = transitions;
    return j;
}

Json ReportToJson(const JobReport& report) {
    Json j;
    j["schema_version"] = report.schemaVersion;
    j["job_id"] = report.jobId;
    j["job_type"] = report.jobType;
    j["plugin_id"] = report.pluginId;
    j["state"] = ToString(report.state);
    j["exit_code"] = report.exitCode;
    Json artifacts = Json::array();
    for (const auto& artifact : report.artifacts) {
        artifacts.push_back(Json{{"id", artifact.id},
                                 {"path", artifact.path.string()},
                                 {"schema", artifact.schema},
                                 {"sha256", artifact.sha256},
                                 {"role", artifact.role}});
    }
    j["artifacts"] = artifacts;
    j["metrics"] = report.metrics;
    j["diagnostics"] = report.diagnostics;
    return j;
}

bool WriteJsonFile(const std::filesystem::path& path, const Json& value) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) return false;
    out << value.dump(2);
    return static_cast<bool>(out);
}

} // namespace

CoreJobContext::CoreJobContext(std::string jobId, std::filesystem::path jobDir,
                               ProcessHostFactory factory, JobLogSink logSink)
    : jobId_(std::move(jobId)),
      jobDir_(std::move(jobDir)),
      factory_(std::move(factory)),
      logSink_(std::move(logSink)) {}

void CoreJobContext::log(const std::string& line, bool isError) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        logs_.push_back((isError ? "[err] " : "") + line);
    }
    if (logSink_) logSink_(line, isError);
}

void CoreJobContext::progress(int percent, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    progressPercent_ = percent;
    progressStatus_ = status;
}

bool CoreJobContext::cancelled() const { return cancelled_.load(); }

void CoreJobContext::registerArtifact(const Artifact& artifact) {
    std::lock_guard<std::mutex> lock(mutex_);
    artifacts_.push_back(artifact);
}

void CoreJobContext::emitMetric(const Json& metric) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (metric.is_object()) {
        for (auto it = metric.begin(); it != metric.end(); ++it) {
            metrics_[it.key()] = it.value();
        }
    }
}

std::filesystem::path CoreJobContext::jobDir() const { return jobDir_; }

IProcessHost& CoreJobContext::processHost() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_) {
        if (!factory_) {
            throw std::runtime_error("no process host factory configured");
        }
        host_ = factory_();
        // Cancel 可能先于 Run 到达（provider 还没调用 processHost()）：
        // 此时新创建的 host 必须立即进入取消态，避免随后阻塞的 Run 无法被打断。
        if (cancelled_.load()) host_->Cancel();
    }
    return *host_;
}

void CoreJobContext::RequestCancel() {
    cancelled_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (host_) host_->Cancel();
}

std::vector<Artifact> CoreJobContext::Artifacts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return artifacts_;
}

Json CoreJobContext::Metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

std::vector<std::string> CoreJobContext::Logs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return logs_;
}

int CoreJobContext::ProgressPercent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progressPercent_;
}

std::string CoreJobContext::ProgressStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progressStatus_;
}

CoreJobService::CoreJobService(ProcessHostFactory factory, std::filesystem::path jobsRoot)
    : factory_(std::move(factory)), jobsRoot_(std::move(jobsRoot)) {
    if (jobsRoot_.empty()) {
        jobsRoot_ = std::filesystem::temp_directory_path() / "sigflow-jobs";
    }
}

CoreJobService::~CoreJobService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true);
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void CoreJobService::RegisterProvider(std::shared_ptr<IJobProvider> provider) {
    if (!provider) return;
    std::lock_guard<std::mutex> lock(mutex_);
    providers_[provider->jobType()] = std::move(provider);
}

void CoreJobService::SetDefaultTimeoutSeconds(int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    defaultTimeoutSeconds_ = seconds;
}

void CoreJobService::SetLogSink(JobLogSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    logSink_ = std::move(sink);
}

std::string CoreJobService::MakeJobId() {
    return "job-" + NowCompactUtc() + "-" + std::to_string(++sequence_);
}

std::filesystem::path CoreJobService::JobDirectory(const JobRequest& request,
                                                   const std::string& jobId) const {
    return jobsRoot_ / request.jobType / jobId;
}

std::string CoreJobService::submit(const JobRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string id = MakeJobId();
    JobRecord record;
    record.id = id;
    record.request = request;
    record.state = JobState::Created;
    record.createdAt = NowUtc();
    record.updatedAt = record.createdAt;
    if (request.timeoutSec.has_value()) {
        record.timeoutSec = request.timeoutSec;
    } else if (defaultTimeoutSeconds_ > 0) {
        record.timeoutSec = defaultTimeoutSeconds_;
    }
    record.transitions.push_back(
        {JobState::Created, record.createdAt, "service", "Job submitted.", 0});
    records_[id] = record;
    queue_.push_back(id);
    EnsureWorkersLocked();
    cv_.notify_all();
    return id;
}

bool CoreJobService::cancel(const std::string& jobId, const std::string& /*reason*/) {
    std::shared_ptr<CoreJobContext> context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end() || IsTerminal(it->second.state)) return false;
        cancelRequested_[jobId] = true;
        const auto ctxIt = contexts_.find(jobId);
        if (ctxIt != contexts_.end()) context = ctxIt->second;
    }
    if (context) context->RequestCancel();
    return true;
}

bool CoreJobService::retry(const std::string& jobId, std::string& outNewId) {
    JobRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end() || !IsTerminal(it->second.state)) return false;
        request = it->second.request;
    }
    outNewId = submit(request);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_[outNewId].retryOf = jobId;
    }
    return true;
}

std::optional<JobRecord> CoreJobService::get(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = records_.find(jobId);
    if (it == records_.end()) return std::nullopt;
    return it->second;
}

std::vector<JobRecord> CoreJobService::list(const std::string& projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobRecord> result;
    for (const auto& entry : records_) {
        if (projectId.empty() || entry.second.request.projectId == projectId) {
            result.push_back(entry.second);
        }
    }
    return result;
}

JobReport CoreJobService::report(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = reports_.find(jobId);
    if (it != reports_.end()) return it->second;
    JobReport report;
    const auto recordIt = records_.find(jobId);
    if (recordIt != records_.end()) {
        report.jobId = recordIt->second.id;
        report.jobType = recordIt->second.request.jobType;
        report.pluginId = recordIt->second.request.pluginId;
        report.state = recordIt->second.state;
        report.exitCode = recordIt->second.exitCode;
    }
    return report;
}

void CoreJobService::setConcurrency(std::size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    concurrency_ = std::max<std::size_t>(1, n);
    EnsureWorkersLocked();
    cv_.notify_all();
}

void CoreJobService::EnsureWorkersLocked() {
    while (workers_.size() < concurrency_) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
}

void CoreJobService::WorkerLoop() {
    for (;;) {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stopping_.load() || !queue_.empty();
            });
            if (stopping_.load() && queue_.empty()) return;
            id = queue_.front();
            queue_.pop_front();
        }
        Execute(id);
    }
}

bool CoreJobService::IsCancelRequested(const std::string& jobId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cancelRequested_.find(jobId);
    return it != cancelRequested_.end() && it->second;
}

bool CoreJobService::IsLegalTransition(JobState from, JobState to) const {
    switch (from) {
        case JobState::Created:
            return to == JobState::Validating || to == JobState::Cancelled ||
                   to == JobState::Failed;
        case JobState::Validating:
            return to == JobState::Queued || to == JobState::Cancelled ||
                   to == JobState::Failed;
        case JobState::Queued:
            return to == JobState::Running || to == JobState::Cancelled ||
                   to == JobState::Failed;
        case JobState::Running:
            return to == JobState::ValidatingArtifact || to == JobState::Cancelled ||
                   to == JobState::Failed || to == JobState::TimedOut;
        case JobState::ValidatingArtifact:
            return to == JobState::Succeeded || to == JobState::Failed ||
                   to == JobState::Cancelled || to == JobState::TimedOut;
        default:
            return false;
    }
}

bool CoreJobService::Transition(const std::string& jobId, JobState to,
                                const std::string& reason, int exitCode) {
    JobRecord snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end() || !IsLegalTransition(it->second.state, to)) return false;
        it->second.state = to;
        it->second.updatedAt = NowUtc();
        it->second.exitCode = exitCode;
        it->second.transitions.push_back({to, it->second.updatedAt, "service", reason, exitCode});
        snapshot = it->second;
    }
    PersistRecord(snapshot);
    return true;
}

void CoreJobService::PersistRecord(const JobRecord& record) {
    if (jobsRoot_.empty()) return;
    WriteJsonFile(JobDirectory(record.request, record.id) / "manifest.json",
                  RecordToJson(record));
}

void CoreJobService::PersistReport(const JobReport& report) {
    if (jobsRoot_.empty() || report.jobId.empty()) return;
    std::filesystem::path dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(report.jobId);
        if (it == records_.end()) return;
        dir = JobDirectory(it->second.request, report.jobId) / "reports";
    }
    WriteJsonFile(dir / "job-report.json", ReportToJson(report));
}

JobReport CoreJobService::BuildReport(const std::string& jobId, const CoreJobContext& ctx,
                                      const Error& error, bool timedOut) {
    JobReport report;
    report.schemaVersion = "eda.jobreport.v1";
    report.jobId = jobId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it != records_.end()) {
            report.jobType = it->second.request.jobType;
            report.pluginId = it->second.request.pluginId;
            report.state = it->second.state;
            report.exitCode = it->second.exitCode;
        }
    }
    report.artifacts = ctx.Artifacts();
    report.metrics = ctx.Metrics();
    report.diagnostics = Json::object();
    if (static_cast<bool>(error)) {
        report.diagnostics["error"] = error.message;
    }
    if (timedOut) {
        report.diagnostics["timeout"] = true;
    }
    return report;
}

void CoreJobService::Execute(const std::string& jobId) {
    JobRecord snapshot;
    std::shared_ptr<IJobProvider> provider;
    int fallbackTimeout = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end() || IsTerminal(it->second.state)) return;
        snapshot = it->second;
        fallbackTimeout = defaultTimeoutSeconds_;
        const auto providerIt = providers_.find(snapshot.request.jobType);
        if (providerIt != providers_.end()) provider = providerIt->second;
    }

    if (!provider) {
        Transition(jobId, JobState::Failed,
                   "No registered provider for job type: " + snapshot.request.jobType, -1);
        return;
    }

    const std::filesystem::path dir = JobDirectory(snapshot.request, jobId);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    if (provider->requiresConfirm() && !snapshot.request.requireConfirm) {
        Transition(jobId, JobState::Failed,
                   "Job requires explicit confirmation (requireConfirm).", -1);
        return;
    }

    if (!Transition(jobId, JobState::Validating, "Validation started.", 0)) return;
    if (IsCancelRequested(jobId)) {
        Transition(jobId, JobState::Cancelled, "Cancelled before queueing.", -1);
        return;
    }
    if (!Transition(jobId, JobState::Queued, "Job queued.", 0)) return;
    if (IsCancelRequested(jobId)) {
        Transition(jobId, JobState::Cancelled, "Cancelled before running.", -1);
        return;
    }
    if (!Transition(jobId, JobState::Running, "Job execution started.", 0)) return;

    auto context = std::make_shared<CoreJobContext>(jobId, dir, factory_, logSink_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        contexts_[jobId] = context;
    }
    if (IsCancelRequested(jobId)) context->RequestCancel();

    const auto run = [&]() -> Error {
        try {
            return provider->startJob(snapshot.request, *context);
        } catch (const std::exception& e) {
            return Error{ErrorCode::Internal, e.what(), ""};
        } catch (...) {
            return Error{ErrorCode::Internal, "unknown provider exception", ""};
        }
    };

    const int timeout = snapshot.timeoutSec.value_or(fallbackTimeout);
    Error providerError;
    bool timedOut = false;
    if (timeout > 0) {
        std::atomic<bool> done{false};
        std::thread runner([&]() {
            providerError = run();
            done.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!done.load()) {
            timedOut = true;
            context->RequestCancel();
        }
        runner.join();
    } else {
        providerError = run();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        contexts_.erase(jobId);
    }

    // 超时优先于取消：超时路径也调用了 RequestCancel()，若不先判 timedOut 会被误记为 Cancelled。
    if (timedOut) {
        Transition(jobId, JobState::TimedOut, "Job timed out.", -1);
    } else if (context->cancelled() || IsCancelRequested(jobId)) {
        Transition(jobId, JobState::Cancelled, "Job cancelled.", -1);
    } else if (static_cast<bool>(providerError)) {
        Transition(jobId, JobState::Failed, providerError.message, -1);
    } else if (Transition(jobId, JobState::ValidatingArtifact, "Validating artifacts.", 0)) {
        Transition(jobId, JobState::Succeeded, "Job completed.", 0);
    }

    const JobReport report = BuildReport(jobId, *context, providerError, timedOut);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reports_[jobId] = report;
    }
    PersistReport(report);
}

} // namespace eda
