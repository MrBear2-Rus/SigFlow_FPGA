#include "JobProtocol.h"
namespace eda::web {
const char* StateName(JobState state) {
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

Json RecordToJson(const JobRecord& record) {
    Json j;
    j["schema_version"] = "eda.job.v1";
    j["job_id"] = record.id;
    j["retry_of"] = record.retryOf;
    j["job_type"] = record.request.jobType;
    j["plugin_id"] = record.request.pluginId;
    j["project_id"] = record.request.projectId;
    j["state"] = StateName(record.state);
    j["created_at"] = record.createdAt;
    j["updated_at"] = record.updatedAt;
    j["exit_code"] = record.exitCode;
    j["parameters"] = record.request.params;
    j["metadata"] = record.request.metadata;
    Json transitions = Json::array();
    for (const auto& transition : record.transitions) {
        transitions.push_back(Json{{"state", StateName(transition.state)},
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
    j["state"] = StateName(report.state);
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

Json RequestToJson(const ExecutionRequest& input) {
    const auto& r = input.request;
    return Json{{"schema_version", "sigflow.execution-request.v1"},
                {"job_id", input.jobId}, {"job_type", r.jobType},
                {"plugin_id", r.pluginId}, {"project_id", r.projectId},
                {"project_path", input.projectPath.string()},
                {"work_dir", input.workDir.string()},
                {"parameters", r.params}, {"metadata", r.metadata},
                {"require_confirm", r.requireConfirm},
                {"timeout_sec", r.timeoutSec ? Json(*r.timeoutSec) : Json(nullptr)},
                {"max_output_bytes", input.maxOutputBytes},
                {"tool_root", input.toolRoot.string()}, {"data_dir", input.dataDir.string()},
                {"input_snapshot_id", input.inputSnapshotId ? Json(*input.inputSnapshotId) : Json(nullptr)}};
}
Json ResultEvent(const JobReport& report) {
    return Json{{"type", "result"}, {"state", StateName(report.state)},
                {"exitCode", report.exitCode}, {"jobId", report.jobId}};
}
} // namespace eda::web
