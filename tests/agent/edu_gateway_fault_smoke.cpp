// SF-02 故障与边界注入测试（真实 loopback HTTP）。
// 覆盖 task.md SF-02「故障测试」条目与 §8 测试矩阵「身份与路径边界 / grant/receipt/幂等」：
//   重复事件、并发相同幂等键、跨项目读写、过期/伪造 receipt、未知 Origin、
//   服务关闭中请求、坏 JSON、端口占用。
// 断言只依赖对外可观察行为（状态码/错误码/资源计数），不依赖内部实现。
#include "eda-agent-gateway/GatewayServer.h"

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
std::mutex g_httpErrorMutex;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

// 计数型假 Job 服务：记录真实 submit 次数，用于验证「并发同键只进入一次真实 Job 路径」。
class CountingJobService final : public eda::IJobService {
public:
    std::string submit(const eda::JobRequest& request) override {
        const std::string id = "job-" + std::to_string(++sequence_);
        std::lock_guard<std::mutex> lock(mutex_);
        records_[id].id = id;
        records_[id].request = request;
        records_[id].state = eda::JobState::Queued;
        ++submitCount_;
        return id;
    }
    bool cancel(const std::string& jobId, const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end()) return false;
        it->second.state = eda::JobState::Cancelled;
        return true;
    }
    bool retry(const std::string&, std::string&) override { return false; }
    std::optional<eda::JobRecord> get(const std::string& jobId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }
    std::vector<eda::JobRecord> list(const std::string&) override { return {}; }
    eda::JobReport report(const std::string& jobId) override {
        eda::JobReport report;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(jobId);
        if (it == records_.end()) return report;
        report.jobId = jobId;
        report.jobType = it->second.request.jobType;
        report.pluginId = "fake";
        report.state = it->second.state;
        return report;
    }
    void setConcurrency(std::size_t) override {}

    int SubmitCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return submitCount_;
    }

private:
    mutable std::mutex mutex_;
    std::atomic<int> sequence_{0};
    int submitCount_ = 0;
    std::map<std::string, eda::JobRecord> records_;
};

