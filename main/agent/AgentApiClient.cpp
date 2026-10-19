#include "agent/AgentApiClient.h"

#include <httplib.h>

#include <chrono>
#include <future>
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

Json::Value MakeSelection(const std::string& question, const std::string& sourceId,
                          const std::string& nodeId) {
    Json::Value selection(Json::objectValue);
    selection["source_id"] = sourceId;
    if (!nodeId.empty()) selection["node_id"] = nodeId;
    selection["text"] = question;
    return selection;
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
    const std::string& question, const std::string& sourceId, const std::string& nodeId) const {
    Json::Value body(Json::objectValue);
    body["kind"] = kind;
    body["level"] = level;
    body["selection"] = MakeSelection(question, sourceId, nodeId);
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

AgentApiClient::Response AgentApiClient::Request(const char* method, const std::string& path,
                                                 const Json::Value* body) const {
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
    if (!client || request.projectId.empty() || request.revision.empty() ||
        request.question.empty() || (worker_.valid() &&
                                     worker_.wait_for(std::chrono::milliseconds(0)) !=
                                         std::future_status::ready)) {
        return false;
    }
    if (worker_.valid()) worker_.get();
    generation_ = generation;
    projectId_ = request.projectId;
    revision_ = request.revision;
    cancelled_ = false;
    worker_ = std::async(std::launch::async,
        [client = std::move(client), generation, request = std::move(request)]() {
            Result result;
            result.generation = generation;
            result.projectId = request.projectId;
            result.revision = request.revision;
            const auto health = client->Health();
            if (!health.ok()) {
                result.state = Result::State::Failed;
                result.errorCode = health.errorCode;
                result.errorMessage = health.errorMessage;
                return result;
            }
            const auto session = client->CreateSession(request.projectId, request.revision);
            if (!session.ok() || !session.envelope["data"].isObject()) {
                result.state = Result::State::Failed;
                result.errorCode = session.errorCode.empty() ? "INVALID_RESPONSE" : session.errorCode;
                result.errorMessage = session.errorMessage;
                return result;
            }
            result.sessionId = StringField(session.envelope["data"], "session_id");
            const auto run = client->CreateRun(result.sessionId, request.kind, request.level,
                                                request.question, request.sourceId, request.nodeId);
            if (!run.ok() || !run.envelope["data"].isObject()) {
                result.state = Result::State::Failed;
                result.errorCode = run.errorCode.empty() ? "INVALID_RESPONSE" : run.errorCode;
                result.errorMessage = run.errorMessage;
                return result;
            }
            result.data = run.envelope["data"];
            result.runId = StringField(result.data, "run_id");
            const std::string state = StringField(result.data, "state");
            result.state = state == "degraded" ? Result::State::Degraded :
                           (state == "succeeded" ? Result::State::Succeeded : Result::State::Failed);
            return result;
        });
    return true;
}

void TeachingSessionController::Tick() {
    if (!worker_.valid() || worker_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    Result result = worker_.get();
    if (cancelled_) return;
    if (result.generation != generation_ || result.projectId != projectId_ ||
        result.revision != revision_) {
        result.state = Result::State::Stale;
        return;
    }
    if (callback_) callback_(result);
}

void TeachingSessionController::Cancel() {
    cancelled_ = true;
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
    generation_ = generation;
}
