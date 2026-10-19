#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

// Small, contract-aware client for the authenticated local Agent API.  It is
// deliberately independent of wx so the same client can be exercised by
// native contract tests and used by the desktop controller.
class AgentApiClient final {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 0;
        std::string token;
        std::string protocol = "edu.api.v1";
        std::string instanceId;
    };

    struct Response {
        bool transportOk = false;
        int httpStatus = 0;
        Json::Value envelope;
        std::string errorCode;
        std::string errorMessage;

        bool ok() const { return transportOk && httpStatus >= 200 && httpStatus < 300 &&
                                 errorCode.empty(); }
    };

    explicit AgentApiClient(Config config);

    Response Health() const;
    Response Capabilities() const;
    Response CreateSession(const std::string& projectId, const std::string& revision,
                           const std::string& top = {},
                           const Json::Value& target = Json::Value(Json::objectValue),
                           const std::string& locale = "zh-CN") const;
    Response CreateRun(const std::string& sessionId, const std::string& kind,
                       const std::string& level, const std::string& question,
                       const std::string& sourceId = "ui-selection",
                       const std::string& nodeId = {}) const;
    Response GetSession(const std::string& sessionId) const;
    Response GetRun(const std::string& runId) const;
    Response GetEvents(const std::string& sessionId, const std::string& after = {}) const;

    const Config& config() const { return config_; }

private:
    Response Request(const char* method, const std::string& path,
                     const Json::Value* body = nullptr) const;
    Response ParseResponse(int status, const std::string& body) const;

    Config config_;
};

class TeachingSessionController final {
public:
    struct Request {
        std::string projectId;
        std::string revision;
        std::string question;
        std::string kind = "explain";
        std::string level = "L1";
        std::string sourceId = "ui-selection";
        std::string nodeId;
    };

    struct Result {
        enum class State { Succeeded, Degraded, Failed, Cancelled, Stale };
        State state = State::Failed;
        std::uint64_t generation = 0;
        std::string projectId;
        std::string revision;
        std::string sessionId;
        std::string runId;
        Json::Value data;
        std::string errorCode;
        std::string errorMessage;
    };

    using Callback = std::function<void(const Result&)>;

    explicit TeachingSessionController(Callback callback = {});
    ~TeachingSessionController();

    TeachingSessionController(const TeachingSessionController&) = delete;
    TeachingSessionController& operator=(const TeachingSessionController&) = delete;

    // Starts one authenticated create-session -> create-run transaction.  A
    // second request is rejected while the previous worker is still draining.
    bool Start(std::shared_ptr<const AgentApiClient> client, std::uint64_t generation,
               Request request);
    // Must be called from the UI timer.  Completion is delivered only when its
    // generation, project and revision still match the active request.
    void Tick();
    // Invalidates any in-flight result.  The worker may finish, but its result
    // cannot reach the current UI generation.
    void Cancel();
    void InvalidateGeneration(std::uint64_t generation);
    bool busy() const { return worker_.valid(); }
    std::uint64_t generation() const { return generation_; }

private:
    std::future<Result> worker_;
    Callback callback_;
    std::uint64_t generation_ = 0;
    std::string projectId_;
    std::string revision_;
    bool cancelled_ = false;
};