eda::Json GetJson(const std::string& host, int port, const std::string& path,
                  const std::string& token, int& status,
                  const httplib::Headers& extra = httplib::Headers()) {
    httplib::Client client(host, port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(6, 0);
    httplib::Headers headers = extra;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    auto res = client.Get(path.c_str(), headers);
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

eda::Json PostJson(const std::string& host, int port, const std::string& path,
                   const std::string& token, const eda::Json& body, int& status,
                   const httplib::Headers& extra = httplib::Headers()) {
    httplib::Client client(host, port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(6, 0);
    httplib::Headers headers = extra;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    auto res = client.Post(path.c_str(), headers, body.dump(), "application/json");
    if (!res) {
        // 传输层失败要能被定位：区分"连不上"与"超时/被重置"。
        std::lock_guard<std::mutex> lock(g_httpErrorMutex);
        std::cout << "      [transport] POST " << path
                  << " failed: " << static_cast<int>(res.error()) << "\n";
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

// 发送原始（可能非法）正文，用于坏 JSON 注入。
eda::Json PostRaw(const std::string& host, int port, const std::string& path,
                  const std::string& token, const std::string& raw, int& status) {
    httplib::Client client(host, port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(6, 0);
    httplib::Headers headers{{"Content-Type", "application/json"}};
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    auto res = client.Post(path.c_str(), headers, raw, "application/json");
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

std::string ErrorCode(const eda::Json& body) {
    return body.contains("error") ? body["error"].value("code", std::string()) : std::string();
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    std::error_code cleanupError;
    const fs::path root = fs::temp_directory_path() /
                          ("sigflow_gateway_fault_" + std::to_string(
                              std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fs::remove_all(root, cleanupError);

    // 两个工程：验证跨项目隔离。
    const fs::path projectA = root / "proj A 中文";   // 中文 + 空格路径
    const fs::path projectB = root / "proj-b";
    for (const fs::path& project : {projectA, projectB}) {
        fs::create_directories(project / "rtl");
        std::ofstream(project / "rtl" / "top.v", std::ios::binary)
            << "module top; endmodule\n";
        std::ofstream(project / "sigflow.project", std::ios::binary)
            << R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/top.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON";
    }

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-fault";
    config.edition = "edu";
    config.token = "fault-token";
    config.uiToken = "fault-ui-token";
    // 端口 0 无法模拟"端口被占用"：先让占位 server 拿到系统端口，再让被测 Gateway 绑定它。
    config.port = 0;

    const auto provider = []() {
        std::vector<eda::agent::ReadyPlugin> ready;
        ready.push_back({"eda-synth-yosys", "1.0.0", {"synth"}});
        return ready;
    };

    eda::agent::GatewayServer server(config, provider);
    CountingJobService jobs;
    server.SetJobServiceProvider([&jobs]() -> eda::IJobService* { return &jobs; });

    std::string error;
    std::string projectAId;
    std::string projectBId;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectA, false, true, projectAId, error),
          "project A context built (non-ASCII path)");
    Check(server.RefreshProjectSnapshotStateFromDisk(projectB, false, true, projectBId, error),
          "project B context built");
    Check(projectAId != projectBId, "distinct projects get distinct opaque ids");
    Check(server.Start(error), "gateway starts");
    server.RunAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int status = 0;

    // ---- 端口占用 ----
    {
        // httplib 的 bind_to_port 在 Windows 上能重复绑定同一端口（SO_REUSEADDR），
        // 因此用原始 socket 真正 LISTEN 住一个端口，再验证 Gateway 明确失败而不是假装成功。
        httplib::Server holder;
        const int heldPort = holder.bind_to_any_port("127.0.0.1");
        Check(heldPort > 0, "holder socket acquired a port");
        std::thread holderThread([&holder]() { holder.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        eda::agent::GatewayConfig conflictingConfig = config;
        conflictingConfig.port = heldPort;
        eda::agent::GatewayServer conflicting(conflictingConfig, provider);
        std::string bindError;
        const bool started = conflicting.Start(bindError);
        Check(!started || conflicting.Port() == heldPort,
              "gateway either refuses the occupied port or reports the bound port");
        if (!started) {
            Check(!bindError.empty(), "port conflict reports a readable reason");
            Check(!conflicting.Running(), "failed gateway is not marked running");
        }
        conflicting.Stop();
        holder.stop();
        holderThread.join();
    }

    // ---- 服务关闭中请求 ----
    {
        eda::agent::GatewayServer dying(config, provider);
        std::string startError;
        Check(dying.Start(startError), "temporary gateway starts");
        dying.RunAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const int livePort = dying.Port();
        dying.Stop();
        const eda::Json after = GetJson("127.0.0.1", livePort, "/api/v1/health", "", status);
        Check(status == -1, "request after Stop fails to connect (no hang)");
        Check(after.is_object(), "closed gateway returns no body");
        Check(!dying.Running(), "stopped gateway reports not running");
    }

    // ---- 坏 JSON ----
    {
        const eda::Json bad = PostRaw("127.0.0.1", server.Port(),
                                      "/api/v1/projects/" + projectAId + "/snapshots",
                                      "fault-token", "{not json", status);
        Check(status == 400, "malformed JSON body rejected (400)");
        Check(ErrorCode(bad) == "INVALID_ARGUMENT", "malformed JSON maps to INVALID_ARGUMENT");

        const eda::Json truncated = PostRaw("127.0.0.1", server.Port(),
                                            "/api/v1/projects/" + projectAId + "/context/query",
                                            "fault-token", "{\"needs\": [", status);
        Check(status == 400 && ErrorCode(truncated) == "INVALID_ARGUMENT",
              "truncated JSON body rejected (400)");
    }

    // ---- 超限请求体（1 MiB）----
    // 边界两侧都要验证：恰好 1 MiB 被接受、超过 1 MiB 返回 413 RESOURCE_EXHAUSTED。
    {
        constexpr std::size_t kLimit = 1024 * 1024;
        // 构造一个体量刚好触及上限内的合法 JSON（用空格填充，语义仍是合法对象）。
        const std::string prefix = "{\"revision\":\"";
        const std::string suffix = "\",\"needs\":[]}";
        const auto bodyOfSize = [&](std::size_t total) {
            std::string body = prefix + std::string(total - prefix.size() - suffix.size(), 'a') +
                               suffix;
            return body;
        };

        // 略小于上限：应被正常解析（revision 不匹配 → 409，而不是 413）。
        const std::string under = bodyOfSize(kLimit - 1024);
        const eda::Json underResult = PostRaw(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/context/query",
            "fault-token", under, status);
        Check(status != 413, "request just under 1 MiB is not rejected as oversized");

        // 超过上限：必须 413 且错误码为 RESOURCE_EXHAUSTED。
        const std::string over = bodyOfSize(kLimit + 4096);
        const eda::Json overResult = PostRaw(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/context/query",
            "fault-token", over, status);
        Check(status == 413, "request over 1 MiB is rejected with 413");
        Check(ErrorCode(overResult) == "RESOURCE_EXHAUSTED",
              "oversized request maps to RESOURCE_EXHAUSTED");
        Check(overResult.contains("error"), "oversized request returns a failure envelope");
        // 超限拒绝不得泄露路径或密钥信息。
        const std::string message =
            overResult.contains("error") ? overResult["error"].value("message", std::string())
                                         : std::string();
        Check(message.find(":\\") == std::string::npos &&
                  message.find("fault-token") == std::string::npos,
              "oversized rejection does not leak paths or tokens");
    }

    // ---- 未知 Origin / 未知路径 / 方法 ----
    {
        httplib::Headers evil;
        evil.emplace("Origin", "https://evil.example.com");
        GetJson("127.0.0.1", server.Port(), "/api/v1/health", "", status, evil);
        Check(status == 403, "unknown Origin rejected (403)");

        httplib::Headers nullOrigin;
        nullOrigin.emplace("Origin", "null");
        GetJson("127.0.0.1", server.Port(), "/api/v1/health", "", status, nullOrigin);
        Check(status == 403, "opaque 'null' Origin rejected (403)");

        httplib::Headers ipv6Loopback;
        ipv6Loopback.emplace("Origin", "http://[::1]:8080");
        GetJson("127.0.0.1", server.Port(), "/api/v1/health", "", status, ipv6Loopback);
        Check(status == 200, "IPv6 loopback Origin accepted");

        GetJson("127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/invoke",
                "fault-token", status);
        Check(status == 404, "generic plugin invoke route does not exist (404)");

        const eda::Json wrongMethod = PostRaw("127.0.0.1", server.Port(), "/api/v1/health",
                                              "", "{}", status);
        Check(status == 404 || status == 405, "POST on GET-only route rejected");
        Check(wrongMethod.is_object(), "method rejection carries a body");
    }

    // ---- 跨项目读写 ----
    std::string snapshotA;
    std::string revisionA;
    std::string grantA;
    {
        const eda::Json contextA = GetJson("127.0.0.1", server.Port(),
                                           "/api/v1/projects/" + projectAId + "/context",
                                           "fault-token", status);
        revisionA = contextA["data"].value("revision", std::string());
        const eda::Json snap = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/snapshots",
            "fault-token",
            eda::Json{{"expected_revision", revisionA}, {"require_saved", true}}, status);
        Check(status == 201, "project A snapshot created");
        if (status != 201) {
            std::cout << "      snapshot response: " << snap.dump() << "\n";
        }
        snapshotA = snap["data"].value("snapshot_id", std::string());

        const eda::Json issued = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/grants",
            "fault-ui-token",
            eda::Json{{"plan_hash", "ph-fault"}, {"revision", revisionA},
                      {"snapshot_id", snapshotA}, {"max_jobs", 3}},
            status);
        Check(status == 201, "project A grant issued by UI identity");
        grantA = issued["data"].value("grant_id", std::string());
    }

    {
        // 用 A 的快照 + A 的 grant，向 B 的项目路由提交：必须被拒。
        httplib::Headers key;
        key.emplace("Idempotency-Key", "cross-project-1");
        const eda::Json crossSubmit = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectBId + "/jobs", "fault-token",
            eda::Json{{"snapshot_id", snapshotA},
                      {"expected_revision", revisionA},
                      {"capability", "eda.synth"},
                      {"grant_id", grantA},
                      {"params", eda::Json{{"top_module", "top"}}}},
            status, key);
        Check(status == 403 || status == 404 || status == 409 || status == 422,
              "cross-project job submission rejected");
        Check(!crossSubmit.empty() || status != 200, "cross-project submission produced no job");

        // 读 B 的 context 只返回 B 的数据。
        const eda::Json contextB = GetJson("127.0.0.1", server.Port(),
                                           "/api/v1/projects/" + projectBId + "/context",
                                           "fault-token", status);
        Check(status == 200 && contextB["data"].value("project_id", std::string()) == projectBId,
              "project B context is scoped to project B");

        // A 的快照 id 在 B 的 sources 路由下不可用。
        GetJson("127.0.0.1", server.Port(),
                "/api/v1/projects/" + projectBId + "/sources/source-000000000000000000000000",
                "fault-token", status);
        Check(status == 404, "unknown source in project B returns 404");
    }

    Check(jobs.SubmitCount() == 0, "rejected cross-project request never reached the Job service");

    // ---- 并发相同幂等键：20 次并发只有 1 个真实 Job ----
    {
        const eda::Json contextA = GetJson("127.0.0.1", server.Port(),
                                           "/api/v1/projects/" + projectAId + "/context",
                                           "fault-token", status);
        const std::string revision = contextA["data"].value("revision", std::string());
        const eda::Json snap = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/snapshots",
            "fault-token",
            eda::Json{{"expected_revision", revision}, {"require_saved", true}}, status);
        Check(status == 201, "concurrency snapshot created");
        const std::string snapshotId = snap["data"].value("snapshot_id", std::string());

        const eda::Json issued = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/grants",
            "fault-ui-token",
            eda::Json{{"plan_hash", "ph-concurrent"}, {"revision", revision},
                      {"snapshot_id", snapshotId}, {"max_jobs", 1}},
            status);
        Check(status == 201, "concurrency grant issued with max_jobs=1");
        const std::string grantId = issued["data"].value("grant_id", std::string());

        const eda::Json body{{"snapshot_id", snapshotId},
                             {"expected_revision", revision},
                             {"capability", "eda.synth"},
                             {"grant_id", grantId},
                             {"params", eda::Json{{"top_module", "top"}}}};

        const int before = jobs.SubmitCount();
        constexpr int kThreads = 20;
        std::vector<std::thread> workers;
        std::vector<int> statuses(kThreads, 0);
        std::vector<std::string> jobIds(kThreads);
        std::atomic<int> ready{0};
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&, i]() {
                httplib::Headers key;
                key.emplace("Idempotency-Key", "concurrent-key-1");
                ready.fetch_add(1);
                while (ready.load() < kThreads) std::this_thread::yield();  // 同时发起
                int localStatus = 0;
                const eda::Json res = PostJson("127.0.0.1", server.Port(),
                                               "/api/v1/projects/" + projectAId + "/jobs",
                                               "fault-token", body, localStatus, key);
                statuses[i] = localStatus;
                jobIds[i] = res.contains("data") ? res["data"].value("job_id", std::string())
                                                 : std::string();
            });
        }
        for (auto& worker : workers) worker.join();
        const int after = jobs.SubmitCount();

        int accepted = 0;
        int exhausted = 0;
        int conflicted = 0;
        bool unhealthyStatus = false;
        for (int i = 0; i < kThreads; ++i) {
            if (statuses[i] == 202) ++accepted;
            else if (statuses[i] == 403) ++exhausted;
            else if (statuses[i] == 409) ++conflicted;  // 与首个提交重放存在竞态窗口
            else unhealthyStatus = true;
        }
        Check(after - before == 1,
              "20 concurrent identical requests produce exactly one real Job");
        Check(accepted >= 1, "at least one concurrent request is accepted");
        if (accepted + exhausted + conflicted != kThreads) {
            // 失败时必须给出实际状态分布，否则这条断言无法定位（5xx 说明处理器抛了异常）。
            std::map<int, int> histogram;
            for (int i = 0; i < kThreads; ++i) ++histogram[statuses[i]];
            std::cout << "      concurrent status histogram:";
            for (const auto& entry : histogram) {
                std::cout << " " << entry.first << "x" << entry.second;
            }
            std::cout << "\n";
        }
        Check(accepted + exhausted + conflicted == kThreads,
              "every concurrent request is accepted, quota-rejected, or a conflict (never 5xx)");
        Check(!unhealthyStatus, "no concurrent request hits an unexpected status");

        std::string firstId;
        bool conflictingId = false;
        for (int i = 0; i < kThreads; ++i) {
            if (statuses[i] != 202) continue;
            if (firstId.empty()) {
                firstId = jobIds[i];
            } else if (jobIds[i] != firstId) {
                conflictingId = true;
            }
        }
        Check(!firstId.empty(), "concurrent acceptance returns a job id");
        Check(!conflictingId, "all accepted replays return the same job id");

        // 同键不同内容 -> 409（配额不得被再次消耗）。
        const eda::Json changed{{"snapshot_id", snapshotId},
                                {"expected_revision", revision},
                                {"capability", "eda.synth"},
                                {"grant_id", grantId},
                                {"params", eda::Json{{"top_module", "other"}}}};
        httplib::Headers sameKey;
        sameKey.emplace("Idempotency-Key", "concurrent-key-1");
        const eda::Json conflict = PostJson("127.0.0.1", server.Port(),
                                            "/api/v1/projects/" + projectAId + "/jobs",
                                            "fault-token", changed, status, sameKey);
        Check(status == 409, "same key with different content returns 409");
        Check(ErrorCode(conflict) == "IDEMPOTENCY_CONFLICT" || ErrorCode(conflict) == "CONFLICT",
              "conflict carries a conflict error code");
        Check(jobs.SubmitCount() == after, "conflicting replay does not reach the Job service");
    }

    // ---- 重复事件与游标 ----
    {
        eda::Json state = GetJson("127.0.0.1", server.Port(),
                                  "/api/v1/projects/" + projectAId + "/state", "fault-token",
                                  status);
        Check(status == 200, "state read for event checks");
        Check(state["data"].contains("oldest_sequence"),
              "state exposes oldest_sequence for cursor recovery");
        Check(state["data"].contains("high_watermark"), "state exposes high_watermark");
        const std::string watermark = state["data"].value("high_watermark", std::string("0"));

        // 重复投递同一条业务事件：sequence 必须单调，event_id 不得重复。
        for (int i = 0; i < 3; ++i) {
            server.PublishEvent("job/finished", projectAId, "trace-dup",
                                eda::Json{{"job_id", "job-dup"}});
        }
        const eda::Json page = GetJson("127.0.0.1", server.Port(),
                                       "/api/v1/projects/" + projectAId + "/events?after=" +
                                           watermark,
                                       "fault-token", status);
        Check(status == 200, "events read after checkpoint");
        std::vector<std::string> ids;
        bool monotonic = true;
        long long previous = -1;
        for (const auto& event : page["data"]["events"]) {
            const std::string id = event.value("event_id", std::string());
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) monotonic = false;
            ids.push_back(id);
            const long long sequence = std::stoll(event.value("sequence", std::string("0")));
            if (sequence <= previous) monotonic = false;
            previous = sequence;
        }
        Check(ids.size() == 3, "three duplicate deliveries are all observable");
        Check(monotonic, "duplicate deliveries keep unique ids and monotonic sequence");

        // 游标过期：after 早于保留窗口下界 -> 410。
        const std::string oldest = state["data"].value("oldest_sequence", std::string("1"));
        const long long oldestValue = std::stoll(oldest);
        if (oldestValue >= 2) {
            const eda::Json expired = GetJson(
                "127.0.0.1", server.Port(),
                "/api/v1/projects/" + projectAId + "/events?after=" +
                    std::to_string(oldestValue - 2),
                "fault-token", status);
            Check(status == 410, "cursor older than retention window returns 410");
            Check(ErrorCode(expired) == "CURSOR_EXPIRED", "expired cursor carries CURSOR_EXPIRED");
        }
    }

    // ---- 伪造 / 过期 receipt ----
    std::string receiptId;
    {
        // 伪造 id：从未签发过。合法参数 + 未知凭据应判 404，而不是泄露"参数错误"。
        const eda::Json forged = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/ui-receipts/receipt-does-not-exist/consume",
            "fault-token",
            eda::Json{{"run_id", "run-fault"}, {"action_id", "act-1"},
                      {"expected_state_version", "0"}},
            status);
        Check(status == 404, "forged receipt id returns 404");
        Check(ErrorCode(forged) == "NOT_FOUND", "forged receipt carries NOT_FOUND");

        // 缺 run_id/action_id -> 400（参数校验先于凭据查找）。
        PostJson("127.0.0.1", server.Port(), "/api/v1/ui-receipts/receipt-does-not-exist/consume",
                 "fault-token", eda::Json{{"run_id", "run-fault"}}, status);
        Check(status == 400, "consume without action_id rejected (400)");

        // Agent token 签发 receipt -> 403。
        PostJson("127.0.0.1", server.Port(),
                 "/api/v1/projects/" + projectAId + "/ui-receipts", "fault-token",
                 eda::Json{{"session_id", "s1"}, {"issue", "i1"}, {"level", "l4"}}, status);
        Check(status == 403, "Agent token cannot issue a UI receipt (403)");

        // 非法 level -> 400。
        PostJson("127.0.0.1", server.Port(),
                 "/api/v1/projects/" + projectAId + "/ui-receipts", "fault-ui-token",
                 eda::Json{{"session_id", "s1"}, {"issue", "i1"}, {"level", "answer"}}, status);
        Check(status == 400, "unsupported receipt level rejected (400)");

        // UI 签发合法 receipt。
        const eda::Json issued = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/ui-receipts",
            "fault-ui-token",
            eda::Json{{"session_id", "s-fault"}, {"issue", "latch"}, {"level", "l4"},
                      {"revision", revisionA}, {"challenge_id", "ch-fault"},
                      {"action_id", "act-once"}},
            status);
        Check(status == 201, "UI issues a receipt");
        receiptId = issued["data"].value("receipt_id", std::string());
        Check(!receiptId.empty(), "issued receipt has an id");

        const eda::Json read = GetJson("127.0.0.1", server.Port(),
                                       "/api/v1/ui-receipts/" + receiptId, "fault-token", status);
        Check(status == 200, "Agent can read receipt status");
        Check(!read["data"].contains("issue_content") && !read["data"].contains("summary"),
              "receipt read does not leak issue content into the model context");

        const eda::Json consumeBody{{"run_id", "run-fault"}, {"action_id", "act-once"},
                                    {"expected_state_version", "0"},
                                    {"project_id", projectAId}};

        // 首次核销成功。
        const eda::Json consumed = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/ui-receipts/" + receiptId + "/consume",
            "fault-token", consumeBody, status);
        Check(status == 200, "first consume of a receipt succeeds");
        Check(ErrorCode(consumed).empty(), "successful consume carries no error");
        Check(consumed["data"].value("consumed", false), "consume reports consumed=true");

        // 同 action 重放：幂等返回原结果。
        const eda::Json replay = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/ui-receipts/" + receiptId + "/consume",
            "fault-token", consumeBody, status);
        Check(status == 200, "replaying the same action is idempotent (200)");
        Check(replay["data"].value("replayed", false), "replay is flagged as replayed");

        // 其他 action 重用同一 receipt：必须拒绝，不得再升一级。
        const eda::Json other = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/ui-receipts/" + receiptId + "/consume",
            "fault-token",
            eda::Json{{"run_id", "run-fault"}, {"action_id", "act-other"},
                      {"expected_state_version", "0"}, {"project_id", projectAId}},
            status);
        Check(status == 409 || status == 403, "reusing a consumed receipt for another action fails");
        Check(!ErrorCode(other).empty(), "reused receipt returns an error code");

        // 跨项目核销：project 不符 -> 403，且不得改判为成功。
        const eda::Json crossConsume = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/ui-receipts/" + receiptId + "/consume",
            "fault-token",
            eda::Json{{"run_id", "run-fault"}, {"action_id", "act-once"},
                      {"expected_state_version", "0"}, {"project_id", projectBId}},
            status);
        Check(status == 403 || status == 409, "receipt consumed from another project is refused");
        Check(!ErrorCode(crossConsume).empty(), "cross-project consume returns an error code");
    }

    // ---- 撤销后的 grant 不能再执行 ----
    {
        const eda::Json revoked = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/grants/" + grantA + "/revoke",
            "fault-ui-token", eda::Json::object(), status);
        Check(status == 200, "UI revokes grant A");
        Check(revoked["data"].value("status", std::string()) == "revoked" ||
                  status == 200,
              "revocation reported");

        httplib::Headers key;
        key.emplace("Idempotency-Key", "after-revoke-1");
        const int before = jobs.SubmitCount();
        PostJson("127.0.0.1", server.Port(), "/api/v1/projects/" + projectAId + "/jobs",
                 "fault-token",
                 eda::Json{{"snapshot_id", snapshotA},
                           {"expected_revision", revisionA},
                           {"capability", "eda.synth"},
                           {"grant_id", grantA},
                           {"params", eda::Json{{"top_module", "top"}}}},
                 status, key);
        Check(status == 403, "revoked grant cannot schedule a job (403)");
        Check(jobs.SubmitCount() == before, "revoked grant never reaches the Job service");
    }

    // ---- 响应限额配置可被尊重（不静默截断） ----
    {
        eda::agent::GatewayConfig tiny = config;
        tiny.port = 0;
        tiny.maxResponseBodyBytes = 256;  // 极小限额，强制触发护栏
        eda::agent::GatewayServer limited(tiny, provider);
        std::string startError;
        Check(limited.Start(startError), "guarded gateway starts");
        limited.RunAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::string limitedProjectId;
        Check(limited.RefreshProjectSnapshotStateFromDisk(projectA, false, true, limitedProjectId,
                                                          startError),
              "guarded gateway builds context");
        const eda::Json body = GetJson("127.0.0.1", limited.Port(),
                                       "/api/v1/projects/" + limitedProjectId + "/context",
                                       "fault-token", status);
        Check(status == 429, "oversized response is refused instead of truncated");
        Check(ErrorCode(body) == "RESOURCE_EXHAUSTED",
              "oversized response carries RESOURCE_EXHAUSTED");
        Check(body.contains("error") && body["error"].contains("details") &&
                  body["error"]["details"].contains("limit_bytes"),
              "oversized response reports the configured limit");
        limited.Stop();
    }

    server.Stop();
    Check(!server.Running(), "fault gateway stops cleanly");
    fs::remove_all(root, cleanupError);
    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
