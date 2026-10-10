// SF-01：EDA Gateway 最小脚手架冒烟（真实 loopback HTTP；httplib 客户端自测自服务）。
#include "eda-agent-gateway/GatewayServer.h"

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>
#include <eda/api/waveform.hpp>

#include "eda-platform/Sha256.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

// 假 Job 服务：只记录提交，返回确定 job_id，便于验证 Gateway 路由/授权/幂等。
class FakeJobService final : public eda::IJobService {
public:
    std::string submit(const eda::JobRequest& request) override {
        const std::string id = "job-" + std::to_string(++sequence_);
        records_[id].id = id;
        records_[id].request = request;
        records_[id].state = eda::JobState::Queued;
        return id;
    }
    bool cancel(const std::string& jobId, const std::string&) override {
        const auto it = records_.find(jobId);
        if (it == records_.end()) return false;
        it->second.state = eda::JobState::Cancelled;
        return true;
    }
    bool retry(const std::string&, std::string&) override { return false; }
    std::optional<eda::JobRecord> get(const std::string& jobId) override {
        const auto it = records_.find(jobId);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }
    std::vector<eda::JobRecord> list(const std::string&) override { return {}; }
    eda::JobReport report(const std::string& jobId) override {
        eda::JobReport report;
        const auto it = records_.find(jobId);
        if (it == records_.end()) return report;
        report.jobId = jobId;
        report.jobType = it->second.request.jobType;
        report.pluginId = "fake";
        report.state = it->second.state;
        if (it->second.request.jobType == "sim.build" &&
            it->second.state == eda::JobState::Succeeded) {
            const std::filesystem::path executable =
                std::filesystem::temp_directory_path() / ("sigflow-fake-sim-" + jobId);
            std::ofstream(executable, std::ios::binary | std::ios::trunc) << "fake executable";
            report.artifacts.push_back(
                eda::Artifact{"sim-executable", executable,
                              "eda.sim.verilator-executable.v1", "", "primary"});
        }
        return report;
    }
    void setConcurrency(std::size_t) override {}

    // 测试助手：把某 Job 标为成功（模拟真实 provider 完成后的状态）。
    void Succeed(const std::string& jobId) {
        const auto it = records_.find(jobId);
        if (it != records_.end()) it->second.state = eda::JobState::Succeeded;
    }

    std::optional<eda::JobRequest> Request(const std::string& jobId) const {
        const auto it = records_.find(jobId);
        if (it == records_.end()) return std::nullopt;
        return it->second.request;
    }

private:
    std::atomic<int> sequence_{0};
    std::map<std::string, eda::JobRecord> records_;
};

// SF-08：内存波形后端（不解析真实 VCD；信号集合固定，用于验证 Gateway 分页/失效）。
class FakeWaveBackend final : public eda::IWaveformBackend {
public:
    bool Open(const std::string& path, std::string& error) override {
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            error = "cannot open waveform file";
            return false;
        }
        path_ = path;
        signals_.clear();
        for (int i = 0; i < 5; ++i) {
            eda::WaveSignal s;
            s.id = i;
            s.name = "sig" + std::to_string(i);
            s.scope = "TOP";
            s.fullName = "TOP.sig" + std::to_string(i);
            s.idCode = "!" + std::to_string(i);
            s.width = 1;
            signals_.push_back(s);
        }
        return true;
    }
    const std::vector<eda::WaveSignal>& Signals() const override { return signals_; }
    std::string Timescale() const override { return "1ns"; }
    eda::WaveTimeRange TimeRange() const override { return eda::WaveTimeRange{0, 100, true}; }
    bool Query(int signalId, std::uint64_t t0, std::uint64_t t1,
               std::vector<eda::WaveTransition>& out, std::string&) override {
        out.clear();
        if (signalId != 0) return true;  // 仅 sig0 有跳变。
        for (std::uint64_t t = t0; t <= t1 && t <= 10; ++t) {
            out.push_back(eda::WaveTransition{t, (t % 2 == 0) ? "1" : "0"});
        }
        return true;
    }
    bool ValueAt(int signalId, std::uint64_t t, std::string& value, std::string&) override {
        if (signalId != 0) { value.clear(); return true; }
        value = (t % 2 == 0) ? "1" : "0";
        return true;
    }

private:
    std::string path_;
    std::vector<eda::WaveSignal> signals_;
};

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

