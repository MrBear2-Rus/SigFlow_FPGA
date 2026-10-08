#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <vector>

#include <wx/string.h>

enum class ToolJobType {
    Simulation,
    Synthesis,
    PnR,
    Pack,
    Flash,
};

enum class ToolJobState {
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

struct ToolJobTransition {
    ToolJobState state = ToolJobState::Created;
    wxString timestamp;
    wxString operatorName;
    wxString reason;
    int exitCode = 0;
};

struct ToolJobRequest {
    ToolJobType type = ToolJobType::Simulation;
    wxString projectPath;
    wxString operatorName = "local";
    wxString retryOf;
    Json::Value parameters = Json::objectValue;
};

struct ToolJob {
    wxString id;
    wxString retryOf;
    ToolJobRequest request;
    ToolJobState state = ToolJobState::Created;
    wxString createdAt;
    wxString updatedAt;
    int exitCode = 0;
    std::vector<ToolJobTransition> transitions;
};

struct JobArtifact {
    wxString path;
    wxString kind;
    std::uint64_t sizeBytes = 0;
    wxString sha256;
};

struct JobError {
    wxString code;
    wxString severity = "error";
    wxString stage;
    wxString irCoordinate;
    wxString summary;
    int logLine = 0;
};

struct JobReport {
    wxString schemaVersion = "1.0";
    ToolJobType jobType = ToolJobType::Simulation;
    wxString jobId;
    ToolJobState state = ToolJobState::Created;
    wxString startedAt;
    wxString completedAt;
    std::int64_t durationMs = 0;
    int exitCode = 0;
    wxString summary;
    std::vector<JobArtifact> artifacts;
    std::vector<JobError> errors;
};

struct ToolJobPaths {
    wxString root;
    wxString inputs;
    wxString scripts;
    wxString logs;
    wxString artifacts;
    wxString reports;
    wxString manifest;
    wxString jobReport;
};

struct JobExecutionOptions {
    bool requireConfirm = false;
    std::function<bool()> confirm;
    std::function<void(const wxString&, bool)> output;
    std::uint64_t maxOutputBytes = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t memoryLimitBytes = 0;
};

wxString ToString(ToolJobType type);
bool ParseToolJobType(const wxString& value, ToolJobType& type);
wxString ToString(ToolJobState state);
bool ParseToolJobState(const wxString& value, ToolJobState& state);
bool IsTerminalToolJobState(ToolJobState state);

class JobService {
public:
    bool Create(const ToolJobRequest& request, ToolJob& job, wxString& errorMessage) const;
    bool Load(const wxString& projectPath, const wxString& jobId, ToolJob& job,
              wxString& errorMessage) const;
    bool List(const wxString& projectPath, std::vector<ToolJob>& jobs,
              wxString& errorMessage) const;
    bool Transition(const wxString& projectPath, const wxString& jobId,
                    ToolJobState targetState, const wxString& reason, int exitCode,
                    wxString& errorMessage) const;
    bool Start(const wxString& projectPath, const wxString& jobId,
               wxString& errorMessage) const;
    bool Cancel(const wxString& projectPath, const wxString& jobId,
                const wxString& reason, wxString& errorMessage) const;
    bool Retry(const wxString& projectPath, const wxString& jobId, ToolJob& retryJob,
               wxString& errorMessage) const;

    // 回收僵尸 Running 作业；activeJobIds 中的作业视为活动，绝不回收。
    bool RecoverStaleJobs(const wxString& projectPath,
                          const std::vector<wxString>& activeJobIds,
                          wxString& errorMessage) const;

    bool WriteReport(const wxString& projectPath, const wxString& jobId,
                     const JobReport& report, wxString& errorMessage) const;
    bool LoadReport(const wxString& projectPath, const wxString& jobId,
                    JobReport& report, wxString& errorMessage) const;
    bool RecordArtifact(const wxString& projectPath, const wxString& jobId,
                        const wxString& path, const wxString& kind,
                        wxString& errorMessage) const;

    static ToolJobPaths GetPaths(const wxString& projectPath, ToolJobType type,
                                 const wxString& jobId);
    static wxString Sha256File(const wxString& path);

    // 运行中进程登记：执行器登记可终止句柄，Cancel 据此真正终止工具进程。
    static void RegisterProcess(const wxString& projectPath, const wxString& jobId,
                                void* processHandle);
    static void UnregisterProcess(const wxString& projectPath, const wxString& jobId);
    static bool IsCancelRequested(const wxString& projectPath, const wxString& jobId);
    static void ClearCancelRequested(const wxString& projectPath, const wxString& jobId);

    // 并发上限：默认同类 1 个 Running。
    static void SetConcurrencyLimit(ToolJobType type, std::size_t limit);
    static std::size_t ConcurrencyLimit(ToolJobType type);

private:
    static bool IsLegalTransition(ToolJobState source, ToolJobState target);
    static bool IsSafeJobId(const wxString& value);
};
