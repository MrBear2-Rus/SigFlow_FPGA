#include "UiAuthorizationService.h"

#include "eda-platform/Sha256.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace eda {
namespace agent {
namespace {

std::string StringField(const Json& object, const char* key) {
    return object.is_object() && object.contains(key) && object[key].is_string()
               ? object[key].get<std::string>()
               : std::string();
}

bool HasEnvelopeIdentity(const Json& envelope, const std::string& protocol) {
    return envelope.is_object() && envelope.value("schema_version", std::string()) == protocol &&
           !StringField(envelope, "request_id").empty() &&
           !StringField(envelope, "trace_id").empty();
}

bool IsTerminal(const std::string& state) {
    return state == "Succeeded" || state == "Failed" || state == "Cancelled" ||
           state == "TimedOut";
}

} // namespace

CoreGatewayClient::CoreGatewayClient(Config config) : config_(std::move(config)) {}

CoreGatewayClient::Response CoreGatewayClient::CreateSnapshot(
    const std::string& projectId, const std::string& expectedRevision) const {
    Json body{{"expected_revision", expectedRevision}, {"require_saved", true}};
    return Request("POST", "/api/v1/projects/" + projectId + "/snapshots",
                   config_.agentToken, &body);
}

CoreGatewayClient::Response CoreGatewayClient::IssueGrant(
    const std::string& projectId, const std::string& planHash, const std::string& revision,
    const std::string& snapshotId, const Json& steps, std::uint64_t ttlSeconds,
    std::size_t maxJobs) const {
    Json body{{"plan_hash", planHash},
              {"revision", revision},
              {"snapshot_id", snapshotId},
              {"steps", steps},
              {"ttl_seconds", ttlSeconds},
              {"max_jobs", maxJobs}};
    return Request("POST", "/api/v1/projects/" + projectId + "/grants",
                   config_.uiToken, &body);
}

CoreGatewayClient::Response CoreGatewayClient::GetGrant(const std::string& grantId) const {
    return Request("GET", "/api/v1/grants/" + grantId, config_.agentToken);
}

CoreGatewayClient::Response CoreGatewayClient::RevokeGrant(const std::string& grantId) const {
    return Request("POST", "/api/v1/grants/" + grantId + "/revoke", config_.uiToken,
                   nullptr);
}

CoreGatewayClient::Response CoreGatewayClient::SubmitJob(
    const std::string& projectId, const std::string& grantId, const std::string& snapshotId,
    const std::string& revision, const std::string& planHash, const std::string& stepId,
    const std::string& capability, const Json& params,
    const std::string& idempotencyKey) const {
    Json body{{"snapshot_id", snapshotId},
              {"expected_revision", revision},
              {"capability", capability},
              {"grant_id", grantId},
              {"params", params},
              {"plan_hash", planHash},
              {"step_id", stepId}};
    return Request("POST", "/api/v1/projects/" + projectId + "/jobs",
                   config_.agentToken, &body, idempotencyKey);
}

CoreGatewayClient::Response CoreGatewayClient::GetJobState(const std::string& jobId) const {
    return Request("GET", "/api/v1/jobs/" + jobId, config_.agentToken);
}

CoreGatewayClient::Response CoreGatewayClient::GetReport(const std::string& jobId) const {
    return Request("GET", "/api/v1/jobs/" + jobId + "/report", config_.agentToken);
}

CoreGatewayClient::Response CoreGatewayClient::ListJobs(const std::string& projectId) const {
    return Request("GET", "/api/v1/projects/" + projectId + "/jobs?limit=500",
                   config_.agentToken);
}

CoreGatewayClient::Response CoreGatewayClient::CancelJob(const std::string& jobId) const {
    return Request("POST", "/api/v1/jobs/" + jobId + "/cancel", config_.agentToken,
                   nullptr);
}

CoreGatewayClient::Response CoreGatewayClient::Request(
    const char* method, const std::string& path, const std::string& token,
    const Json* body, const std::string& idempotencyKey) const {
    Response result;
    if (config_.port <= 0 || config_.port > 65535 || token.empty()) {
        result.errorCode = "INVALID_CLIENT_CONFIG";
        result.errorMessage = "Core Gateway client has no valid port or token";
        return result;
    }
    httplib::Client client(config_.host, config_.port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(30, 0);
    client.set_write_timeout(2, 0);
    httplib::Headers headers{{"Authorization", "Bearer " + token}};
    if (!idempotencyKey.empty()) headers.emplace("Idempotency-Key", idempotencyKey);
    httplib::Result response;
    if (std::string(method) == "GET") {
        response = client.Get(path.c_str(), headers);
    } else {
        const std::string payload = body == nullptr ? "{}" : body->dump();
        response = client.Post(path.c_str(), headers, payload, "application/json");
    }
    if (!response) {
        result.errorCode = "SERVICE_UNAVAILABLE";
        result.errorMessage = "Core Gateway connection failed";
        return result;
    }
    result.transportOk = true;
    return ParseResponse(response->status, response->body);
}

CoreGatewayClient::Response CoreGatewayClient::ParseResponse(
    int status, const std::string& body) const {
    Response result;
    result.transportOk = true;
    result.httpStatus = status;
    try {
        result.envelope = Json::parse(body);
    } catch (const std::exception&) {
        result.errorCode = "INVALID_RESPONSE";
        result.errorMessage = "Core Gateway returned invalid JSON";
        return result;
    }
    if (!HasEnvelopeIdentity(result.envelope, config_.protocol)) {
        result.errorCode = "INVALID_RESPONSE";
        result.errorMessage = "Core Gateway response envelope is incompatible";
        return result;
    }
    const Json error = result.envelope.value("error", Json());
    if (error.is_object()) {
        result.errorCode = StringField(error, "code");
        result.errorMessage = StringField(error, "message");
        if (result.errorCode.empty()) result.errorCode = "REMOTE_ERROR";
    } else if (status < 200 || status >= 300) {
        result.errorCode = "HTTP_ERROR";
        result.errorMessage = "Core Gateway request failed";
    }
    return result;
}

UiAuthorizationService::UiAuthorizationService(CoreGatewayClient::Config config)
    : gateway_(std::move(config)) {}

std::string UiAuthorizationService::CanonicalPlanHash(const Plan& plan) {
    const Json canonical{{"project_id", plan.projectId},
                          {"revision", plan.revision},
                          {"steps", plan.steps},
                          {"ttl_seconds", plan.ttlSeconds},
                          {"max_jobs", plan.maxJobs}};
    const std::string serialized = canonical.dump();
    return platform::Sha256Hex(serialized.data(), serialized.size());
}

CoreGatewayClient::Response UiAuthorizationService::LocalError(
    const std::string& code, const std::string& message) {
    CoreGatewayClient::Response response;
    response.errorCode = code;
    response.errorMessage = message;
    return response;
}

CoreGatewayClient::Response UiAuthorizationService::Authorize(
    const Plan& plan, Authorization& out) const {
    out = {};
    if (plan.projectId.empty() || plan.revision.empty() || !plan.steps.is_array() ||
        plan.steps.empty()) {
        return LocalError("INVALID_PLAN", "project, revision and non-empty steps are required");
    }
    const std::string planHash = CanonicalPlanHash(plan);
    const auto snapshot = gateway_.CreateSnapshot(plan.projectId, plan.revision);
    if (!snapshot.ok()) return snapshot;
    const std::string snapshotId = StringField(snapshot.data(), "snapshot_id");
    if (snapshotId.empty() || StringField(snapshot.data(), "revision") != plan.revision) {
        return LocalError("INVALID_RESPONSE", "snapshot response is not bound to the plan revision");
    }
    const auto grant = gateway_.IssueGrant(plan.projectId, planHash, plan.revision, snapshotId,
                                           plan.steps, plan.ttlSeconds, plan.maxJobs);
    if (!grant.ok()) return grant;
    const std::string grantId = StringField(grant.data(), "grant_id");
    if (grantId.empty() || StringField(grant.data(), "plan_hash") != planHash ||
        StringField(grant.data(), "snapshot_id") != snapshotId ||
        StringField(grant.data(), "revision") != plan.revision) {
        return LocalError("INVALID_RESPONSE", "grant response is not bound to the approved plan");
    }
    out.projectId = plan.projectId;
    out.revision = plan.revision;
    out.planHash = planHash;
    out.snapshotId = snapshotId;
    out.grantId = grantId;
    return grant;
}

CoreGatewayClient::Response UiAuthorizationService::Reconcile(
    const Plan& plan, const Authorization& authorization, const Json& step) const {
    const auto list = gateway_.ListJobs(plan.projectId);
    if (!list.ok() || !list.data().value("jobs", Json::array()).is_array()) return list;
    const std::string stepId = StringField(step, "step_id");
    const std::string capability = StringField(step, "capability");
    for (const Json& job : list.data()["jobs"]) {
        if (!job.is_object() || StringField(job, "plan_hash") != authorization.planHash ||
            StringField(job, "grant_id") != authorization.grantId ||
            StringField(job, "step_id") != stepId ||
            StringField(job, "snapshot_id") != authorization.snapshotId ||
            StringField(job, "capability") != capability) {
            continue;
        }
        CoreGatewayClient::Response recovered;
        recovered.transportOk = true;
        recovered.httpStatus = 202;
        recovered.envelope = Json{{"schema_version", gateway_.config().protocol},
                                  {"request_id", "reconciled"},
                                  {"trace_id", "reconciled"},
                                  {"data", Json{{"job_id", StringField(job, "job_id")},
                                                  {"state", StringField(job, "state")},
                                                  {"replayed", true}}}};
        TrackOwnedJob(StringField(job, "job_id"));
        return recovered;
    }
    return LocalError("RECOVERY_REQUIRED", "job submission outcome is unknown; query before retrying");
}

bool UiAuthorizationService::WaitForTerminal(
    const std::string& jobId, std::uint64_t timeoutMs,
    CoreGatewayClient::Response& terminal) const {
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        terminal = gateway_.GetJobState(jobId);
        if (!terminal.ok()) return false;
        const std::string state = StringField(terminal.data(), "state");
        if (IsTerminal(state)) return true;
        if (timeoutMs == 0 || static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count()) >= timeoutMs) {
            terminal = LocalError("TIMEOUT_RECONCILIATION", "job did not reach a terminal state");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

UiAuthorizationService::Execution UiAuthorizationService::ExecutePlan(
    const Plan& plan, const Authorization& authorization, std::uint64_t timeoutMs) const {
    Execution result;
    if (authorization.projectId != plan.projectId || authorization.revision != plan.revision ||
        authorization.planHash != CanonicalPlanHash(plan) || authorization.snapshotId.empty() ||
        authorization.grantId.empty()) {
        result.response = LocalError("PLAN_CHANGED", "plan changed or authorization is stale; re-approve");
        return result;
    }
    if (!plan.steps.is_array() || plan.steps.empty()) {
        result.response = LocalError("INVALID_PLAN", "plan has no executable steps");
        return result;
    }

    std::map<std::string, std::string> stepJobs;
    for (const Json& step : plan.steps) {
        if (!step.is_object()) {
            result.response = LocalError("INVALID_PLAN", "plan step must be an object");
            return result;
        }
        const std::string stepId = StringField(step, "step_id");
        const std::string capability = StringField(step, "capability");
        if (stepId.empty() || capability.empty()) {
            result.response = LocalError("INVALID_PLAN", "plan step needs step_id and capability");
            return result;
        }
        if (step.contains("depends_on") && step["depends_on"].is_array()) {
            for (const Json& dependency : step["depends_on"]) {
                const std::string dependencyId = dependency.is_string()
                                                    ? dependency.get<std::string>()
                                                    : std::string();
                if (dependencyId.empty() || stepJobs.find(dependencyId) == stepJobs.end()) {
                    result.response = LocalError("DEPENDENCY_UNSATISFIED",
                                                 "plan dependency has not completed");
                    return result;
                }
            }
        }
        Json params = step.value("params", Json::object());
        if (!params.is_object()) {
            result.response = LocalError("INVALID_PLAN", "step params must be an object");
            return result;
        }
        if (params.contains("build_step_id")) {
            const std::string buildStep = StringField(params, "build_step_id");
            const auto build = stepJobs.find(buildStep);
            if (build == stepJobs.end()) {
                result.response = LocalError("DEPENDENCY_UNSATISFIED", "approved build step is missing");
                return result;
            }
            params.erase("build_step_id");
            params["build_job_id"] = build->second;
        }
        const std::string keyMaterial = "sigflow-edu-plan-v1\n" + plan.projectId + "\n" +
                                        authorization.planHash + "\n" + stepId;
        const std::string idempotencyKey =
            platform::Sha256Hex(keyMaterial.data(), keyMaterial.size());
        auto submitted = gateway_.SubmitJob(plan.projectId, authorization.grantId,
                                             authorization.snapshotId, plan.revision,
                                             authorization.planHash, stepId, capability, params,
                                             idempotencyKey);
        if (!submitted.ok() &&
            (!submitted.transportOk || submitted.errorCode == "RECOVERY_REQUIRED")) {
            submitted = Reconcile(plan, authorization, step);
        }
        if (!submitted.ok()) {
            result.response = submitted;
            return result;
        }
        const std::string jobId = StringField(submitted.data(), "job_id");
        if (jobId.empty()) {
            result.response = LocalError("INVALID_RESPONSE", "job submission omitted job_id");
            return result;
        }
        TrackOwnedJob(jobId);
        CoreGatewayClient::Response terminal;
        const bool reachedTerminal = WaitForTerminal(jobId, timeoutMs, terminal);
        if (!reachedTerminal) {
            result.steps.push_back(StepResult{stepId, jobId, "Unknown", false});
            result.response = terminal;
            return result;
        }
        const std::string state = StringField(terminal.data(), "state");
        result.steps.push_back(StepResult{stepId, jobId, state,
                                          submitted.data().value("replayed", false)});
        stepJobs[stepId] = jobId;
        result.response = terminal;
        if (state != "Succeeded") {
            result.cancelled = state == "Cancelled";
            result.completed = true;
            return result;
        }
    }
    result.completed = true;
    return result;
}

CoreGatewayClient::Response UiAuthorizationService::Observe(const std::string& jobId) const {
    return gateway_.GetJobState(jobId);
}

CoreGatewayClient::Response UiAuthorizationService::Cancel(const std::string& jobId) const {
    // Gateway returns accepted=true as an acknowledgement.  The caller must
    // Observe() before presenting a terminal "Cancelled" state.
    if (!OwnsJob(jobId)) {
        return LocalError("POLICY_DENIED", "job is outside the current authorization session");
    }
    return gateway_.CancelJob(jobId);
}

void UiAuthorizationService::TrackOwnedJob(const std::string& jobId) const {
    if (jobId.empty()) return;
    std::lock_guard<std::mutex> lock(ownershipMutex_);
    ownedJobs_.insert(jobId);
}

bool UiAuthorizationService::OwnsJob(const std::string& jobId) const {
    std::lock_guard<std::mutex> lock(ownershipMutex_);
    return ownedJobs_.find(jobId) != ownedJobs_.end();
}

} // namespace agent
} // namespace eda
