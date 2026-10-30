#include "agent/AgentApiClient.h"

#include <httplib.h>

#include <chrono>
#include <atomic>
#include <cctype>
#include <future>
#include <thread>
#include <utility>

namespace {

std::string StringField(const Json::Value& value, const char* key) {
    return value.isObject() && value[key].isString() ? value[key].asString() : std::string();
}

bool HasEnvelopeIdentity(const Json::Value& envelope, const std::string& protocol) {
    return envelope.isObject() && StringField(envelope, "schema_version") == protocol &&
           !StringField(envelope, "request_id").empty() &&
           !StringField(envelope, "trace_id").empty();
}

Json::Value MakeSelection(const agent::gui::selection::Resolved& evidence,
                          const std::string& fallbackSourceId,
                          const std::string& fallbackNodeId) {
    Json::Value selection(Json::objectValue);
    if (!evidence.present) {
        // 未选中：保持历史（兼容）形状，text 为空字符串，绝不把问题正文当证据。
        selection["source_id"] = fallbackSourceId.empty() ? "ui-selection" : fallbackSourceId;
        if (!fallbackNodeId.empty()) selection["node_id"] = fallbackNodeId;
        selection["text"] = "";
        return selection;
    }
    // 线上只允许 opaque ID 与 64 KiB 内的片段：这里再校验一次，
    // 即使调用方绕过 AgentSelectionContext::Resolve 也不会把路径/非法 ID 发出去。
    const std::string sourceId = agent::gui::selection::SafeOpaqueId(evidence.sourceId)
                                     ? evidence.sourceId : std::string();
    const std::string nodeId = agent::gui::selection::SafeOpaqueId(evidence.nodeId)
                                   ? evidence.nodeId : std::string();
    bool clamped = false;
    const std::string boundedText = agent::gui::selection::ClampUtf8(
        evidence.text, agent::gui::selection::kMaxSelectionBytes, clamped);
    const bool truncated = evidence.truncated || clamped;
    if (!sourceId.empty()) selection["source_id"] = sourceId;
    if (!nodeId.empty()) selection["node_id"] = nodeId;
    if (evidence.startLine > 0 && evidence.endLine >= evidence.startLine) {
        selection["start_line"] = static_cast<Json::UInt64>(evidence.startLine);
        selection["end_line"] = static_cast<Json::UInt64>(evidence.endLine);
    }
    selection["text"] = boundedText;
    if (evidence.dirty) {
        // 未保存缓冲：显式标注来源与截断，服务端不得把它当作当前磁盘 source_ref。
        selection["origin"] = "buffer";
        selection["truncated"] = truncated;
    } else if (truncated) {
        selection["truncated"] = true;
    }
    return selection;
}

bool SafeId(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

/// U2 action 版本：canonical decimal `^(0|[1-9][0-9]{0,18})$` 且 < 2^63，
/// 与 sigflowAgent host `http.py::_version` 完全一致。拒绝符号、空白、前导零与超界。
bool CanonicalU2Version(const std::string& value) {
    if (value.empty() || value.size() > 19) return false;
    if (value.size() > 1 && value.front() == '0') return false;
    for (const unsigned char ch : value)
        if (ch < '0' || ch > '9') return false;
    if (value.size() == 19 && value > "9223372036854775807") return false;
    return true;
}

/// U2 recover 的 question：1..4096 UTF-8 字节、单行安全文本。
/// 拒绝路径样式、HTML 标签、脚本/本地协议与凭据样式，以及除换行/回车/制表外的控制符。
bool SafeRecoverQuestion(const std::string& question) {
    if (question.empty() || question.size() > 4096) return false;
    if (question.find_first_not_of(" \t\r\n") == std::string::npos) return false;
    for (const unsigned char ch : question) {
        if ((ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 0x7F) return false;
    }
    if (question.find('<') != std::string::npos ||
        question.find('>') != std::string::npos) return false;
    if (agent::gui::selection::LooksLikePath(question)) return false;
    if (agent::gui::selection::LooksLikeSensitiveText(question)) return false;
    return true;
}

std::string UniqueKey(const char* prefix) {
    static std::atomic<unsigned long long> count{0};
    return std::string(prefix) + "-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(++count);
}

TeachingSessionController::Result ReadTeachingState(
    const std::shared_ptr<const AgentApiClient>& client,
    TeachingSessionController::Result result, const std::string& learner,
    const std::string& previousVersion = {},
    const std::shared_ptr<std::atomic<bool>>& cancel = {}) {
    result.teaching = true;
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (cancel && cancel->load()) {
            // The UI is no longer bound to this run. This is best effort; the
            // caller discards the result and never claims cancellation succeeded.
            client->CancelTeachingRun(result.runId, learner);
            result.state = TeachingSessionController::Result::State::Cancelled;
            return result;
        }
        const auto response = client->GetTeachingRun(result.runId, learner);
        // The adapter may observe a concurrent commit between its projection
        // reads. Retrying this read is safe; never replay a mutation here.
        if (response.httpStatus == 409 && response.errorCode == "IDEMPOTENCY_CONFLICT") {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (!response.ok() || !response.envelope["data"].isObject()) {
            result.state = TeachingSessionController::Result::State::Failed;
            result.errorCode = response.errorCode.empty() ? "INVALID_RESPONSE" : response.errorCode;
            result.errorMessage = response.errorMessage;
            return result;
        }
        const auto& data = response.envelope["data"];
        if (StringField(data, "schema_version") != "u1.u2-teaching.v1" ||
            StringField(data, "session_id") != result.sessionId ||
            StringField(data, "run_id") != result.runId ||
            !data["state_version"].isString() || !data["cards"].isArray()) {
            result.state = TeachingSessionController::Result::State::Failed;
            result.errorCode = "INVALID_RESPONSE";
            result.errorMessage = "教学状态不符合当前会话协议";
            return result;
        }
        result.data = data;
        const auto status = StringField(data, "teaching_status");
        const bool changed = previousVersion.empty() || StringField(data, "state_version") != previousVersion;
        if (status == "Completed") {
            result.state = TeachingSessionController::Result::State::Succeeded;
            return result;
        }
        if (status == "Failed" || status == "BudgetExceeded" || status == "StartRequiresRetry") {
            result.state = TeachingSessionController::Result::State::Failed;
            result.errorCode = status;
            result.errorMessage = "教学流程未完成，请检查证据并重试。";
            return result;
        }
        if (status == "Cancelled") {
            result.state = TeachingSessionController::Result::State::Cancelled;
            return result;
        }
        if ((status == "WaitingStudent" || status == "WaitingApproval" ||
             status == "WaitingEvidence" || status == "RecoveryRequired") && changed) {
            result.state = TeachingSessionController::Result::State::Waiting;
            return result;
        }
        if (status != "Running" && status != "WaitingStudent" && status != "WaitingApproval" &&
            status != "WaitingEvidence" && status != "RecoveryRequired") {
            result.state = TeachingSessionController::Result::State::Failed;
            result.errorCode = "INVALID_RESPONSE";
            result.errorMessage = "教学服务返回了未知状态";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!result.data.isObject()) {
        result.state = TeachingSessionController::Result::State::Failed;
        result.errorCode = "IDEMPOTENCY_CONFLICT";
        result.errorMessage = "教学状态正在更新，请刷新重试。";
    } else result.state = TeachingSessionController::Result::State::Pending;
    return result;
}

} // namespace

AgentApiClient::AgentApiClient(Config config) : config_(std::move(config)) {}

AgentApiClient::Response AgentApiClient::Health() const {
    return Request("GET", "/api/v1/health");
}

AgentApiClient::Response AgentApiClient::Capabilities() const {
    return Request("GET", "/api/v1/capabilities");
}

AgentApiClient::Response AgentApiClient::CreateSession(
    const std::string& projectId, const std::string& revision, const std::string& top,
    const Json::Value& target, const std::string& locale) const {
    Json::Value body(Json::objectValue);
    body["project_id"] = projectId;
    body["revision"] = revision;
    if (!top.empty()) body["top"] = top;
    body["target"] = target;
    body["locale"] = locale;
    return Request("POST", "/api/v1/sessions", &body);
}

AgentApiClient::Response AgentApiClient::CreateRun(
    const std::string& sessionId, const std::string& kind, const std::string& level,
    const std::string& question, const std::string& sourceId, const std::string& nodeId,
    const agent::gui::selection::Resolved& selection) const {
    Json::Value body(Json::objectValue);
    body["kind"] = kind;
    body["level"] = level;
    body["selection"] = MakeSelection(selection, sourceId, nodeId);
    body["question"] = question;
    return Request("POST", "/api/v1/sessions/" + sessionId + "/runs", &body);
}

AgentApiClient::Response AgentApiClient::GetSession(const std::string& sessionId) const {
    return Request("GET", "/api/v1/sessions/" + sessionId);
}

AgentApiClient::Response AgentApiClient::GetRun(const std::string& runId) const {
    return Request("GET", "/api/v1/runs/" + runId);
}

AgentApiClient::Response AgentApiClient::GetEvents(const std::string& sessionId,
                                                   const std::string& after) const {
    return Request("GET", "/api/v1/sessions/" + sessionId + "/events" +
                   (after.empty() ? std::string() : "?after=" + after));
}

AgentApiClient::Response AgentApiClient::CreateTeachingSession(
    const std::string& projectId, const std::string& revision,
    const std::string& learnerId, const std::string& key) const {
    Json::Value body(Json::objectValue);
    body["project_id"] = projectId;
    body["revision"] = revision;
    return Request("POST", "/api/v1/u2/sessions", &body, learnerId, key);
}

AgentApiClient::Response AgentApiClient::CreateTeachingRun(
    const std::string& sessionId, const std::string& learnerId, const std::string& key,
    const std::string& question, const std::string& jobId, const std::string& mode) const {
    if (!SafeId(sessionId) || !SafeId(jobId)) return Response{false, 0, {}, "INVALID_CLIENT_CONFIG", "Invalid session or Job ID"};
    Json::Value body(Json::objectValue);
    body["question"] = question;
    body["job_id"] = jobId;
    body["issue_id"] = "job-observation";
    // The guided report flow pauses for explicit hint/finish actions only in
    // diagnose mode; explain is a single terminal response in the U2 core.
    body["intent"] = "diagnose";
    body["mode"] = mode;
    return Request("POST", "/api/v1/u2/sessions/" + sessionId + "/runs", &body, learnerId, key);
}

AgentApiClient::Response AgentApiClient::GetTeachingRun(const std::string& runId,
                                                         const std::string& learnerId) const {
    if (!SafeId(runId)) return Response{false, 0, {}, "INVALID_CLIENT_CONFIG", "Invalid run ID"};
    return Request("GET", "/api/v1/u2/runs/" + runId, nullptr, learnerId);
}

AgentApiClient::Response AgentApiClient::TeachingAction(
    const std::string& runId, const std::string& learnerId, const std::string& kind,
    const std::string& actionId, const std::string& expectedVersion) const {
    // 只读动作白名单（u2-action.schema.json 的 kind 枚举子集）：
    // execute_plan / reference_* / prediction_* / 批准 / L4 一律不支持。
    const bool kindSupported = kind == "hint_next" || kind == "finish" ||
                               kind == "refresh_evidence" || kind == "observe_verification";
    if (!SafeId(runId) || !SafeId(actionId) || !kindSupported ||
        !CanonicalU2Version(expectedVersion)) {
        return Response{false, 0, {}, "INVALID_CLIENT_CONFIG", "Invalid teaching action"};
    }
    Json::Value body(Json::objectValue);
    body["kind"] = kind;
    body["action_id"] = actionId;
    // 合同/宿主字段名是 expected_version：旧字段名不再出现在任何请求体里。
    body["expected_version"] = expectedVersion;
    return Request("POST", "/api/v1/u2/runs/" + runId + "/actions", &body, learnerId);
}

AgentApiClient::Response AgentApiClient::RefreshTeachingEvidence(
    const std::string& runId, const std::string& learnerId, const std::string& actionId,
    const std::string& expectedVersion) const {
    return TeachingAction(runId, learnerId, "refresh_evidence", actionId, expectedVersion);
}

AgentApiClient::Response AgentApiClient::ObserveTeachingVerification(
    const std::string& runId, const std::string& learnerId, const std::string& actionId,
    const std::string& expectedVersion) const {
    return TeachingAction(runId, learnerId, "observe_verification", actionId, expectedVersion);
}

AgentApiClient::Response AgentApiClient::RecoverTeachingRun(
    const std::string& runId, const std::string& learnerId, const std::string& actionId,
    const std::string& expectedVersion, const std::string& question) const {
    if (!SafeId(runId) || !SafeId(actionId) || !CanonicalU2Version(expectedVersion)) {
        return Response{false, 0, {}, "INVALID_CLIENT_CONFIG", "Invalid teaching action"};
    }
    if (!SafeRecoverQuestion(question)) {
        // 固定文案：不把问题原文或命中的敏感片段写进错误。
        return Response{false, 0, {}, "INVALID_CLIENT_CONFIG", "Invalid recovery question"};
    }
    Json::Value body(Json::objectValue);
    body["kind"] = "recover";
    body["action_id"] = actionId;
    body["expected_version"] = expectedVersion;
    body["question"] = question;
    return Request("POST", "/api/v1/u2/runs/" + runId + "/actions", &body, learnerId);
}

AgentApiClient::Response AgentApiClient::CancelTeachingRun(
    const std::string& runId, const std::string& learnerId) const {
    if (!SafeId(runId)) return Response{false, 0, {}, "INVALID_CLIENT_CONFIG", "Invalid run ID"};
    Json::Value body(Json::objectValue);
    return Request("POST", "/api/v1/u2/runs/" + runId + "/cancel", &body, learnerId);
}

AgentApiClient::Response AgentApiClient::Request(const char* method, const std::string& path,
                                                 const Json::Value* body, const std::string& learnerId,
                                                 const std::string& idempotencyKey) const {
    Response result;
    if (config_.port <= 0 || config_.port > 65535 || config_.token.empty()) {
        result.errorCode = "INVALID_CLIENT_CONFIG";
        result.errorMessage = "Agent API client has no valid port or UI token";
        return result;
    }
    httplib::Client client(config_.host, config_.port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(30, 0);
    client.set_write_timeout(2, 0);
    httplib::Headers headers{{"Authorization", "Bearer " + config_.token}};
    if ((!learnerId.empty() && !SafeId(learnerId)) ||
        (!idempotencyKey.empty() && !SafeId(idempotencyKey))) {
        result.errorCode = "INVALID_CLIENT_CONFIG";
        result.errorMessage = "Invalid learner or idempotency identifier";
        return result;
    }
    if (!learnerId.empty()) headers.emplace("X-Learner-ID", learnerId);
    if (!idempotencyKey.empty()) headers.emplace("Idempotency-Key", idempotencyKey);
    httplib::Result response;
    if (std::string(method) == "GET") {
        response = client.Get(path.c_str(), headers);
    } else {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        const std::string payload = Json::writeString(writer, body ? *body : Json::Value(Json::objectValue));
        response = client.Post(path.c_str(), headers, payload, "application/json");
    }
    if (!response) {
        result.errorCode = "SERVICE_UNAVAILABLE";
        result.errorMessage = "Agent API connection failed";
        return result;
    }
    result.transportOk = true;
    return ParseResponse(response->status, response->body);
}

AgentApiClient::Response AgentApiClient::ParseResponse(int status, const std::string& body) const {
    Response result;
    result.transportOk = true;
    result.httpStatus = status;
    Json::CharReaderBuilder readerBuilder;
    std::string parseError;
    const std::unique_ptr<Json::CharReader> reader(readerBuilder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &result.envelope, &parseError) ||
        !result.envelope.isObject()) {
        result.errorCode = "INVALID_RESPONSE";
        result.errorMessage = "Agent API returned invalid JSON";
        return result;
    }
    if (!HasEnvelopeIdentity(result.envelope, config_.protocol)) {
        result.errorCode = "INVALID_RESPONSE";
        result.errorMessage = "Agent API response envelope is incompatible";
        return result;
    }
    const Json::Value& error = result.envelope["error"];
    if (error.isObject()) {
        result.errorCode = StringField(error, "code");
        result.errorMessage = StringField(error, "message");
        if (result.errorCode.empty()) result.errorCode = "REMOTE_ERROR";
    } else if (status < 200 || status >= 300) {
        result.errorCode = "HTTP_ERROR";
        result.errorMessage = "Agent API request failed";
    }
    return result;
}

TeachingSessionController::TeachingSessionController(Callback callback)
    : callback_(std::move(callback)) {}

TeachingSessionController::~TeachingSessionController() {
    if (worker_.valid()) worker_.wait();
}

bool TeachingSessionController::Start(std::shared_ptr<const AgentApiClient> client,
                                      std::uint64_t generation, Request request) {
    if (!client || HasTeachingRun() || request.projectId.empty() || request.revision.empty() ||
        request.question.empty() || (worker_.valid() &&
                                     worker_.wait_for(std::chrono::milliseconds(0)) !=
                                         std::future_status::ready)) {
        return false;
    }
    if (worker_.valid()) worker_.get();
    if (generation_ != generation || projectId_ != request.projectId ||
        revision_ != request.revision) sessionId_.clear();
    generation_ = generation;
    projectId_ = request.projectId;
    revision_ = request.revision;
    cancelled_ = false;
    cancelSignal_ = std::make_shared<std::atomic<bool>>(false);
    if (!request.jobId.empty()) {
        teachingClient_ = client;
        teachingLearner_ = request.learnerId;
        teachingLevel_ = request.level;
    }
    worker_ = std::async(std::launch::async,
        [client = std::move(client), generation, request = std::move(request),
         priorSession = sessionId_, cancel = cancelSignal_]() {
            Result result;
            result.generation = generation;
            result.projectId = request.projectId;
            result.revision = request.revision;
            result.question = request.question;
            result.level = request.level;
            const auto health = client->Health();
            if (!health.ok() || StringField(health.envelope["data"], "instance_id") !=
                                    client->config().instanceId ||
                StringField(health.envelope["data"], "protocol") != client->config().protocol) {
                result.state = Result::State::Failed;
                result.errorCode = health.errorCode.empty() ? "INVALID_RESPONSE" : health.errorCode;
                result.errorMessage = health.errorMessage;
                return result;
            }
            const auto capabilities = client->Capabilities();
            if (capabilities.ok() && capabilities.envelope["data"].isObject())
                result.capabilities = capabilities.envelope["data"];
            if (!request.jobId.empty()) {
                const auto& teaching = result.capabilities["teaching"];
                if (!teaching.isObject() || !teaching["available"].isBool() ||
                    !teaching["available"].asBool() ||
                    StringField(teaching, "schema_version") != "u1.u2-teaching.v1") {
                    result.state = Result::State::Failed;
                    result.errorCode = "CAPABILITY_UNAVAILABLE";
                    result.errorMessage = "当前服务未启用报告教学扩展";
                    return result;
                }
                result.teaching = true;
                const auto session = client->CreateTeachingSession(
                    request.projectId, request.revision, request.learnerId, UniqueKey("session"));
                if (!session.ok() || !session.envelope["data"].isObject()) {
                    result.state = Result::State::Failed;
                    result.errorCode = session.errorCode.empty() ? "INVALID_RESPONSE" : session.errorCode;
                    result.errorMessage = session.errorMessage;
                    return result;
                }
                result.sessionId = StringField(session.envelope["data"], "session_id");
                if (!SafeId(result.sessionId) ||
                    StringField(session.envelope["data"], "project_id") != request.projectId ||
                    StringField(session.envelope["data"], "revision") != request.revision) {
                    result.state = Result::State::Failed;
                    result.errorCode = "INVALID_RESPONSE";
                    result.errorMessage = "教学会话与当前工程不匹配";
                    return result;
                }
                const auto run = client->CreateTeachingRun(result.sessionId, request.learnerId,
                    UniqueKey("run"), request.question, request.jobId,
                    teaching["model_available"].isBool() && teaching["model_available"].asBool()
                        ? "deepseek" : "rule");
                if (!run.ok() || !run.envelope["data"].isObject()) {
                    result.state = Result::State::Failed;
                    result.errorCode = run.errorCode.empty() ? "INVALID_RESPONSE" : run.errorCode;
                    result.errorMessage = run.errorMessage;
                    return result;
                }
                result.runId = StringField(run.envelope["data"], "run_id");
                if (!SafeId(result.runId) || StringField(run.envelope["data"], "session_id") != result.sessionId) {
                    result.state = Result::State::Failed;
                    result.errorCode = "INVALID_RESPONSE";
                    result.errorMessage = "教学运行与当前会话不匹配";
                    return result;
                }
                return ReadTeachingState(client, std::move(result), request.learnerId, {}, cancel);
            }
            auto session = priorSession.empty() ? client->CreateSession(request.projectId, request.revision)
                                                : client->GetSession(priorSession);
            if (!priorSession.empty() && session.errorCode == "NOT_FOUND")
                session = client->CreateSession(request.projectId, request.revision);
            if (!session.ok() || !session.envelope["data"].isObject()) {
                result.state = Result::State::Failed;
                result.errorCode = session.errorCode.empty() ? "INVALID_RESPONSE" : session.errorCode;
                result.errorMessage = session.errorMessage;
                return result;
            }
            result.sessionId = StringField(session.envelope["data"], "session_id");
            if (result.sessionId.empty() ||
                StringField(session.envelope["data"], "project_id") != request.projectId ||
                StringField(session.envelope["data"], "revision") != request.revision) {
                result.state = Result::State::Failed;
                result.errorCode = "INVALID_RESPONSE";
                result.errorMessage = "Agent session identity does not match the active project";
                return result;
            }
            const auto run = client->CreateRun(result.sessionId, request.kind, request.level,
                                                request.question, request.sourceId, request.nodeId,
                                                request.selection);
            if (!run.ok() || !run.envelope["data"].isObject()) {
                result.state = Result::State::Failed;
                result.errorCode = run.errorCode.empty() ? "INVALID_RESPONSE" : run.errorCode;
                result.errorMessage = run.errorMessage;
                return result;
            }
            result.data = run.envelope["data"];
            result.runId = StringField(result.data, "run_id");
            const std::string state = StringField(result.data, "state");
            if (result.runId.empty() || StringField(result.data, "session_id") != result.sessionId ||
                !result.data["state_version"].isString() || !result.data["cards"].isArray()) {
                result.state = Result::State::Failed;
                result.errorCode = "INVALID_RESPONSE";
                result.errorMessage = "Agent run identity or payload is invalid";
                return result;
            }
            result.state = state == "degraded" ? Result::State::Degraded :
                           (state == "succeeded" ? Result::State::Succeeded : Result::State::Failed);
            return result;
        });
    return true;
}

void TeachingSessionController::Tick() {
    if (!worker_.valid()) {
        if (HasTeachingRun() && teachingStatus_ == "Running" &&
            std::chrono::steady_clock::now() >= nextTeachingPoll_) TeachingAction("refresh");
        return;
    }
    if (worker_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    Result result = worker_.get();
    if (cancelled_) return;
    if (result.generation != generation_ || result.projectId != projectId_ ||
        result.revision != revision_) {
        result.state = Result::State::Stale;
        return;
    }
    if (result.teaching && !result.runId.empty()) {
        teachingSessionId_ = result.sessionId;
        teachingRunId_ = result.runId;
        if (result.data.isObject()) {
            teachingVersion_ = StringField(result.data, "state_version");
            teachingStatus_ = StringField(result.data, "teaching_status");
        }
        if (!result.level.empty()) teachingLevel_ = result.level;
        nextTeachingPoll_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    } else if (!result.teaching && !result.sessionId.empty()) sessionId_ = result.sessionId;
    if (callback_) callback_(result);
    if (result.teaching && (result.state == Result::State::Succeeded ||
                            result.state == Result::State::Cancelled ||
                            (result.state == Result::State::Failed && result.data.isObject()))) {
        teachingRunId_.clear(); teachingSessionId_.clear(); teachingClient_.reset();
    }
}

bool TeachingSessionController::StartPostAction(const std::string& kind,
                                                const std::string& question) {
    if (!HasTeachingRun() || !teachingClient_ || worker_.valid()) return false;
    if (kind != "hint_next" && kind != "finish" && kind != "refresh_evidence" &&
        kind != "recover") {
        return false;
    }
    if (kind == "recover" && question.find_first_not_of(" \t\r\n") == std::string::npos) {
        // 空/纯空白草稿不构成恢复说明：本地直接拒绝，绝不发空问题。
        return false;
    }
    Result result;
    result.generation = generation_; result.projectId = projectId_; result.revision = revision_;
    result.sessionId = teachingSessionId_; result.runId = teachingRunId_;
    result.level = teachingLevel_; result.teaching = true;
    const auto client = teachingClient_;
    const auto learner = teachingLearner_;
    const auto version = teachingVersion_;
    const auto runId = teachingRunId_;
    const auto actionId = UniqueKey("action");
    const auto cancel = cancelSignal_;
    worker_ = std::async(std::launch::async,
        [client, learner, version, runId, kind, question, actionId, result, cancel]() mutable {
            // 只读动作：refresh_evidence 与 recover 都只请求"重读/恢复"，不执行工具。
            const auto ack = kind == "recover"
                ? client->RecoverTeachingRun(runId, learner, actionId, version, question)
                : kind == "refresh_evidence"
                    ? client->RefreshTeachingEvidence(runId, learner, actionId, version)
                    : client->TeachingAction(runId, learner, kind, actionId, version);
            if (!ack.ok() || !ack.envelope["data"].isObject() ||
                !ack.envelope["data"]["accepted"].isBool() ||
                !ack.envelope["data"]["accepted"].asBool()) {
                result.state = Result::State::Failed;
                result.errorCode = ack.errorCode.empty() ? "INVALID_RESPONSE" : ack.errorCode;
                result.errorMessage = ack.errorMessage;
                return result;
            }
            // accepted 之后仍以 GET 读状态：动作回执不是终态。
            return ReadTeachingState(client, std::move(result), learner, version, cancel);
        });
    return true;
}

bool TeachingSessionController::RefreshEvidence() {
    return StartPostAction("refresh_evidence", std::string());
}

bool TeachingSessionController::RecoverTeaching(const std::string& question) {
    // 恢复入口只在后端报告 RecoveryRequired 时可用；其余状态一律 fail-closed。
    if (!RecoveryRequired()) return false;
    return StartPostAction("recover", question);
}

bool TeachingSessionController::TeachingAction(const std::string& action) {
    if (!HasTeachingRun() || !teachingClient_ || worker_.valid() ||
        (action != "hint_next" && action != "finish" && action != "cancel" && action != "refresh")) return false;
    if (action == "hint_next" || action == "finish") return StartPostAction(action, std::string());
    Result result;
    result.generation = generation_; result.projectId = projectId_; result.revision = revision_;
    result.sessionId = teachingSessionId_; result.runId = teachingRunId_;
    result.level = teachingLevel_; result.teaching = true;
    const auto client = teachingClient_;
    const auto learner = teachingLearner_;
    const auto version = teachingVersion_;
    const auto cancel = cancelSignal_;
    worker_ = std::async(std::launch::async, [client, learner, version, action, result, cancel]() mutable {
        if (action == "cancel") {
            const auto ack = client->CancelTeachingRun(result.runId, learner);
            if (!ack.ok() || !ack.envelope["data"].isObject() || !ack.envelope["data"]["accepted"].isBool() ||
                !ack.envelope["data"]["accepted"].asBool()) {
                result.state = Result::State::Failed;
                result.errorCode = ack.errorCode.empty() ? "INVALID_RESPONSE" : ack.errorCode;
                result.errorMessage = ack.errorMessage;
                return result;
            }
            return ReadTeachingState(client, std::move(result), learner, std::string(), nullptr);
        }
        // action == "refresh"：仍是本地 GET 轮询，绝不 POST refresh_evidence。
        return ReadTeachingState(client, std::move(result), learner, std::string(), cancel);
    });
    return true;
}

void TeachingSessionController::Cancel() {
    cancelled_ = true;
    if (cancelSignal_) cancelSignal_->store(true);
    ++generation_;
    if (callback_) {
        Result result;
        result.state = Result::State::Cancelled;
        result.generation = generation_;
        callback_(result);
    }
}

void TeachingSessionController::InvalidateGeneration(std::uint64_t generation) {
    cancelled_ = true;
    if (cancelSignal_) cancelSignal_->store(true);
    generation_ = generation;
    sessionId_.clear();
    teachingRunId_.clear(); teachingSessionId_.clear(); teachingClient_.reset();
}
