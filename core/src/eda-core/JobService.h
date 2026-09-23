#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>

namespace eda {

using JobLogSink = std::function<void(const std::string&, bool)>;

// JobContext 的具体实现：持有 jobDir、按需创建 IProcessHost、收集 artifact/metric/log。
class CoreJobContext final : public JobContext {
public:
    using ProcessHostFactory = std::function<std::unique_ptr<IProcessHost>()>;

    CoreJobContext(std::string jobId, std::filesystem::path jobDir,
                   ProcessHostFactory factory, JobLogSink logSink);

    void log(const std::string& line, bool isError) override;
    void progress(int percent, const std::string& status) override;
    bool cancelled() const override;
    void registerArtifact(const Artifact& artifact) override;
    void emitMetric(const Json& metric) override;
    std::filesystem::path jobDir() const override;
    IProcessHost& processHost() override;

    // 服务内部使用。
    void RequestCancel();
    std::vector<Artifact> Artifacts() const;
    Json Metrics() const;
    std::vector<std::string> Logs() const;
    int ProgressPercent() const;
    std::string ProgressStatus() const;

private:
    std::string jobId_;
    std::filesystem::path jobDir_;
    ProcessHostFactory factory_;
    JobLogSink logSink_;
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mutex_;
    std::unique_ptr<IProcessHost> host_;
    std::vector<Artifact> artifacts_;
    Json metrics_ = Json::object();
    std::vector<std::string> logs_;
    int progressPercent_ = 0;
    std::string progressStatus_;
};

// IJobService 的核心实现：异步 worker 池 + 内存 Job 库 + 可选文件持久化。
// 注：startJob 由服务在子线程运行；超时或取消时置 cancelled 并终止进程树。
class CoreJobService final : public IJobService {
public:
    using ProcessHostFactory = std::function<std::unique_ptr<IProcessHost>()>;

    explicit CoreJobService(ProcessHostFactory factory,
                            std::filesystem::path jobsRoot = {});
    ~CoreJobService() override;

    void RegisterProvider(std::shared_ptr<IJobProvider> provider);
    void SetDefaultTimeoutSeconds(int seconds); // <=0：不超时（默认 0）
    void SetLogSink(JobLogSink sink);

    // IJobService
    std::string submit(const JobRequest& request) override;
    bool cancel(const std::string& jobId, const std::string& reason) override;
    bool retry(const std::string& jobId, std::string& outNewId) override;
    std::optional<JobRecord> get(const std::string& jobId) override;
    std::vector<JobRecord> list(const std::string& projectId) override;
    JobReport report(const std::string& jobId) override;
    void setConcurrency(std::size_t n) override;

private:
    void WorkerLoop();
    void Execute(const std::string& jobId);
    bool IsLegalTransition(JobState from, JobState to) const;
    bool Transition(const std::string& jobId, JobState to, const std::string& reason,
                    int exitCode);
    std::string MakeJobId();
    int ResolveTimeout(const JobRecord& record) const;
    std::filesystem::path JobDirectory(const JobRequest& request,
                                       const std::string& jobId) const;
    bool IsCancelRequested(const std::string& jobId) const;
    JobReport BuildReport(const std::string& jobId, const CoreJobContext& ctx,
                          const Error& error, bool timedOut);
    void PersistRecord(const JobRecord& record);
    void PersistReport(const JobReport& report);
    void EnsureWorkersLocked();

    ProcessHostFactory factory_;
    JobLogSink logSink_;
    std::filesystem::path jobsRoot_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::map<std::string, JobRecord> records_;
    std::map<std::string, JobReport> reports_;
    std::map<std::string, std::shared_ptr<CoreJobContext>> contexts_;
    std::map<std::string, bool> cancelRequested_;
    std::map<std::string, std::shared_ptr<IJobProvider>> providers_;
    std::vector<std::thread> workers_;
    std::size_t concurrency_ = 1;
    int defaultTimeoutSeconds_ = 0;
    std::atomic<bool> stopping_{false};
    std::uint64_t sequence_ = 0;
};

} // namespace eda
