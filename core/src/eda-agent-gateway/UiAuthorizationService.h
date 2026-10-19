#pragma once

#include <eda/api/Types.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace eda {
namespace agent {

// Native client for the Core Gateway.  The UI identity is deliberately kept
// separate from the Agent identity: only the former can issue/revoke grants;
// all job reads, submissions and cancellation use the Agent token.
class CoreGatewayClient final {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 0;
        std::string agentToken;
        std::string uiToken;
        std::string protocol = "edu.api.v1";
    };

    struct Response {
        bool transportOk = false;
        int httpStatus = 0;
        Json envelope = Json::object();
        std::string errorCode;
        std::string errorMessage;

        bool ok() const {
            return transportOk && httpStatus >= 200 && httpStatus < 300 && errorCode.empty();
        }
        const Json& data() const {
            static const Json empty = Json::object();
            return envelope.contains("data") ? envelope["data"] : empty;
        }
    };

    explicit CoreGatewayClient(Config config);

    Response CreateSnapshot(const std::string& projectId,
                            const std::string& expectedRevision) const;
    Response IssueGrant(const std::string& projectId, const std::string& planHash,
                        const std::string& revision, const std::string& snapshotId,
                        const Json& steps, std::uint64_t ttlSeconds,
                        std::size_t maxJobs) const;
    Response GetGrant(const std::string& grantId) const;
    Response RevokeGrant(const std::string& grantId) const;
    Response SubmitJob(const std::string& projectId, const std::string& grantId,
                       const std::string& snapshotId, const std::string& revision,
                       const std::string& planHash, const std::string& stepId,
                       const std::string& capability, const Json& params,
                       const std::string& idempotencyKey) const;
    Response GetJobState(const std::string& jobId) const;
    Response GetReport(const std::string& jobId) const;
    Response ListJobs(const std::string& projectId) const;
    Response CancelJob(const std::string& jobId) const;

    const Config& config() const { return config_; }

private:
    Response Request(const char* method, const std::string& path,
                     const std::string& token, const Json* body = nullptr,
                     const std::string& idempotencyKey = {}) const;
    Response ParseResponse(int status, const std::string& body) const;

    Config config_;
};

// The one native approval/execution path used by UI and Agent.  It performs
// canonical plan hashing, issues a snapshot-bound grant, submits each step
// with a stable idempotency key, observes the same Gateway Job state, and
// translates symbolic build_step_id dependencies into the actual build job id.
class UiAuthorizationService final {
public:
    struct Plan {
        std::string projectId;
        std::string revision;
        Json steps = Json::array();
        std::uint64_t ttlSeconds = 600;
        std::size_t maxJobs = 3;
    };

    struct Authorization {
        std::string projectId;
        std::string revision;
        std::string planHash;
        std::string snapshotId;
        std::string grantId;
    };

    struct StepResult {
        std::string stepId;
        std::string jobId;
        std::string state;
        bool replayed = false;
    };

    struct Execution {
        bool completed = false;
        bool cancelled = false;
        std::vector<StepResult> steps;
        CoreGatewayClient::Response response;
    };

    explicit UiAuthorizationService(CoreGatewayClient::Config config);

    // Hash includes only stable project/revision/steps content.  Any plan
    // edit therefore invalidates an existing Authorization before submission.
    static std::string CanonicalPlanHash(const Plan& plan);

    CoreGatewayClient::Response Authorize(const Plan& plan, Authorization& out) const;
    Execution ExecutePlan(const Plan& plan, const Authorization& authorization,
                          std::uint64_t timeoutMs = 30000) const;
    CoreGatewayClient::Response Observe(const std::string& jobId) const;
    CoreGatewayClient::Response Cancel(const std::string& jobId) const;

    const CoreGatewayClient& gateway() const { return gateway_; }

private:
    static CoreGatewayClient::Response LocalError(const std::string& code,
                                                  const std::string& message);
    CoreGatewayClient::Response Reconcile(const Plan& plan, const Authorization& authorization,
                                          const Json& step) const;
    bool WaitForTerminal(const std::string& jobId, std::uint64_t timeoutMs,
                         CoreGatewayClient::Response& terminal) const;
    void TrackOwnedJob(const std::string& jobId) const;
    bool OwnsJob(const std::string& jobId) const;

    CoreGatewayClient gateway_;
    mutable std::mutex ownershipMutex_;
    mutable std::set<std::string> ownedJobs_;
};

} // namespace agent
} // namespace eda
