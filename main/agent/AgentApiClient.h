#pragma once

#include <json/json.h>

#include "agent/gui/AgentSelectionContext.h"

#include <cstdint>
#include <atomic>
#include <chrono>
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
                       const std::string& nodeId = {},
                       const agent::gui::selection::Resolved& selection = {}) const;
    Response GetSession(const std::string& sessionId) const;
    Response GetRun(const std::string& runId) const;
    Response GetEvents(const std::string& sessionId, const std::string& after = {}) const;
    Response CreateTeachingSession(const std::string& projectId, const std::string& revision,
                                   const std::string& learnerId, const std::string& key) const;
    Response CreateTeachingRun(const std::string& sessionId, const std::string& learnerId,
                               const std::string& key, const std::string& question,
                               const std::string& jobId, const std::string& mode) const;
    Response GetTeachingRun(const std::string& runId, const std::string& learnerId) const;
    /// U2 只读动作（contracts/edu-agent/v1/schemas/u2-action.schema.json）：
    ///   * 仅接受 hint_next / finish / refresh_evidence / observe_verification；
    ///   * 请求体严格为 {kind, action_id, expected_version}（`expected_version` 必须是
    ///     canonical decimal ^(0|[1-9][0-9]{0,18})$ 且 < 2^63，与 host `_version` 一致）；
    ///   * execute_plan / reference_* / prediction_* / 批准与 L4 一律 fail-closed。
    Response TeachingAction(const std::string& runId, const std::string& learnerId,
                            const std::string& kind, const std::string& actionId,
                            const std::string& expectedVersion) const;
    /// 显式只读动作辅助：refresh_evidence（重新读取证据，不执行工具）。
    Response RefreshTeachingEvidence(const std::string& runId, const std::string& learnerId,
                                     const std::string& actionId,
                                     const std::string& expectedVersion) const;
    /// 显式只读动作辅助：observe_verification（仅观察运行记录，不执行工具）。
    Response ObserveTeachingVerification(const std::string& runId, const std::string& learnerId,
                                         const std::string& actionId,
                                         const std::string& expectedVersion) const;
    /// U2 recover：{kind:"recover", action_id, expected_version, question}。
    /// question 必须 1..4096 UTF-8 字节，且不含路径、HTML、脚本/本地协议或凭据样式；
    /// 任一参数非法都 fail-closed（不发请求、错误不回显输入）。
    Response RecoverTeachingRun(const std::string& runId, const std::string& learnerId,
                                const std::string& actionId, const std::string& expectedVersion,
                                const std::string& question) const;
    Response CancelTeachingRun(const std::string& runId, const std::string& learnerId) const;

    const Config& config() const { return config_; }

private:
    Response Request(const char* method, const std::string& path,
                     const Json::Value* body = nullptr, const std::string& learnerId = {},
                     const std::string& idempotencyKey = {}) const;
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
        std::string jobId;
        std::string learnerId = "local-learner";
        // 宿主在 UI 线程抽取并投影过的可信选择证据（opaque ID + 有界片段）。
        // 默认空 = 未选中，请求保持兼容的空 selection。
        agent::gui::selection::Resolved selection;
    };

    struct Result {
        enum class State { Succeeded, Degraded, Waiting, Pending, Failed, Cancelled, Stale };
        State state = State::Failed;
        std::uint64_t generation = 0;
        std::string projectId;
        std::string revision;
        std::string question;
        std::string level;
        std::string sessionId;
        std::string runId;
        Json::Value data;
        Json::Value capabilities;
        bool teaching = false;
        std::string errorCode;
        std::string errorMessage;
    };

    using Callback = std::function<void(const Result&)>;

    explicit TeachingSessionController(Callback callback = {});
    ~TeachingSessionController();

    TeachingSessionController(const TeachingSessionController&) = delete;
    TeachingSessionController& operator=(const TeachingSessionController&) = delete;

    // 开始一次已认证的 create-session -> create-run 事务。上一个 worker 尚未结束时拒绝。
    bool Start(std::shared_ptr<const AgentApiClient> client, std::uint64_t generation,
               Request request);
    // 必须从 UI 定时器调用。只有 generation/project/revision 仍匹配当前请求时才交付结果。
    void Tick();
    // 作废任何在途结果。worker 可以完成，但结果不能进入当前 UI generation。
    void Cancel();
    /// 教学动作：hint_next / finish / cancel / refresh。
    ///   * refresh 仍是本地 GET 轮询（读取状态），绝不 POST refresh_evidence；
    ///   * hint_next / finish 走 POST，accepted 后再 GET 读状态。
    bool TeachingAction(const std::string& action);
    /// 只读动作：POST refresh_evidence → accepted 后 GET 读状态；不执行任何工具。
    /// 未接线到 GUI 时由工作台/按钮可见性控制（见 vm::ProjectRecoveryActions）。
    bool RefreshEvidence();
    /// 恢复教学：**仅当**后端 teaching_status == "RecoveryRequired" 时允许；
    /// 空白草稿直接拒绝（不发请求），内容边界仍由 AgentApiClient 最终把关。
    bool RecoverTeaching(const std::string& question);
    /// 当前教学状态（wire 原文；空 = 未知）。GUI 用它判断 RecoveryRequired。
    const std::string& TeachingStatus() const { return teachingStatus_; }
    /// 当前 state_version 原文（空 = 未知）。
    const std::string& TeachingVersion() const { return teachingVersion_; }
    bool RecoveryRequired() const { return teachingStatus_ == "RecoveryRequired"; }
    bool HasTeachingRun() const { return !teachingRunId_.empty(); }
    void InvalidateGeneration(std::uint64_t generation);
    bool busy() const { return worker_.valid(); }
    std::uint64_t generation() const { return generation_; }

private:
    /// 统一 POST 动作（refresh_evidence / recover）：accepted 后 GET 读状态。
    /// 空 question 的 recover 在本地直接拒绝，不进入网络。
    bool StartPostAction(const std::string& kind, const std::string& question);
    std::future<Result> worker_;
    std::shared_ptr<std::atomic<bool>> cancelSignal_;
    Callback callback_;
    std::uint64_t generation_ = 0;
    std::string projectId_;
    std::string revision_;
    std::string sessionId_;
    std::string teachingSessionId_;
    std::string teachingRunId_;
    std::string teachingVersion_;
    std::string teachingLearner_;
    std::string teachingStatus_;
    std::string teachingLevel_;
    std::shared_ptr<const AgentApiClient> teachingClient_;
    std::chrono::steady_clock::time_point nextTeachingPoll_{};
    bool cancelled_ = false;
};
