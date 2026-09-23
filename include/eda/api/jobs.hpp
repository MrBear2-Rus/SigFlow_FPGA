#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "services.hpp"

namespace eda {

class IProcessHost; // P0-4 定义；此处仅前向声明

enum class JobState {
    Created,
    Validating,
    Queued,
    Running,
    ValidatingArtifact,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
};

struct JobRequest {
    std::string jobType;
    std::string pluginId;
    std::string projectId;
    Json params;
    bool requireConfirm = false;
    // 覆盖服务默认超时；<=0 表示不超时；空表示用服务默认值。
    std::optional<int> timeoutSec;
};

struct JobTransition {
    JobState state = JobState::Created;
    std::string timestamp;
    std::string op;
    std::string reason;
    int exitCode = 0;
};

struct JobRecord {
    std::string id;
    std::string retryOf;
    JobRequest request;
    JobState state = JobState::Created;
    std::vector<JobTransition> transitions;
    std::string createdAt;
    std::string updatedAt;
    int exitCode = 0;
    std::string traceId;
    std::optional<int> timeoutSec;
};

struct JobReport {
    std::string schemaVersion = "eda.jobreport.v1";
    std::string jobId;
    std::string jobType;
    std::string pluginId;
    JobState state = JobState::Created;
    int exitCode = 0;
    std::vector<Artifact> artifacts;
    Json metrics;
    Json diagnostics;
};

class JobContext {
public:
    virtual ~JobContext() = default;
    virtual void log(const std::string& line, bool isError) = 0;
    virtual void progress(int percent, const std::string& status) = 0;
    virtual bool cancelled() const = 0;
    virtual void registerArtifact(const Artifact& artifact) = 0;
    virtual void emitMetric(const Json& metric) = 0;
    virtual std::filesystem::path jobDir() const = 0;
    virtual IProcessHost& processHost() = 0;
};

class IJobProvider {
public:
    virtual ~IJobProvider() = default;
    virtual std::string jobType() const = 0;
    virtual Json paramsSchema() const = 0;
    virtual Json resultSchema() const = 0;
    // 需要用户显式确认（如烧录实板）；JobRequest::requireConfirm 为 false 时服务应拒绝执行。
    virtual bool requiresConfirm() const { return false; }
    // 返回 Error::Ok() 表示成功；非 None 表示失败（服务据此置 Failed）。
    virtual Error startJob(const JobRequest& request, JobContext& ctx) = 0;
};

class IJobService : public Service {
public:
    virtual std::string submit(const JobRequest& request) = 0;
    virtual bool cancel(const std::string& jobId, const std::string& reason) = 0;
    virtual bool retry(const std::string& jobId, std::string& outNewId) = 0;
    virtual std::optional<JobRecord> get(const std::string& jobId) = 0;
    virtual std::vector<JobRecord> list(const std::string& projectId) = 0;
    virtual JobReport report(const std::string& jobId) = 0;
    virtual void setConcurrency(std::size_t n) = 0;
};

} // namespace eda
