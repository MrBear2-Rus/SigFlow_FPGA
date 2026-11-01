#include "JobRuntime.h"
#include "JobPaths.h"
#include "Toolchain.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"
#include <eda/api/process.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <set>
#include <stdexcept>
namespace fs = std::filesystem;
namespace eda::web {
namespace {
class Runtime final : public JobContext, public IProcessHost {
  public:
    ExecutionRequest input;
    JobReport report;
    EventSink sink;
    std::mutex mutex, hostMutex;
    std::unique_ptr<IProcessHost> host;
    std::atomic<bool> stopping{false}, timedOut{false}, broken{false};
    ProcessResult last;
    std::ofstream logs;
    std::uint64_t outputBytes = 0;
    bool truncated = false;
    std::set<std::string> artifactIds;
    explicit Runtime(ExecutionRequest r, EventSink s) : input(std::move(r)), sink(std::move(s)) {
        report.jobId = input.jobId;
        report.jobType = input.request.jobType;
        report.pluginId = input.request.pluginId;
        report.exitCode = kNoToolExitCode;
        report.metrics = Json::array();
        report.diagnostics = Json::array();
    }
    void emit(const Json& event) {
        if (broken.load())
            return;
        try {
            sink(event);
        } catch (...) {
            broken = true;
            stopping = true;
            Cancel();
        }
    }
    void log(const std::string& line, bool error) override {
        std::lock_guard<std::mutex> lock(mutex);
        size_t accepted = line.size();
        if (input.maxOutputBytes)
            accepted = static_cast<size_t>(
                std::min<std::uint64_t>(accepted, input.maxOutputBytes - outputBytes));
        if (accepted < line.size())
            truncated = true;
        if (!accepted)
            return;
        outputBytes += accepted;
        auto text = line.substr(0, accepted);
        logs.write(text.data(), text.size());
        if (!logs) {
            broken = true;
            stopping = true;
            Cancel();
            return;
        }
        emit(Json{{"type", "log"}, {"level", error ? "error" : "info"}, {"line", text}});
    }
    void progress(int percent, const std::string& status) override {
        if (percent < 0 || percent > 100)
            throw std::invalid_argument("progress outside [0,100]");
        std::lock_guard<std::mutex> lock(mutex);
        emit(Json{{"type", "progress"}, {"percent", percent}, {"status", status}});
    }
    bool cancelled() const override { return stopping.load(); }
    fs::path jobDir() const override { return input.workDir; }
    IProcessHost& processHost() override { return *this; }
    void emitMetric(const Json& metric) override { report.metrics.push_back(metric); }
    void registerArtifact(const Artifact& artifact) override {
        auto path = CheckedPath(input.workDir, artifact.path, true);
        auto artifactRoot = input.workDir / "artifacts";
        path = CheckedPath(artifactRoot, path, true);
        if (!fs::is_regular_file(path) || artifact.id.empty() ||
            !artifactIds.insert(artifact.id).second)
            throw std::invalid_argument("invalid or duplicate artifact");
        auto hash = platform::Sha256FileHex(path);
        if (hash.size() != 64 || (!artifact.sha256.empty() && artifact.sha256 != hash))
            throw std::runtime_error("artifact digest mismatch");
        auto copy = artifact;
        copy.path = path.lexically_relative(input.workDir);
        copy.sha256 = hash;
        report.artifacts.push_back(copy);
        emit(Json{{"type", "artifact"},
                  {"id", copy.id},
                  {"path", copy.path.string()},
                  {"role", copy.role},
                  {"sha256", copy.sha256}});
    }
    ProcessResult Run(const ProcessSpec& original, const ProcessOutputCallback& callback) override {
        if (stopping.load()) {
            ProcessResult r;
            r.outcome = ProcessOutcome::Cancelled;
            return last = r;
        }
        ProcessSpec spec = original;
        spec.workingDirectory = CheckedPath(
            input.workDir, spec.workingDirectory.empty() ? input.workDir : spec.workingDirectory,
            true);
        // Runtime owns the cumulative Job output cap; the underlying host still drains all pipes.
        spec.maxOutputBytes = input.maxOutputBytes;
        {
            std::lock_guard<std::mutex> lock(hostMutex);
            host = CreatePlatformProcessHost();
            if (stopping.load())
                host->Cancel();
        }
        auto result = host->Run(spec, [&](const auto& text, bool error) {
            try {
                if (callback)
                    callback(text, error);
                else
                    log(text, error);
            } catch (...) {
                broken = true;
                stopping = true;
                Cancel();
            }
        });
        if (result.outputTruncated)
            truncated = true;
        {
            std::lock_guard<std::mutex> lock(hostMutex);
            host.reset();
        }
        last = result;
        return result;
    }
    void Cancel() override {
        std::lock_guard<std::mutex> lock(hostMutex);
        if (host)
            host->Cancel();
    }
};
}  // namespace
const ExecutionRequest& ExecutionFor(const JobContext& context) {
    const auto* runtime = dynamic_cast<const Runtime*>(&context);
    if (!runtime)
        throw std::invalid_argument("context is not a Web execution context");
    return runtime->input;
}
ToolResolution ResolveExecutionTool(const JobContext& context, ToolQuery query) {
    const auto& input = ExecutionFor(context);
    std::vector<fs::path> defaults;
    if (!input.toolRoot.empty()) {
        if (input.request.metadata.value("tool_root_source", std::string()) == "argument")
            query.bundledRoots.insert(query.bundledRoots.begin(), input.toolRoot);
        else
            defaults.push_back(input.toolRoot);
    }
    return DefaultToolchain(defaults).Resolve(query);
}
fs::path CheckedInput(const JobContext& context, const fs::path& path) {
    const auto& input = ExecutionFor(context);
    fs::path resolved;
    try {
        resolved = CheckedPath(input.projectPath, path, true);
    } catch (const std::invalid_argument&) {
        if (!path.is_absolute() || input.dataDir.empty())
            throw;
        resolved = CheckedPath(input.dataDir, path, true);
    }
    if (!fs::is_regular_file(resolved))
        throw std::invalid_argument("input must be a regular file");
    return resolved;
}
int ExecuteJob(ExecutionRequest input, IJobProvider& provider, const EventSink& sink,
               const std::function<bool()>& cancelled) {
    if (input.request.jobType != provider.jobType() || !input.request.params.is_object() ||
        !input.request.metadata.is_object())
        throw std::invalid_argument("invalid request or provider mapping");
    input.projectPath = CheckedPath(input.projectPath, input.projectPath, true);
    input.workDir = CheckedPath(input.projectPath, input.workDir, true);
    auto expected = input.projectPath / ".sigflow/agent/jobs" / input.request.jobType / input.jobId;
    if (input.workDir != expected || !fs::is_directory(input.workDir) ||
        !fs::is_empty(input.workDir))
        throw std::invalid_argument("Job directory must match identity and be empty");
    Runtime runtime(input, sink);
    // Exclusive claim prevents two CLI instances from executing the same directory.
    if (!fs::create_directory(input.workDir / ".runtime-claim"))
        throw std::runtime_error("Job already claimed");
    AtomicJson(input.workDir, "request.json", RequestToJson(input));
    fs::create_directory(input.workDir / "artifacts");
    fs::create_directory(input.workDir / "logs");
    runtime.logs.open(input.workDir / "logs/tool.log", std::ios::binary);
    if (!runtime.logs)
        throw std::runtime_error("cannot open job log");
    JobRecord record;
    record.id = input.jobId;
    record.request = input.request;
    record.createdAt = platform::UtcTimestamp();
    auto transition = [&](JobState state) {
        record.state = state;
        record.exitCode = runtime.report.exitCode;
        record.updatedAt = platform::UtcTimestamp();
        record.transitions.push_back({state, record.updatedAt, "execute", "", record.exitCode});
        AtomicJson(input.workDir, "manifest.json", RecordToJson(record));
    };
    transition(JobState::Running);
    std::atomic<bool> done{false};
    auto started = std::chrono::steady_clock::now();
    std::thread monitor([&] {
        while (!done.load()) {
            if (cancelled && cancelled()) {
                runtime.stopping = true;
                runtime.Cancel();
            }
            if (input.request.timeoutSec.value_or(0) > 0 &&
                std::chrono::steady_clock::now() - started >=
                    std::chrono::seconds(*input.request.timeoutSec)) {
                runtime.timedOut = true;
                runtime.stopping = true;
                runtime.Cancel();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    Error error;
    try {
        if (provider.requiresConfirm() && !input.request.requireConfirm)
            error = {ErrorCode::InvalidArgument, "explicit hardware confirmation required", ""};
        else
            error = provider.startJob(input.request, runtime);
        if (!error && !runtime.stopping) {
            transition(JobState::ValidatingArtifact);
            for (const auto& artifact : runtime.report.artifacts) {
                auto path = CheckedPath(input.workDir, artifact.path, true);
                if (platform::Sha256FileHex(path) != artifact.sha256)
                    throw std::runtime_error("artifact changed after registration");
            }
        }
    } catch (const std::exception& e) {
        error = {ErrorCode::Internal, e.what(), ""};
    } catch (...) {
        error = {ErrorCode::Internal, "unknown provider exception", ""};
    }
    done = true;
    monitor.join();
    if (!error && runtime.last.started && runtime.last.outcome != ProcessOutcome::Success) {
        auto code = runtime.last.outcome == ProcessOutcome::TimedOut    ? ErrorCode::TimedOut
                    : runtime.last.outcome == ProcessOutcome::Cancelled ? ErrorCode::Cancelled
                                                                        : ErrorCode::Internal;
        error = {code, "tool execution did not succeed", ""};
    }
    if (!error && !runtime.last.started && !runtime.last.errorMessage.empty())
        error = {ErrorCode::Crashed, runtime.last.errorMessage, ""};
    runtime.report.exitCode = runtime.last.started ? runtime.last.exitCode : kNoToolExitCode;
    if (runtime.timedOut || error.code == ErrorCode::TimedOut)
        runtime.report.state = JobState::TimedOut;
    else if (runtime.stopping || error.code == ErrorCode::Cancelled)
        runtime.report.state = JobState::Cancelled;
    else
        runtime.report.state = error ? JobState::Failed : JobState::Succeeded;
    if (runtime.broken)
        throw std::runtime_error("event/log channel failed; execution stopped");
    if (error) {
        runtime.report.diagnostics.push_back(
            Json{{"code", ToString(error.code)}, {"message", error.message}});
        runtime.emit(Json{{"type", "diagnostic"},
                          {"severity", "error"},
                          {"code", ToString(error.code)},
                          {"message", error.message},
                          {"file", ""},
                          {"line", 0}});
    }
    if (runtime.truncated)
        runtime.report.metrics.push_back(Json{{"output_truncated", true}});
    runtime.logs.flush();
    if (!runtime.logs)
        throw std::runtime_error("log flush failed");
    if (runtime.report.state != JobState::Succeeded)
        runtime.report.artifacts.clear();
    AtomicJson(input.workDir, "reports/report.json", ReportToJson(runtime.report));
    transition(runtime.report.state);
    runtime.emit(ResultEvent(runtime.report));
    if (runtime.broken)
        throw std::runtime_error("final result channel failed");
    return runtime.report.state == JobState::Succeeded ? 0 : 4;
}
}  // namespace eda::web