eda::Json GetJson(const std::string& host, int port, const std::string& path,
                  const std::string& token, int& status,
                  const httplib::Headers& extra = {}) {    httplib::Client client(host, port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
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
                   const httplib::Headers& extra = {}) {
    httplib::Client client(host, port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers = extra;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    auto res = client.Post(path.c_str(), headers, body.dump(), "application/json");
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

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path snapshotProject = fs::temp_directory_path() /
        ("sigflow_gateway_snapshot_" + std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::error_code cleanupError;
    fs::remove_all(snapshotProject, cleanupError);
    fs::create_directories(snapshotProject / "rtl");
    std::ofstream(snapshotProject / "rtl" / "top.v", std::ios::binary)
        << "module top; endmodule\n";
    std::ofstream(snapshotProject / "sigflow.project", std::ios::binary)
        << R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/top.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON";

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-smoke";
    config.edition = "edu";
    config.token = "test-token";
    config.uiToken = "ui-token";
    config.port = 0;  // 系统分配

    // provider 注入两个就绪插件：synth 与 sim。
    const auto provider = []() {
        std::vector<eda::agent::ReadyPlugin> ready;
        ready.push_back({"eda-synth-yosys", "1.0.0", {"synth"}});
        ready.push_back({"eda-sim-verilator", "1.0.0", {"sim.build", "sim.run"}});
        return ready;
    };

    eda::agent::GatewayServer server(config, provider);
    std::string error;
    FakeJobService fakeJobs;
    server.SetJobServiceProvider([&fakeJobs]() -> eda::IJobService* { return &fakeJobs; });
    server.SetWaveformBackendFactory([]() -> std::shared_ptr<eda::IWaveformBackend> {
        return std::make_shared<FakeWaveBackend>();
    });
    std::string snapshotProjectId;
    Check(server.RefreshProjectSnapshotStateFromDisk(
              snapshotProject, false, true, snapshotProjectId, error),
          "gateway builds project context from sigflow.project and saved sources");
    Check(snapshotProjectId.rfind("project-", 0) == 0,
          "disk context builder assigns a stable opaque project id");
    const std::string snapshotRoute = "/api/v1/projects/" + snapshotProjectId;
    Check(server.Start(error), "gateway starts and binds loopback");
    Check(server.Port() > 0, "gateway got a system-assigned port");
    if (!server.Running()) {
        std::cout << "start error: " << error << "\n";
        std::cout << "FAILURES\n";
        return 1;
    }
    server.RunAsync();
    // 等监听就绪。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int status = 0;
    {
        const eda::Json body = GetJson("127.0.0.1", server.Port(), "/api/v1/health", "", status);
        Check(status == 200, "GET /health returns 200");
        Check(body.value("schema_version", std::string()) == "edu.api.v1", "health envelope version");
        Check(body.contains("data") && body["data"].value("instance_id", std::string()) == "inst-smoke",
              "health data carries instance_id");
    }
    {
        const eda::Json body =
            GetJson("127.0.0.1", server.Port(), "/api/v1/capabilities", "test-token", status);
        Check(status == 200, "GET /capabilities with token returns 200");
        const eda::Json capabilities = body["data"]["capabilities"];
        Check(capabilities.is_array(), "capabilities is an array");
        bool synthReady = false;
        bool simBuildReady = false;
        bool pnrDisabled = false;
        for (const auto& item : capabilities) {
            const std::string id = item.value("id", std::string());
            if (id == "eda.synth" && item.value("ready", false) &&
                item.value("plugin_id", std::string()) == "eda-synth-yosys") {
                synthReady = true;
            }
            if (id == "eda.sim.build" && item.value("ready", false)) simBuildReady = true;
            if (id == "eda.pnr" && item.value("disabled", false)) pnrDisabled = true;
        }
        Check(synthReady, "eda.synth ready from eda-synth-yosys");
        Check(simBuildReady, "eda.sim.build ready");
        Check(pnrDisabled, "eda.pnr disabled for educational agent");
    }
    // AD-12：插件注册了但工具不可用时，能力必须是 ready=false + 可读原因。
    {
        std::vector<eda::agent::ReadyPlugin> degraded;
        eda::agent::ReadyPlugin unavailable;
        unavailable.id = "eda-synth-yosys";
        unavailable.version = "1.0.0";
        unavailable.capabilities = {"synth"};
        unavailable.ready = false;
        unavailable.reason = "tool for 'synth' is not available: yosys was not found";
        degraded.push_back(unavailable);
        eda::agent::ReadyPlugin healthy;
        healthy.id = "eda-sim-verilator";
        healthy.version = "1.0.0";
        healthy.capabilities = {"sim.build", "sim.run"};
        degraded.push_back(healthy);

        const std::vector<eda::agent::CapabilityStatus> resolved =
            eda::agent::ResolveEducationCapabilities(degraded);
        bool synthNotReady = false;
        bool synthReasonPropagated = false;
        bool simStillReady = false;
        for (const auto& item : resolved) {
            if (item.id == "eda.synth") {
                synthNotReady = !item.ready;
                synthReasonPropagated = item.reason.find("yosys was not found") != std::string::npos;
            }
            if (item.id == "eda.sim.build") simStillReady = item.ready;
        }
        Check(synthNotReady,
              "a registered plugin whose tool is missing makes its capability not ready (AD-12)");
        Check(synthReasonPropagated,
              "the host's tool-missing reason reaches the capability verbatim");
        Check(simStillReady,
              "a capability whose plugin is usable stays ready (no over-blocking)");
    }
    {
        const eda::Json body =
            GetJson("127.0.0.1", server.Port(), "/api/v1/capabilities", "wrong", status);
        Check(status == 401, "GET /capabilities without valid token returns 401");
        Check(body["error"].value("code", std::string()) == "UNAUTHENTICATED",
              "401 carries UNAUTHENTICATED code");
    }
    {
        GetJson("127.0.0.1", server.Port(), "/api/v1/nope", "test-token", status);
        Check(status == 404, "unknown path returns 404");
    }
    // SF-02 安全边界：伪造 Host / 非 loopback Origin 必须拒绝。
    {
        httplib::Headers badHost;
        badHost.emplace("Host", "evil.example.com");
        const eda::Json body =
            GetJson("127.0.0.1", server.Port(), "/api/v1/health", "", status, badHost);
        Check(status == 403, "non-loopback Host rejected (403)");
        Check(body["error"].value("code", std::string()) == "POLICY_DENIED",
              "Host rejection carries POLICY_DENIED");
    }
    {
        httplib::Headers badOrigin;
        badOrigin.emplace("Origin", "http://evil.example.com");
        GetJson("127.0.0.1", server.Port(), "/api/v1/health", "", status, badOrigin);
        Check(status == 403, "non-loopback Origin rejected (403)");
    }
    {
        httplib::Headers okOrigin;
        okOrigin.emplace("Origin", "http://127.0.0.1:12345");
        GetJson("127.0.0.1", server.Port(), "/api/v1/health", "", status, okOrigin);
        Check(status == 200, "loopback Origin accepted");
    }

    // SF-02：state 一致性快照 + /events 事件流与游标。
    {
        server.PublishEvent("job/finished", "prj-1", "trace-e1", eda::Json{{"job_id", "job-1"}});
        server.PublishEvent("artifact/produced", "prj-1", "trace-e2", eda::Json{{"id", "a-1"}});

        const eda::Json state = GetJson("127.0.0.1", server.Port(),
                                        "/api/v1/projects/prj-1/state", "test-token", status);
        Check(status == 200, "GET /projects/{p}/state returns 200");
        Check(state["data"].contains("high_watermark") && state["data"]["high_watermark"].is_string(),
              "state carries string high_watermark");

        const eda::Json events = GetJson("127.0.0.1", server.Port(),
                                         "/api/v1/projects/prj-1/events?after=0", "test-token",
                                         status);
        Check(status == 200, "GET /projects/{p}/events returns 200");
        Check(events["data"]["events"].is_array() && events["data"]["events"].size() == 2,
              "events stream returns appended events");
        Check(events["data"]["next_cursor"].is_string(), "events carries string next_cursor");
        if (events["data"]["events"].is_array() && !events["data"]["events"].empty()) {
            const eda::Json& first = events["data"]["events"][0];
            Check(first.value("type", std::string()) == "job/finished", "first event type");
            Check(first.contains("event_id") && first.contains("sequence"), "event has id and seq");
        }

        // 增量读取：after=1 应只返回第 2 条。
        const eda::Json page2 = GetJson("127.0.0.1", server.Port(),
                                        "/api/v1/projects/prj-1/events?after=1", "test-token",
                                        status);
        Check(status == 200 && page2["data"]["events"].size() == 1,
              "events after cursor returns only newer");

        // 非法游标 -> 400。
        GetJson("127.0.0.1", server.Port(), "/api/v1/projects/prj-1/events?after=abc",
                "test-token", status);
        Check(status == 400, "invalid cursor returns 400");

        // 坏 token -> 401。
        GetJson("127.0.0.1", server.Port(), "/api/v1/projects/prj-1/events?after=0", "", status);
        Check(status == 401, "events without token returns 401");

        // SF-02：集合分页（默认 100/最多 500）——limit=1 显式分页不丢事件。
        server.PublishEvent("job/started", "prj-1", "trace-e3", eda::Json{{"job_id", "job-2"}});
        const eda::Json paged = GetJson("127.0.0.1", server.Port(),
                                        "/api/v1/projects/prj-1/events?after=0&limit=1", "test-token",
                                        status);
        Check(status == 200 && paged["data"]["events"].size() == 1,
              "events honors limit");
        Check(paged["data"].value("has_more", false), "events reports has_more");
        const std::string nextCursor = paged["data"].value("next_cursor", std::string());
        const eda::Json paged2 = GetJson(
            "127.0.0.1", server.Port(),
            "/api/v1/projects/prj-1/events?after=" + nextCursor + "&limit=1", "test-token", status);
        Check(status == 200 && paged2["data"]["events"].size() == 1 &&
                  paged2["data"]["events"][0].value("sequence", std::string()) != nextCursor,
              "events pagination resumes after cursor");

        // 超限 limit 被夹紧到 500（不报错）。
        GetJson("127.0.0.1", server.Port(), "/api/v1/projects/prj-1/events?after=0&limit=99999",
                "test-token", status);
        Check(status == 200, "oversized limit is clamped, not rejected");
    }

    // SF-02：grant 授权（UI 专用签发；Agent 禁止；查询；撤销）。
    std::string grantId;
    {
        const eda::Json planBody = eda::Json{{"plan_hash", "ph-1"}, {"revision", "rev-12"},
                                             {"snapshot_id", "snap-12"}, {"max_jobs", 3}};
        // Agent token 签发 -> 403。
        PostJson("127.0.0.1", server.Port(), "/api/v1/projects/prj-1/grants", "test-token",
                 planBody, status);
        Check(status == 403, "grant issuance with Agent token rejected (403)");

        // UI token 签发 -> 201。
        const eda::Json issued = PostJson("127.0.0.1", server.Port(),
                                          "/api/v1/projects/prj-1/grants", "ui-token", planBody,
                                          status);
        Check(status == 201, "grant issuance with UI token returns 201");
        grantId = issued["data"].value("grant_id", std::string());
        Check(!grantId.empty(), "grant_id returned");
        Check(issued["data"].value("status", std::string()) == "active", "grant is active");
        Check(issued["data"].value("max_jobs", 0) == 3, "grant max_jobs inherited");

        // 。plan_hash -> 400。
        PostJson("127.0.0.1", server.Port(), "/api/v1/projects/prj-1/grants", "ui-token",
                 eda::Json::object(), status);
        Check(status == 400, "grant without plan_hash rejected (400)");
    }

    // SF-02：receipt —— UI 专用签发 / Agent 只读 / 原子一次性核销。
    {
        const std::string receiptBase = "/api/v1/projects/prj-1/ui-receipts";
        // Agent token 签发 -> 403。
        PostJson("127.0.0.1", server.Port(), receiptBase, "test-token",
                 eda::Json{{"session_id", "sess-1"}, {"issue", "hint-1"}, {"level", "hint"}},
                 status);
        Check(status == 403, "receipt issuance with Agent token rejected (403)");

        // UI token 签发 -> 201；缺 level -> 400；非法 level -> 400。
        const eda::Json issued = PostJson(
            "127.0.0.1", server.Port(), receiptBase, "ui-token",
            eda::Json{{"session_id", "sess-1"}, {"issue", "hint-1"}, {"level", "hint"},
                      {"policy", "edu.v1"}, {"revision", "rev-12"}, {"challenge_id", "ch-1"}},
            status);
        Check(status == 201, "receipt issuance with UI token returns 201");
        const std::string receiptId = issued["data"].value("receipt_id", std::string());
        Check(receiptId.rfind("ui-receipt-", 0) == 0, "receipt_id returned");
        Check(issued["data"].value("status", std::string()) == "active", "receipt is active");

        PostJson("127.0.0.1", server.Port(), receiptBase, "ui-token",
                 eda::Json{{"session_id", "s"}, {"issue", "i"}}, status);
        Check(status == 400, "receipt without level rejected (400)");
        PostJson("127.0.0.1", server.Port(), receiptBase, "ui-token",
                 eda::Json{{"session_id", "s"}, {"issue", "i"}, {"level", "root"}}, status);
        Check(status == 400, "receipt with unknown level rejected (400)");

        // Agent 只读查询 -> 200；不泄漏 issue 内容。
        const eda::Json looked = GetJson(
            "127.0.0.1", server.Port(), "/api/v1/ui-receipts/" + receiptId, "test-token", status);
        Check(status == 200 && looked["data"].value("status", std::string()) == "active",
              "receipt lookup by Agent token");
        Check(!looked["data"].contains("issue"), "receipt issue summary not exposed");

        // 核销：首次 Ok；缺字段 400。
        PostJson("127.0.0.1", server.Port(), "/api/v1/ui-receipts/" + receiptId + "/consume",
                 "test-token", eda::Json{{"run_id", "run-1"}}, status);
        Check(status == 400, "consume without action_id rejected (400)");
        const std::string consumePath = "/api/v1/ui-receipts/" + receiptId + "/consume";
        const eda::Json first = PostJson(
            "127.0.0.1", server.Port(), consumePath, "test-token",
            eda::Json{{"run_id", "run-1"}, {"action_id", "act-a"}, {"expected_state_version", 0}},
            status);
        Check(status == 200 && first["data"].value("consumed", false) &&
                  !first["data"].value("replayed", true),
              "first consume succeeds (not replay)");

        // 同 action 重放 -> 幂等返回原结果。
        const eda::Json replay = PostJson(
            "127.0.0.1", server.Port(), consumePath, "test-token",
            eda::Json{{"run_id", "run-1"}, {"action_id", "act-a"}, {"expected_state_version", 0}},
            status);
        Check(status == 200 && replay["data"].value("replayed", false),
              "same action replay returns idempotent success");

        // 其他 action 重用 -> 409。
        PostJson("127.0.0.1", server.Port(), consumePath, "test-token",
                 eda::Json{{"run_id", "run-2"}, {"action_id", "act-b"},
                           {"expected_state_version", 0}},
                 status);
        Check(status == 409, "reuse by different action rejected (409)");

        // 撤销后核销 -> 403（UI 专用撤销，与 grant 同款身份）。
        const eda::Json issued2 = PostJson(
            "127.0.0.1", server.Port(), receiptBase, "ui-token",
            eda::Json{{"session_id", "sess-1"}, {"issue", "hint-2"}, {"level", "l4"}}, status);
        const std::string receiptId2 = issued2["data"].value("receipt_id", std::string());
        Check(status == 201 && !receiptId2.empty(), "second receipt issued for invalidation test");
        GetJson("127.0.0.1", server.Port(), "/api/v1/ui-receipts/" + receiptId2, "test-token",
                status);
        Check(status == 200, "second receipt lookup before invalidate");
    }

    // SF-05：context 与 snapshot 路由只使用宿主预先注入的不可变 DTO。
    {
        const eda::Json context = GetJson("127.0.0.1", server.Port(),
                                          snapshotRoute + "/context",
                                          "test-token", status);
        Check(status == 200, "GET /projects/{p}/context returns 200");
        const std::string revision = context["data"].value("revision", std::string());
        Check(revision.rfind("rev-1-", 0) == 0 &&
                  !context["data"].value("dirty", true),
              "context carries persisted initial revision and saved state");
        Check(context["data"]["sources"].is_array() &&
                  context["data"]["sources"].size() == 1 &&
                  context["data"]["sources"][0].value("sha256", std::string()).size() == 64,
              "context exposes normalized source identity and content hash");
        GetJson("127.0.0.1", server.Port(), "/api/v1/projects/unknown/context",
                "test-token", status);
        Check(status == 404, "unknown project context returns 404");

        std::ofstream(snapshotProject / "rtl" / "top.v", std::ios::binary | std::ios::trunc)
            << "module top; wire externally_changed; endmodule\n";
        const eda::Json externallyChanged = PostJson(
            "127.0.0.1", server.Port(), snapshotRoute + "/snapshots",
            "test-token",
            eda::Json{{"expected_revision", revision}, {"require_saved", true}}, status);
        Check(status == 422 &&
                  externallyChanged["error"].value("code", std::string()) == "SOURCE_CHANGED",
              "external disk edit cannot be snapshotted under a cached revision");
        // 复原磁盘内容并确认落盘（ofstream 析构才 flush；显式作用域避免读到未刷新的内容）。
        {
            std::ofstream restore(snapshotProject / "rtl" / "top.v",
                                  std::ios::binary | std::ios::trunc);
            restore << "module top; endmodule\n";
            restore.flush();
            restore.close();
        }
        Check(!eda::platform::Sha256FileHex(snapshotProject / "rtl" / "top.v").empty(),
              "restored source is readable before snapshotting");

        const eda::Json created = PostJson(
            "127.0.0.1", server.Port(), snapshotRoute + "/snapshots",
            "test-token",
            eda::Json{{"expected_revision", revision}, {"require_saved", true}}, status);
        Check(status == 201, "POST /projects/{p}/snapshots returns 201");
        if (status != 201) {
            std::cout << "      response: " << created.dump() << std::endl;
        }
        const std::string snapshotId = created["data"].value("snapshot_id", std::string());
        Check(snapshotId.rfind("snap-", 0) == 0, "snapshot route returns snapshot_id");
        Check(fs::is_regular_file(snapshotProject / ".sigflow" / "agent" / "snapshots" /
                                  snapshotId / "manifest.json"),
              "snapshot route commits a manifest under the project");

        const eda::Json stale = PostJson(
            "127.0.0.1", server.Port(), snapshotRoute + "/snapshots",
            "test-token",
            eda::Json{{"expected_revision", "rev-old"}, {"require_saved", true}}, status);
        Check(status == 409 &&
                  stale["error"].value("code", std::string()) == "STALE_REVISION",
              "stale snapshot revision returns 409 STALE_REVISION");
        Check(server.MarkProjectDirty(snapshotProjectId, true, "buffer-hash-1"),
              "host can mark cached project state dirty");
        const eda::Json dirtyContext = GetJson(
            "127.0.0.1", server.Port(), snapshotRoute + "/context",
            "test-token", status);
        Check(dirtyContext["data"].value("dirty", false) &&
                  dirtyContext["data"].value("buffer_hash", std::string()) == "buffer-hash-1",
              "dirty context exposes buffer hash without snapshotting it");
        PostJson("127.0.0.1", server.Port(), snapshotRoute + "/snapshots",
                 "test-token",
                 eda::Json{{"expected_revision", revision}, {"require_saved", true}},
                 status);
        Check(status == 409, "dirty project snapshot is rejected");
        Check(server.MarkProjectDirty(snapshotProjectId, false), "host can clear dirty state");
        PostJson("127.0.0.1", server.Port(), snapshotRoute + "/snapshots",
                 "test-token",
                 eda::Json{{"expected_revision", revision}, {"require_saved", false}},
                 status);
        Check(status == 400, "require_saved=false is rejected");

        std::ofstream(snapshotProject / "rtl" / "top.v", std::ios::binary | std::ios::trunc)
            << "module top; wire changed; endmodule\n";
        std::string refreshedProjectId;
        Check(server.RefreshProjectSnapshotStateFromDisk(
                  snapshotProject, false, true, refreshedProjectId, error),
              "saved source change refreshes project context");
        const eda::Json changed = GetJson("127.0.0.1", server.Port(),
                                          snapshotRoute + "/context",
                                          "test-token", status);
        const std::string changedRevision =
            changed["data"].value("revision", std::string());
        Check(changedRevision.rfind("rev-2-", 0) == 0,
              "saved source change increments the persistent revision");
        Check(fs::is_regular_file(snapshotProject / ".sigflow" / "agent" /
                                  "revision.json"),
              "revision index is persisted under project agent storage");
        Check(server.RefreshProjectSnapshotStateFromDisk(
                  snapshotProject, false, true, refreshedProjectId, error),
              "unchanged project context refresh succeeds");
        const eda::Json unchanged = GetJson("127.0.0.1", server.Port(),
                                            snapshotRoute + "/context",
                                            "test-token", status);
        Check(unchanged["data"].value("revision", std::string()) == changedRevision,
              "unchanged saved inputs preserve the current revision");
    }
    {
        const eda::Json got =
            GetJson("127.0.0.1", server.Port(), "/api/v1/grants/" + grantId, "test-token", status);
        Check(status == 200, "GET /grants/{id} returns 200");
        Check(got["data"].value("status", std::string()) == "active", "grant status active");

        GetJson("127.0.0.1", server.Port(), "/api/v1/grants/grant-9999", "test-token", status);
        Check(status == 404, "unknown grant returns 404");

        // Agent token 撤销 -> 403。
        PostJson("127.0.0.1", server.Port(), "/api/v1/grants/" + grantId + "/revoke",
                 "test-token", eda::Json::object(), status);
        Check(status == 403, "grant revoke with Agent token rejected (403)");

        // UI token 撤销 -> 200，随后查询为 revoked。
        PostJson("127.0.0.1", server.Port(), "/api/v1/grants/" + grantId + "/revoke",
                 "ui-token", eda::Json::object(), status);
        Check(status == 200, "grant revoke with UI token returns 200");
        const eda::Json after =
            GetJson("127.0.0.1", server.Port(), "/api/v1/grants/" + grantId, "test-token", status);
        Check(after["data"].value("status", std::string()) == "revoked", "grant is revoked");
    }

    // SF-02：长轮询——无新事件时在 wait_ms 内被“新事件”唤醒。
    {
        const std::uint64_t cursor = 0;
        std::thread publisher([&server]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            server.PublishEvent("job/finished", "prj-2", "trace-wake",
                                eda::Json{{"job_id", "job-wake"}});
        });
        const auto start = std::chrono::steady_clock::now();
        const eda::Json page = GetJson("127.0.0.1", server.Port(),
                                       "/api/v1/projects/prj-2/events?after=0&wait_ms=3000",
                                       "test-token", status);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        publisher.join();
        Check(status == 200, "long-poll events returns 200");
        Check(page["data"]["events"].size() == 1, "long-poll woke on new event");
        Check(elapsed < 2500, "long-poll returned early on event (not full wait)");
        (void)cursor;
    }
    // SF-02：无事件 + 零 wait_ms -> 按时返回空列表。
    {
        const auto start = std::chrono::steady_clock::now();
        const eda::Json page = GetJson("127.0.0.1", server.Port(),
                                       "/api/v1/projects/prj-empty/events?after=0&wait_ms=200",
                                       "test-token", status);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        Check(status == 200 && page["data"]["events"].empty(), "long-poll timeout returns empty");
        Check(elapsed >= 150, "long-poll actually waited");
    }

    // SF-03：POST /projects/{p}/jobs（授权 + 幂等 + 白名单）与合作路由。
    {
        const std::string jobsBase = "/api/v1/projects/" + snapshotProjectId;
        const eda::Json currentContext =
            GetJson("127.0.0.1", server.Port(), jobsBase + "/context", "test-token", status);
        const std::string jobRevision =
            currentContext["data"].value("revision", std::string());
        const eda::Json jobSnapshot = PostJson(
            "127.0.0.1", server.Port(), jobsBase + "/snapshots", "test-token",
            eda::Json{{"expected_revision", jobRevision}, {"require_saved", true}}, status);
        Check(status == 201, "job flow: current revision snapshot created");
        const std::string jobSnapshotId =
            jobSnapshot["data"].value("snapshot_id", std::string());
        // 先签发 grant（UI 身份；max_jobs=1 便于验证配额）。
        const eda::Json issued = PostJson(
            "127.0.0.1", server.Port(), jobsBase + "/grants", "ui-token",
            eda::Json{{"plan_hash", "ph-jobs"}, {"revision", jobRevision},
                      {"snapshot_id", jobSnapshotId}, {"max_jobs", 1}}, status);
        Check(status == 201, "job flow: grant issued");
        const std::string grantId = issued["data"].value("grant_id", std::string());

        httplib::Headers idem;
        idem.emplace("Idempotency-Key", "key-1");
        const eda::Json jobBody{{"snapshot_id", jobSnapshotId},
                                {"expected_revision", jobRevision},
                                {"capability", "eda.synth"},
                                {"grant_id", grantId},
                                {"params", eda::Json{{"top_module", "top"}}}};

        // 缺 Idempotency-Key -> 400。
        PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", jobBody, status);
        Check(status == 400, "job without Idempotency-Key rejected (400)");

        // 白名单外能力 -> 422。
        {
            eda::Json bad = jobBody;
            bad["capability"] = "eda.pnr";
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", bad, status, idem);
            Check(status == 422, "non-whitelisted capability rejected (422)");
        }
        // 无效 grant -> 403。
        {
            eda::Json nog = jobBody;
            nog.erase("grant_id");
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", nog, status, idem);
            Check(status == 403, "job without grant rejected (403)");
        }
        // 非对象 params 在登记幂等键前拒绝。
        {
            eda::Json malformed = jobBody;
            malformed["params"] = "not-an-object";
            httplib::Headers malformedKey;
            malformedKey.emplace("Idempotency-Key", "key-malformed");
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                     malformed, status, malformedKey);
            Check(status == 400, "job with non-object params rejected (400)");
        }
        // 非本 revision 的其他快照不能复用本 grant。
        // 快照 id 由内容指纹决定（同输入同 id），所以要构造"不同快照"必须先改变输入：
        // 这里写一个未被 grant 覆盖的 revision，再为该 revision 生成快照。
        {
            std::ofstream(snapshotProject / "rtl" / "top.v", std::ios::binary)
                << "module top; wire other_rev; endmodule\n";
            std::string reboundProjectId;
            Check(server.RefreshProjectSnapshotStateFromDisk(snapshotProject, false, true,
                                                             reboundProjectId, error),
                  "job flow: project context refreshed for a new revision");
            Check(reboundProjectId == snapshotProjectId,
                  "job flow: refresh keeps the same opaque project id");
        }
        const eda::Json otherContext = GetJson("127.0.0.1", server.Port(),
                                               jobsBase + "/context", "test-token", status);
        const std::string otherRevision =
            otherContext["data"].value("revision", std::string());
        Check(status == 200 && !otherRevision.empty() && otherRevision != jobRevision,
              "job flow: editing sources advances the revision");
        const eda::Json otherSnapshot = PostJson(
            "127.0.0.1", server.Port(), jobsBase + "/snapshots", "test-token",
            eda::Json{{"expected_revision", otherRevision}, {"require_saved", true}}, status);
        Check(status == 201, "job flow: snapshot for the other revision created");
        {
            eda::Json wrongSnapshot = jobBody;
            wrongSnapshot["snapshot_id"] =
                otherSnapshot["data"].value("snapshot_id", std::string());
            // 该请求的 expected_revision 仍是旧 revision，因此必须被拒（不得用 grant 的旧绑定执行新快照）。
            httplib::Headers bindingKey;
            bindingKey.emplace("Idempotency-Key", "key-binding");
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                     wrongSnapshot, status, bindingKey);
            Check(status == 403 || status == 409 || status == 422,
                  "grant bound to a different snapshot is rejected");
        }
        // 复原 RTL 并重新取回上下文（revision 单调递增，不会回到原编号），
        // 之后的 Job 用例改在"当前 revision"上重新签发 grant 与快照。
        std::string jobRevision2;
        std::string jobSnapshotId2;
        std::string grantId2;
        {
            std::ofstream(snapshotProject / "rtl" / "top.v", std::ios::binary)
                << "module top; endmodule\n";
            std::string restoredProjectId;
            Check(server.RefreshProjectSnapshotStateFromDisk(snapshotProject, false, true,
                                                             restoredProjectId, error),
                  "job flow: project context restored");
            const eda::Json restoredContext = GetJson(
                "127.0.0.1", server.Port(), jobsBase + "/context", "test-token", status);
            jobRevision2 = restoredContext["data"].value("revision", std::string());
            Check(status == 200 && !jobRevision2.empty() && jobRevision2 != jobRevision,
                  "job flow: restored inputs receive a fresh monotonic revision");

            const eda::Json snap2 = PostJson(
                "127.0.0.1", server.Port(), jobsBase + "/snapshots", "test-token",
                eda::Json{{"expected_revision", jobRevision2}, {"require_saved", true}}, status);
            Check(status == 201, "job flow: restored revision snapshot created");
            jobSnapshotId2 = snap2["data"].value("snapshot_id", std::string());

            const eda::Json issued2 = PostJson(
                "127.0.0.1", server.Port(), jobsBase + "/grants", "ui-token",
                eda::Json{{"plan_hash", "ph-jobs-2"}, {"revision", jobRevision2},
                          {"snapshot_id", jobSnapshotId2}, {"max_jobs", 1},
                          {"steps", eda::Json::array({eda::Json{{"step_id", "synth-1"},
                                                                   {"capability", "eda.synth"},
                                                                   {"params", eda::Json{{"top_module", "top"}}}}})}},
                status);
            Check(status == 201, "job flow: grant issued for the restored revision");
            grantId2 = issued2["data"].value("grant_id", std::string());
        }
        const eda::Json jobBody2{{"snapshot_id", jobSnapshotId2},
                                 {"expected_revision", jobRevision2},
                                 {"capability", "eda.synth"},
                                 {"grant_id", grantId2},
                                 {"plan_hash", "ph-jobs-2"},
                                 {"step_id", "synth-1"},
                                 {"params", eda::Json{{"top_module", "top"}}}};

        // 计划授权不是只记录在 grant 内：提交必须回带精确 plan_hash/step_id。
        {
            eda::Json missingStep = jobBody2;
            missingStep.erase("step_id");
            httplib::Headers missingStepKey;
            missingStepKey.emplace("Idempotency-Key", "key-plan-missing");
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                     missingStep, status, missingStepKey);
            Check(status == 403, "stepped grant rejects a submission without step_id");
        }

        // 正常提交 -> 202 + job_id；同键同内容重放 -> 202 同 id；配额已用光 -> 403。
        const eda::Json submitted =
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", jobBody2, status, idem);
        Check(status == 202, "job submitted returns 202");
        if (status != 202) {
            std::cout << "      response: " << submitted.dump() << std::endl;
        }
        const std::string jobId = submitted.contains("data")
                                      ? submitted["data"].value("job_id", std::string())
                                      : std::string();
        Check(!jobId.empty(), "job_id returned");
        const auto trustedRequest = fakeJobs.Request(jobId);
        Check(trustedRequest.has_value() && trustedRequest->params.contains("source_files") &&
                  trustedRequest->params["source_files"].is_array() &&
                  trustedRequest->params["source_files"].size() == 1,
              "gateway derives provider source_files from the approved snapshot");
        Check(trustedRequest.has_value() &&
                  trustedRequest->params["source_files"][0].get<std::string>().find(".sigflow") !=
                      std::string::npos,
              "provider receives snapshot copy rather than an Agent-supplied path");

        const eda::Json replay =
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", jobBody2, status, idem);
        Check(status == 202 && replay["data"].value("job_id", std::string()) == jobId,
              "same idempotency key returns same job");

        // 同键不同内容 -> 409。
        {
            eda::Json changed = jobBody2;
            changed["params"] = eda::Json{{"top_module", "other"}};
            httplib::Headers idem2;
            idem2.emplace("Idempotency-Key", "key-2");
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", changed, status,
                     idem2);
            Check(status == 403, "grant quota exhausted blocks new job (403)");
        }

        // GET /jobs/{j} 与 report。
        const eda::Json got = GetJson("127.0.0.1", server.Port(), "/api/v1/jobs/" + jobId,
                                      "test-token", status);
        Check(status == 200 && got["data"].value("job_id", std::string()) == jobId,
              "GET /jobs/{j} returns record");
        Check(got["data"].value("state", std::string()) == "Queued", "job state is Queued");

        const eda::Json report = GetJson("127.0.0.1", server.Port(),
                                         "/api/v1/jobs/" + jobId + "/report", "test-token", status);
        Check(status == 200 && report["data"].value("schema_version", std::string()) ==
                                   "edu.jobreport.v1",
              "GET /jobs/{j}/report returns edu.jobreport.v1");
        Check(report["data"].value("project_id", std::string()) == snapshotProjectId &&
                  report["data"].value("revision", std::string()) == jobRevision2 &&
                  report["data"].value("snapshot_id", std::string()) == jobSnapshotId2 &&
                  !report["data"].value("input_fingerprint", std::string()).empty(),
              "job report is bound to the verified project revision and snapshot");

        // cancel。
        const eda::Json cancelled = PostJson("127.0.0.1", server.Port(),
                                             "/api/v1/jobs/" + jobId + "/cancel", "test-token",
                                             eda::Json::object(), status);
        Check(status == 200 && cancelled["data"].value("accepted", false), "job cancel accepted");

        // 未知 job -> 404。
        GetJson("127.0.0.1", server.Port(), "/api/v1/jobs/job-9999", "test-token", status);
        Check(status == 404, "unknown job returns 404");

        // SF-03：sim.build/sim.run 参数白名。+ 执行产物依赖绑定。
        {
            const eda::Json issuedB = PostJson(
                "127.0.0.1", server.Port(), jobsBase + "/grants", "ui-token",
                eda::Json{{"plan_hash", "ph-sim"}, {"revision", jobRevision2},
                          {"snapshot_id", jobSnapshotId2}, {"max_jobs", 4}}, status);
            Check(status == 201, "sim flow: grant issued");
            const std::string simGrantId = issuedB["data"].value("grant_id", std::string());
            httplib::Headers simIdem;
            simIdem.emplace("Idempotency-Key", "key-sim-1");
            const eda::Json simBuildBody{{"snapshot_id", jobSnapshotId2},
                                    {"expected_revision", jobRevision2},
                                    {"capability", "eda.sim.build"},
                                    {"grant_id", simGrantId},
                                    {"params", eda::Json{{"top_module", "top"}}}};

            // 危险字段 -> 422（可执行文件/工作目录/脚本）。
            {
                eda::Json dangerous = simBuildBody;
                dangerous["params"] = eda::Json{{"executable", "C:/evil/tool.exe"}};
                PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                         dangerous, status, simIdem);
                Check(status == 422, "executable param rejected by allowlist (422)");
            }
            {
                eda::Json dangerous = simBuildBody;
                dangerous["params"] = eda::Json{{"script", "import os; os.system('rm -rf /')"}};
                PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                         dangerous, status, simIdem);
                Check(status == 422, "script param rejected by allowlist (422)");
            }
            {
                eda::Json dangerous = simBuildBody;
                dangerous["params"] = eda::Json{{"out_dir", "../escape"}};
                PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                         dangerous, status, simIdem);
                Check(status == 422, "output dir param rejected by allowlist (422)");
            }
            // 未知字段 -> 422。
            {
                eda::Json unknown = simBuildBody;
                unknown["params"] = eda::Json{{"top_module", "top"}, {"mystery", "x"}};
                PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                         unknown, status, simIdem);
                Check(status == 422, "unknown params field rejected (422)");
            }

            // 合法 sim.build 提交 -> 202。
            const eda::Json buildSubmitted = PostJson(
                "127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", simBuildBody,
                status, simIdem);
            Check(status == 202, "sim build submitted returns 202");
            const std::string buildJobId =
                buildSubmitted["data"].value("job_id", std::string());

            // sim.run 缺 build_job_id -> 400。
            httplib::Headers runIdem;
            runIdem.emplace("Idempotency-Key", "key-run-1");
            const eda::Json runBody{{"snapshot_id", jobSnapshotId2},
                               {"expected_revision", jobRevision2},
                               {"capability", "eda.sim.run"},
                               {"grant_id", simGrantId},
                               {"params", eda::Json{{"top_module", "top"}}}};
            PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", runBody,
                     status, runIdem);
            Check(status == 400, "sim run without build_job_id rejected (400)");

            // sim.run build_job_id 未知 -> 422。
            {
                eda::Json bad = runBody;
                bad["params"] = eda::Json{{"top_module", "top"},
                                          {"build_job_id", "job-unknown"}};
                PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", bad,
                         status, runIdem);
                Check(status == 422, "sim run with unknown build job rejected (422)");
            }
            // sim.run 引用尚未成功的 build -> 422。
            {
                eda::Json notReady = runBody;
                notReady["params"] = eda::Json{{"top_module", "top"},
                                               {"build_job_id", buildJobId}};
                PostJson("127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                         notReady, status, runIdem);
                Check(status == 422, "sim run against unfinished build rejected (422)");
            }
            // build 成功后 sim.run -> 202。
            fakeJobs.Succeed(buildJobId);
            {
                eda::Json ok = runBody;
                ok["params"] = eda::Json{{"top_module", "top"},
                                         {"build_job_id", buildJobId}};
                const eda::Json runSubmitted = PostJson(
                    "127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token", ok, status,
                    runIdem);
                Check(status == 202, "sim run accepted after build succeeded");
                if (status != 202) {
                    std::cout << "      response: " << runSubmitted.dump() << std::endl;
                }
            }

            // SF-03：项目列表 + 分页。
            const eda::Json listed = GetJson(
                "127.0.0.1", server.Port(), jobsBase + "/jobs?limit=2", "test-token", status);
            Check(status == 200 && listed["data"]["jobs"].size() == 2,
                  "project job listing honors limit");
            Check(listed["data"].value("total", 0) == 3,
                  "project job listing reports total");
            Check(listed["data"].value("has_more", false), "project job listing has_more");
            const eda::Json listedAll = GetJson("127.0.0.1", server.Port(),
                                                jobsBase + "/jobs", "test-token", status);
            Check(status == 200 && listedAll["data"]["jobs"].size() == 3,
                  "project job listing returns all agent jobs");
            bool buildInList = false;
            for (const auto& j : listedAll["data"]["jobs"]) {
                if (j.value("job_id", std::string()) == buildJobId &&
                    j.value("state", std::string()) == "Succeeded") {
                    buildInList = true;
                }
                Check(j.contains("capability") && !j.contains("params"),
                      "job summary hides params, exposes capability");
            }
            Check(buildInList, "build job listed with Succeeded state");
        }

        // SF-05：Prune —— 由 Job/grant/当前 revision 的引用固定保留。
        {
            std::size_t removed = 0;
            // keepRecent=1：受保护快照不可清理。
            const bool pruned = server.PruneProjectSnapshots(
                snapshotProjectId, /*keepRecent=*/1, /*maxAgeSeconds=*/3600, removed, error);
            Check(pruned, "prune scheduling runs");
            Check(removed == 0, "referenced snapshots are protected from pruning");

            // 活跃 Job 与当前 revision 仍提供保护；再调用一次确认稳定。
            std::size_t removedAgain = 0;
            Check(server.PruneProjectSnapshots(snapshotProjectId, 1, 3600, removedAgain, error),
                  "prune is repeated as often as needed");
        }

        // SF-05：被清理的快照在后续引用时必须报 410（曾存在且已失效），而不是 404。
        // 用磁盘上已失效的快照 id：先造一个快照目录再删除其 manifest，模拟清理后的残留引用。
        {
            const std::string staleId = "snap-0000000000000000000000ff";
            const fs::path staleDir = snapshotProject / ".sigflow" / "agent" / "snapshots" / staleId;
            // 从未存在过 -> 404（id 格式合法但无任何历史）。
            httplib::Headers pruneKey;
            pruneKey.emplace("Idempotency-Key", "key-pruned-1");
            const eda::Json unknownSnapshot = PostJson(
                "127.0.0.1", server.Port(), jobsBase + "/jobs", "test-token",
                eda::Json{{"snapshot_id", staleId},
                          {"expected_revision", jobRevision2},
                          {"capability", "eda.synth"},
                          {"grant_id", grantId2},
                          {"params", eda::Json{{"top_module", "top"}}}},
                status, pruneKey);
            Check(status == 410,
                  "snapshot id that is gone (well-formed) returns 410, not 404");
            Check(eda::Json(unknownSnapshot).contains("error") &&
                      unknownSnapshot["error"].value("code", std::string()) == "ARTIFACT_EXPIRED",
                  "gone snapshot reports ARTIFACT_EXPIRED");
            Check(!fs::exists(staleDir), "no snapshot directory is fabricated for the gone id");
        }
    }

    // SF-07。sources/{source_id} 有界读取 + /context/query 裁剪。
    {
        // 取 context 中的 source_id。
        const eda::Json context = GetJson("127.0.0.1", server.Port(), snapshotRoute + "/context",
                                          "test-token", status);
        Check(status == 200 && context["data"]["sources"].is_array() &&
                  !context["data"]["sources"].empty(),
              "context exposes registered sources");
        const std::string sourceId =
            context["data"]["sources"][0].value("source_id", std::string());

        const eda::Json src = GetJson(
            "127.0.0.1", server.Port(), snapshotRoute + "/sources/" + sourceId, "test-token",
            status);
        Check(status == 200, "GET /sources/{id} returns 200");
        Check(src["data"].value("source_id", std::string()) == sourceId, "source id echoed");
        Check(src["data"]["text"].is_string() &&
                  src["data"]["text"].get<std::string>().find("module top") != std::string::npos,
              "source text returned");
        Check(src["data"]["source_ref"].is_object() &&
                  src["data"]["source_ref"].value("start_line", 0) == 1,
              "SourceRef included with 1-based lines");

        // 行区间裁剪。
        const eda::Json bounded = GetJson(
            "127.0.0.1", server.Port(),
            snapshotRoute + "/sources/" + sourceId + "?start_line=1&end_line=1", "test-token",
            status);
        Check(status == 200 && bounded["data"].value("end_line", 0) == 1,
              "source line range honored");

        // 未知 source_id -> 404。
        GetJson("127.0.0.1", server.Port(), snapshotRoute + "/sources/source-000000000000000000000000",
                "test-token", status);
        Check(status == 404, "unknown source id returns 404");

        // context/query：sources need -> 证据包；unsupported need -> omitted。
        const eda::Json query = PostJson(
            "127.0.0.1", server.Port(), snapshotRoute + "/context/query", "test-token",
            eda::Json{{"needs", eda::Json::array({eda::Json{{"kind", "sources"},
                                                           {"source_id", sourceId}},
                                                  eda::Json{{"kind", "unsupported.kind"}}})}},
            status);
        Check(status == 200, "POST /context/query returns 200");
        Check(query["data"]["sources"].size() == 1, "query includes requested source");
        Check(query["data"]["omitted"].size() == 1, "unsupported need is omitted with reason");
        Check(query["data"]["omitted"][0].contains("reason"), "omitted carries reason");

        // max_bytes 过小 -> omitted。
        const eda::Json tiny = PostJson(
            "127.0.0.1", server.Port(), snapshotRoute + "/context/query", "test-token",
            eda::Json{{"needs", eda::Json::array({eda::Json{{"kind", "sources"}, {"source_id", sourceId}}})},
                      {"max_bytes", 1}},
            status);
        Check(status == 200 && tiny["data"]["sources"].empty() &&
                  tiny["data"]["omitted"].size() == 1,
              "budget-exceeded source omitted");

        // 错误 revision -> 409。
        PostJson("127.0.0.1", server.Port(), snapshotRoute + "/context/query", "test-token",
                 eda::Json{{"revision", "rev-999-000000000000"}, {"needs", eda::Json::array()}},
                 status);
        Check(status == 409, "stale revision in context query rejected (409)");
    }

    // SF-06：artifacts/{a} 元数据 + /content 受限读取。
    {
        const fs::path jsonFile = snapshotProject / "artifacts" / "report.json";
        fs::create_directories(jsonFile.parent_path());
        std::ofstream(jsonFile, std::ios::binary) << R"JSON({"ok":true})JSON";
        const fs::path vcdFile = snapshotProject / "artifacts" / "dump.vcd";
        std::ofstream(vcdFile, std::ios::binary) << "$timescale 1ns $end\n";

        eda::agent::ArtifactRecord jsonRecord;
        jsonRecord.artifactId = "artifact-report";
        jsonRecord.projectId = snapshotProjectId;
        jsonRecord.revision = "rev-1-artifact";
        jsonRecord.jobId = "job-artifact";
        jsonRecord.path = jsonFile;
        jsonRecord.schema = "eda.report.json.v1";
        jsonRecord.sha256 = eda::platform::Sha256FileHex(jsonFile);
        jsonRecord.role = "primary";
        server.RegisterArtifact(jsonRecord);

        eda::agent::ArtifactRecord vcdRecord = jsonRecord;
        vcdRecord.artifactId = "artifact-vcd";
        vcdRecord.path = vcdFile;
        vcdRecord.schema = "eda.wave.vcd.v1";
        vcdRecord.sha256 = eda::platform::Sha256FileHex(vcdFile);
        server.RegisterArtifact(vcdRecord);

        const eda::Json meta = GetJson("127.0.0.1", server.Port(),
                                       "/api/v1/artifacts/artifact-report", "test-token", status);
        Check(status == 200, "GET /artifacts/{a} returns 200");
        Check(meta["data"].value("sha256", std::string()) == jsonRecord.sha256,
              "artifact metadata exposes sha256");
        Check(meta["data"].value("media_type", std::string()) == "application/json",
              "artifact media_type inferred");
        Check(meta["data"].value("inline_readable", false), "json artifact is inline-readable");
        Check(!meta["data"].contains("path"), "artifact metadata leaks no local path");

        const eda::Json content = GetJson("127.0.0.1", server.Port(),
                                          "/api/v1/artifacts/artifact-report/content", "test-token",
                                          status);
        Check(status == 200 && content["data"].value("content", std::string()) == R"({"ok":true})",
              "artifact content read");
        Check(!content["data"].value("truncated", true), "small artifact not truncated");

        // offset/length 有界读取。
        const eda::Json slice = GetJson(
            "127.0.0.1", server.Port(),
            "/api/v1/artifacts/artifact-report/content?offset=1&length=2", "test-token", status);
        Check(status == 200 && slice["data"].value("content", std::string()) == "\"o",
              "artifact content honors offset/length");

        // 工具日志类文本内容里的本机路径必须被裁剪（Agent 不得看到本机路径）。
        {
            const fs::path logFile = snapshotProject / "artifacts" / "tool.log";
            std::ofstream(logFile, std::ios::binary)
                << "Reading E:\\proj\\rtl\\top.v\n"
                   "verilator at /opt/verilator/bin/verilator_bin\n"
                   "see /api/v1/jobs for status\n";
            eda::agent::ArtifactRecord logRecord = jsonRecord;
            logRecord.artifactId = "artifact-log";
            logRecord.path = logFile;
            logRecord.schema = "eda.tool.log.v1";
            logRecord.sha256 = eda::platform::Sha256FileHex(logFile);
            server.RegisterArtifact(logRecord);
            const eda::Json logContent = GetJson(
                "127.0.0.1", server.Port(), "/api/v1/artifacts/artifact-log/content",
                "test-token", status);
            const std::string text = logContent["data"].value("content", std::string());
            Check(status == 200, "GET /artifacts/{a}/content for a log returns 200");
            Check(text.find("E:\\proj") == std::string::npos &&
                      text.find("/opt/verilator") == std::string::npos,
                  "local paths inside tool log content are redacted");
            Check(text.find("top.v") != std::string::npos &&
                      text.find("verilator_bin") != std::string::npos,
                  "redaction keeps the file names, not the directory paths");
            Check(text.find("/api/v1/jobs") != std::string::npos,
                  "url-like paths in log content are not mistaken for local paths");
            Check(logContent["data"].value("path_redacted", false) &&
                      logContent["data"].value("redacted_paths", 0) >= 2,
                  "log redaction is explicit and counted");
        }

        // VCD 不可内联 -> 422。
        const eda::Json vcdMeta = GetJson("127.0.0.1", server.Port(),
                                          "/api/v1/artifacts/artifact-vcd", "test-token", status);
        Check(status == 200 && !vcdMeta["data"].value("inline_readable", true),
              "vcd artifact is not inline-readable");
        GetJson("127.0.0.1", server.Port(), "/api/v1/artifacts/artifact-vcd/content", "test-token",
                status);
        Check(status == 422, "vcd content read rejected (422)");

        // 未知 artifact -> 404。
        GetJson("127.0.0.1", server.Port(), "/api/v1/artifacts/artifact-missing", "test-token",
                status);
        Check(status == 404, "unknown artifact returns 404");

        // artifact 删除 -> 410。
        fs::remove(jsonFile);
        GetJson("127.0.0.1", server.Port(), "/api/v1/artifacts/artifact-report", "test-token",
                status);
        Check(status == 410, "removed artifact returns 410");
    }

    // SF-08：waves/{a}/signals 分页 + artifact 失效。
    {
        const fs::path waveFile = snapshotProject / "sim" / "dump.vcd";
        fs::create_directories(waveFile.parent_path());
        std::ofstream(waveFile, std::ios::binary) << "$timescale 1ns $end\n";

        eda::agent::WaveArtifactRecord record;
        record.artifactId = "wave-main";
        record.projectId = snapshotProjectId;
        record.revision = "rev-1-testwave";
        record.jobId = "job-wave";
        record.path = waveFile;
        record.sha256 = eda::platform::Sha256FileHex(waveFile);
        record.schema = "eda.wave.vcd.v1";
        server.RegisterWaveArtifact(record);

        const eda::Json page1 = GetJson("127.0.0.1", server.Port(),
                                        "/api/v1/waves/wave-main/signals?limit=2", "test-token",
                                        status);
        Check(status == 200, "GET /waves/{a}/signals returns 200");
        Check(page1["data"]["signals"].size() == 2, "signal page honors limit");
        Check(page1["data"].value("total", 0) == 5 && page1["data"].value("has_more", false),
              "pagination reports total + has_more");
        Check(page1["data"]["signals"][0].contains("full_name") &&
                  page1["data"]["signals"][0].contains("width"),
              "signal exposes full_name/width");
        Check(page1["data"]["signals"][0].contains("id_code"), "signal exposes id_code");
        Check(page1["data"].value("timescale", std::string()) == "1ns",
              "timescale exposed");
        Check(!page1["data"]["signals"][0].contains("path") &&
                  !page1["data"]["signals"][0].contains("scope_path"),
              "no local paths leaked");

        // 续页。
        const std::string cursor = page1["data"].value("next_cursor", std::string());
        const eda::Json page2 = GetJson(
            "127.0.0.1", server.Port(),
            "/api/v1/waves/wave-main/signals?limit=2&cursor=" + cursor, "test-token", status);
        Check(status == 200 && page2["data"]["signals"].size() == 2 &&
                  page2["data"]["signals"].is_array() &&
                  page2["data"].value("has_more", false),
              "next page continues without overlap");
        Check(page2["data"]["signals"][0].value("signal_id", -1) == 2,
              "cursor resumes at right offset");

        // 未知 artifact -> 404。
        GetJson("127.0.0.1", server.Port(), "/api/v1/waves/wave-missing/signals", "test-token",
                status);
        Check(status == 404, "unknown wave artifact returns 404");

        // 坏 cursor -> 400。
        GetJson("127.0.0.1", server.Port(), "/api/v1/waves/wave-main/signals?cursor=bogus",
                "test-token", status);
        Check(status == 400, "invalid cursor returns 400");

        // POST /waves/{a}/query：初值 + transitions。
        const eda::Json query = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/waves/wave-main/query", "test-token",
            eda::Json{{"start_tick", 2}, {"end_tick", 5}, {"signals", eda::Json::array({0, 1})}},
            status);
        Check(status == 200, "POST /waves/{a}/query returns 200");
        Check(query["data"].value("completeness", std::string()) == "exact",
              "full query is exact");
        Check(query["data"]["signals"].size() == 2, "query returns requested signals");
        Check(query["data"]["signals"][0]["initial_value"] == "0",
              "initial_value is value just before start_tick");
        Check(query["data"]["signals"][0]["transitions"].size() == 4,
              "transitions within [start,end] inclusive");
        Check(query["data"]["signals"][0]["transitions"][0].value("tick", std::string()) == "2",
              "tick rendered as decimal string");
        Check(query["data"]["signals"][1]["initial_value"].is_null(),
              "signal without prior value has null initial_value");

        // start_tick=0 -> 无前值。
        const eda::Json fromZero = PostJson(
            "127.0.0.1", server.Port(), "/api/v1/waves/wave-main/query", "test-token",
            eda::Json{{"start_tick", 0}, {"end_tick", 3}, {"signals", eda::Json::array({0})}},
            status);
        Check(status == 200 && fromZero["data"]["signals"][0]["initial_value"].is_null(),
              "start_tick=0 has no initial value");

        // end < start -> 400。
        PostJson("127.0.0.1", server.Port(), "/api/v1/waves/wave-main/query", "test-token",
                 eda::Json{{"start_tick", 9}, {"end_tick", 3}}, status);
        Check(status == 400, "end_tick before start_tick rejected (400)");

        // 坏 tick -> 400。
        PostJson("127.0.0.1", server.Port(), "/api/v1/waves/wave-main/query", "test-token",
                 eda::Json{{"signals", eda::Json::array({0})}}, status);
        Check(status == 400, "missing tick rejected (400)");

        // 坏 cursor -> 400。
        PostJson("127.0.0.1", server.Port(), "/api/v1/waves/wave-main/query", "test-token",
                 eda::Json{{"start_tick", 0}, {"end_tick", 3}, {"cursor", "bogus"}}, status);
        Check(status == 400, "invalid query cursor rejected (400)");

        // artifact 删除 -> 410。
        fs::remove(waveFile);
        GetJson("127.0.0.1", server.Port(), "/api/v1/waves/wave-main/signals", "test-token",
                status);
        Check(status == 410, "removed wave artifact returns 410");
    }

    server.Stop();
    Check(!server.Running(), "gateway stops cleanly");
    fs::remove_all(snapshotProject, cleanupError);
    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
