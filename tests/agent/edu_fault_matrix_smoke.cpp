// NG-10 / NG-06 故障矩阵（非 GUI 部分）：真实 loopback HTTP + Fake IJobService。
//
// 覆盖（每个场景至少一条断言，全部基于真实路由上"实际观察到"的响应）：
//   A. 两个工程并发、互不串数据（快照/grant/job/artifact/跨工程快照复用）
//   B. 产物失效（artifact + wave 文件被删除后不得命中旧缓存）
//   C. 取消语义（只影响目标 Job；重复取消无第二次副作用；未知 Job 404）
//   D. 响应丢失 / 重复提交（同 Idempotency-Key 同内容只建一个 Job；不同内容 409）
//   E. 两个工程并发只读压力（context/state/events，无串扰；/health 仍 200）
//
// 只使用既有生产代码（eda_agent_gateway + httplib 客户端 + eda::Json）。
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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& message) {
    if (ok) {
        std::cout << "  ok: " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << message << "\n";
    }
}

// 仅供报告使用的说明行（不参与通过/失败判定）。
void Note(const std::string& message) { std::cout << "  note: " << message << "\n"; }

// 计数型假 Job 服务：记录真实 submit/cancel 次数，用于验证"被拒请求从未到达 Job 服务"
// 以及"重复取消不产生第二次副作用"。
class FakeJobService final : public eda::IJobService {
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
        ++cancelCount_;
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
        std::lock_guard<std::mutex> lock(mutex_);
        eda::JobReport report;
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
    int CancelCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelCount_;
    }

private:
    mutable std::mutex mutex_;
    std::atomic<int> sequence_{0};
    int submitCount_ = 0;
    int cancelCount_ = 0;
    std::map<std::string, eda::JobRecord> records_;
};

// 内存波形后端（不解析真实 VCD；信号集合固定，便于验证 wave 失效语义）。
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
        if (signalId != 0) return true;
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

std::string ErrorCode(const eda::Json& body) {
    return body.contains("error") ? body["error"].value("code", std::string()) : std::string();
}

httplib::Headers Idempotency(const std::string& key) {
    httplib::Headers headers;
    headers.emplace("Idempotency-Key", key);
    return headers;
}

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
    std::error_code cleanupError;
    const fs::path root = fs::temp_directory_path() /
                          ("sigflow_fault_matrix_" + std::to_string(
                              std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fs::remove_all(root, cleanupError);

    const fs::path projectA = root / "proj a";  // 含空格，顺带覆盖非纯 ASCII 路径边界
    const fs::path projectB = root / "proj-b";
    const std::string manifest = R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/top.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON";
    // A/B 源码内容不同，确保两个工程的输入指纹/快照内容确实不同。
    const std::pair<fs::path, std::string> projects[] = {
        {projectA, "module top; endmodule\n"},
        {projectB, "module top; wire b_only_marker; endmodule\n"},
    };
    for (const auto& entry : projects) {
        fs::create_directories(entry.first / "rtl");
        std::ofstream(entry.first / "rtl" / "top.v", std::ios::binary) << entry.second;
        std::ofstream(entry.first / "sigflow.project", std::ios::binary) << manifest;
    }

    const std::string token = "matrix-token";
    const std::string uiToken = "matrix-ui-token";

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-fault-matrix";
    config.edition = "edu";
    config.token = token;
    config.uiToken = uiToken;
    config.port = 0;  // 系统分配

    const auto provider = []() {
        std::vector<eda::agent::ReadyPlugin> ready;
        ready.push_back({"eda-synth-yosys", "1.0.0", {"synth"}});
        ready.push_back({"eda-sim-verilator", "1.0.0", {"sim.build", "sim.run"}});
        return ready;
    };

    eda::agent::GatewayServer server(config, provider);
    FakeJobService jobs;
    server.SetJobServiceProvider([&jobs]() -> eda::IJobService* { return &jobs; });
    server.SetWaveformBackendFactory([]() -> std::shared_ptr<eda::IWaveformBackend> {
        return std::make_shared<FakeWaveBackend>();
    });

    std::string error;
    std::string idA;
    std::string idB;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectA, false, true, idA, error),
          "A: project context built from sigflow.project");
    Check(server.RefreshProjectSnapshotStateFromDisk(projectB, false, true, idB, error),
          "B: project context built from sigflow.project");
    Check(!idA.empty() && !idB.empty() && idA != idB,
          "A/B: distinct opaque project ids (" + idA + " vs " + idB + ")");
    Check(idA.rfind("project-", 0) == 0 && idB.rfind("project-", 0) == 0,
          "A/B: ids use the project- prefix");

    Check(server.Start(error), "gateway starts and binds loopback");
    Check(server.Port() > 0, "gateway got a system-assigned port");
    if (!server.Running()) {
        std::cout << "start error: " << error << "\n";
        std::cout << "FAILURES\n";
        return 1;
    }
    const int port = server.Port();
    const std::string host = "127.0.0.1";
    server.RunAsync();
    // 轮询 /health 直到监听就绪（最多 ~3 s），避免固定 sleep 造成的首个请求偶发失败。
    int readyStatus = 0;
    bool ready = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        GetJson(host, port, "/api/v1/health", "", readyStatus);
        if (readyStatus == 200) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Check(ready, "gateway answers /health after start (observed " + std::to_string(readyStatus) +
                     ")");
    int status = 0;

    // ==================================================================
    // A. 两个工程并发、互不串数据
    // ==================================================================
    std::cout << "\n[A] cross-project isolation\n";

    std::string revA;
    std::string revB;
    {
        const eda::Json contextA = GetJson(host, port, "/api/v1/projects/" + idA + "/context",
                                           token, status);
        Check(status == 200 && contextA["data"].value("project_id", std::string()) == idA,
              "A.1 GET /projects/A/context returns 200 with matching project_id");
        revA = contextA["data"].value("revision", std::string());
        const eda::Json contextB = GetJson(host, port, "/api/v1/projects/" + idB + "/context",
                                           token, status);
        Check(status == 200 && contextB["data"].value("project_id", std::string()) == idB,
              "A.1 GET /projects/B/context returns 200 with matching project_id");
        revB = contextB["data"].value("revision", std::string());
        Check(!revA.empty() && !revB.empty() && revA != revB,
              "A.1 each project carries its own revision (" + revA + " / " + revB + ")");
        const std::string sourceAId = contextA["data"]["sources"][0].value("source_id", std::string());
        const std::string sourceASha = contextA["data"]["sources"][0].value("sha256", std::string());
        const std::string sourceBSha = contextB["data"]["sources"][0].value("sha256", std::string());
        Check(!sourceAId.empty() && !sourceASha.empty() && !sourceBSha.empty() &&
                  sourceASha != sourceBSha,
              "A.1 A and B register the same relative path with different content hashes");
        Note("source ids are derived from the project-relative path only, so two projects with the "
             "same layout share a source id; isolation is therefore verified on the returned "
             "project/hash/text rather than on the id being unknown.");
        const eda::Json crossSource = GetJson(
            host, port, "/api/v1/projects/" + idB + "/sources/" + sourceAId, token, status);
        Check(status == 200, "A.1 GET /projects/B/sources/{id} returns 200 (observed " +
                                 std::to_string(status) + ")");
        Check(crossSource["data"].value("project_id", std::string()) == idB &&
                  crossSource["data"].value("revision", std::string()) == revB &&
                  crossSource["data"].value("file_hash", std::string()) == sourceBSha,
              "A.1 the read through B reports B's project/revision/hash, never A's");
        Check(crossSource["data"].value("text", std::string()).find("b_only_marker") !=
                  std::string::npos,
              "A.1 the read through B returns B's own file text (A's file is not served)");
        const eda::Json unknownSource = GetJson(
            host, port, "/api/v1/projects/" + idB + "/sources/source-000000000000000000000000",
            token, status);
        Check(status == 404, "A.1 an unknown source id returns 404 in project B (observed " +
                                 std::to_string(status) + ")");
        Check(!unknownSource.contains("data"), "A.1 unknown-source failure carries no data payload");
    }

    std::string snapA;
    std::string snapB;
    {
        const eda::Json createdA = PostJson(
            host, port, "/api/v1/projects/" + idA + "/snapshots", token,
            eda::Json::object({{"expected_revision", revA}, {"require_saved", true}}), status);
        Check(status == 201, "A.2 snapshot A created (observed " + std::to_string(status) + ")");
        snapA = createdA["data"].value("snapshot_id", std::string());
        const eda::Json createdB = PostJson(
            host, port, "/api/v1/projects/" + idB + "/snapshots", token,
            eda::Json::object({{"expected_revision", revB}, {"require_saved", true}}), status);
        Check(status == 201, "A.2 snapshot B created (observed " + std::to_string(status) + ")");
        snapB = createdB["data"].value("snapshot_id", std::string());
        Check(!snapA.empty() && !snapB.empty() && snapA != snapB,
              "A.2 snapshots are distinct per project (" + snapA + " / " + snapB + ")");
    }

    std::string grantA;
    std::string grantB;
    {
        const eda::Json issuedA = PostJson(
            host, port, "/api/v1/projects/" + idA + "/grants", uiToken,
            eda::Json::object({{"plan_hash", "plan-matrix-a"},
                               {"revision", revA},
                               {"snapshot_id", snapA},
                               {"max_jobs", 4}}),
            status);
        Check(status == 201, "A.3 grant for A issued (observed " + std::to_string(status) + ")");
        grantA = issuedA["data"].value("grant_id", std::string());
        const eda::Json issuedB = PostJson(
            host, port, "/api/v1/projects/" + idB + "/grants", uiToken,
            eda::Json::object({{"plan_hash", "plan-matrix-b"},
                               {"revision", revB},
                               {"snapshot_id", snapB},
                               {"max_jobs", 2}}),
            status);
        Check(status == 201, "A.3 grant for B issued (observed " + std::to_string(status) + ")");
        grantB = issuedB["data"].value("grant_id", std::string());
        Check(grantA != grantB && !grantA.empty() && !grantB.empty(),
              "A.3 the two projects hold different grants");
    }

    const int submitsBeforeCross = jobs.SubmitCount();
    {
        // A 的 grant 用在 B 上：快照/revision 合法，只有 grant 归属不符。
        const eda::Json body = eda::Json::object({{"snapshot_id", snapB},
                                                  {"expected_revision", revB},
                                                  {"capability", "eda.synth"},
                                                  {"grant_id", grantA},
                                                  {"params", eda::Json::object({{"top_module", "top"}})}});
        const eda::Json rejected = PostJson(host, port, "/api/v1/projects/" + idB + "/jobs", token,
                                            body, status, Idempotency("ng10-cross-grant-1"));
        Check(status == 403, "A.3 POST /projects/B/jobs with A's grant rejected (observed " +
                                 std::to_string(status) + ")");
        Check(ErrorCode(rejected) == "POLICY_DENIED",
              "A.3 cross-project grant rejection carries POLICY_DENIED (observed '" +
                  ErrorCode(rejected) + "')");
        Check(jobs.SubmitCount() == submitsBeforeCross,
              "A.3 rejected cross-project request never reached the Job service (submits=" +
                  std::to_string(jobs.SubmitCount()) + ")");
    }

    std::string jobA;
    {
        const eda::Json body = eda::Json::object({{"snapshot_id", snapA},
                                                  {"expected_revision", revA},
                                                  {"capability", "eda.synth"},
                                                  {"grant_id", grantA},
                                                  {"params", eda::Json::object({{"top_module", "top"}})}});
        const eda::Json submitted = PostJson(host, port, "/api/v1/projects/" + idA + "/jobs", token,
                                             body, status, Idempotency("ng10-a-job-1"));
        Check(status == 202, "A.3 POST /projects/A/jobs accepted (observed " +
                                 std::to_string(status) + ")");
        jobA = submitted["data"].value("job_id", std::string());
        Check(!jobA.empty(), "A.3 accepted submission returns a job id (" + jobA + ")");
        Check(jobs.SubmitCount() == submitsBeforeCross + 1,
              "A.3 the Fake service recorded exactly one job (submits=" +
                  std::to_string(jobs.SubmitCount()) + ")");

        const eda::Json got = GetJson(host, port, "/api/v1/jobs/" + jobA, token, status);
        Check(status == 200 && got["data"].value("job_id", std::string()) == jobA,
              "A.3 GET /jobs/{A's job} is visible (observed " + std::to_string(status) + ")");
        Check(got["data"].value("project_id", std::string()) == idA,
              "A.3 the job is attributed to project A");

        const eda::Json listedB = GetJson(host, port, "/api/v1/projects/" + idB + "/jobs", token,
                                          status);
        Check(status == 200, "A.3 GET /projects/B/jobs returns 200");
        bool leaked = false;
        for (const auto& item : listedB["data"]["jobs"]) {
            if (item.value("job_id", std::string()) == jobA) leaked = true;
        }
        Check(!leaked && listedB["data"].value("total", -1) == 0,
              "A.3 project B's job list does not expose A's job (total=" +
                  std::to_string(listedB["data"].value("total", -1)) + ")");
    }

    const std::string artifactALive = "artifact-ng10-a-live";
    {
        const fs::path artifactFile = projectA / "artifacts" / "report.json";
        fs::create_directories(artifactFile.parent_path());
        {
            std::ofstream out(artifactFile, std::ios::binary | std::ios::trunc);
            out << R"({"ok":true})";
            out.flush();
        }
        eda::agent::ArtifactRecord record;
        record.artifactId = artifactALive;
        record.projectId = idA;
        record.revision = revA;
        record.jobId = jobA;
        record.path = artifactFile;
        record.schema = "eda.report.json.v1";
        record.sha256 = eda::platform::Sha256FileHex(artifactFile);
        record.role = "primary";
        server.RegisterArtifact(record);

        const eda::Json meta = GetJson(host, port, "/api/v1/artifacts/" + artifactALive, token,
                                       status);
        Check(status == 200, "A.4 GET /artifacts/{A's artifact} returns 200 (observed " +
                                 std::to_string(status) + ")");
        Check(meta["data"].value("project_id", std::string()) == idA && !meta["data"].contains("path"),
              "A.4 artifact metadata is bound to A and leaks no local path");
        GetJson(host, port, "/api/v1/artifacts/artifact-ng10-missing", token, status);
        Check(status == 404, "A.4 nonexistent artifact id returns 404 (observed " +
                                 std::to_string(status) + ")");

        // B 的项目作用域路由不得泄漏 A 的 artifact id。
        const std::string scopedRoutes[] = {
            "/api/v1/projects/" + idB + "/context",
            "/api/v1/projects/" + idB + "/state",
            "/api/v1/projects/" + idB + "/jobs",
            "/api/v1/projects/" + idB + "/events?after=0&wait_ms=0",
            "/api/v1/projects/" + idB + "/legacy/jobs",
        };
        bool exposed = false;
        for (const std::string& route : scopedRoutes) {
            const eda::Json body = GetJson(host, port, route, token, status);
            if (status == 200 && body.dump().find(artifactALive) != std::string::npos) exposed = true;
        }
        Check(!exposed,
              "A.4 no /projects/B/... route exposes A's artifact id");
        GetJson(host, port, "/api/v1/projects/" + idB + "/artifacts/" + artifactALive, token,
                status);
        Check(status == 404,
              "A.4 there is no project-scoped artifact route (observed " + std::to_string(status) +
                  ")");
        Note("artifact reads are addressed only by the global /artifacts/{id} route; the route "
             "itself is not project-scoped, so isolation is asserted via project-scoped routes "
             "and the artifact's own project_id.");
    }

    {
        // A 的 snapshot_id 用在 B 上：必须被拒，且不得新建 Job。
        const int before = jobs.SubmitCount();
        const eda::Json body = eda::Json::object({{"snapshot_id", snapA},
                                                  {"expected_revision", revB},
                                                  {"capability", "eda.synth"},
                                                  {"grant_id", grantB},
                                                  {"params", eda::Json::object({{"top_module", "top"}})}});
        const eda::Json rejected = PostJson(host, port, "/api/v1/projects/" + idB + "/jobs", token,
                                            body, status, Idempotency("ng10-cross-snapshot-1"));
        Check(status == 410, "A.5 A's snapshot cannot be reused under project B (observed " +
                                 std::to_string(status) + ")");
        Check(ErrorCode(rejected) == "ARTIFACT_EXPIRED",
              "A.5 the foreign snapshot id is reported as expired for B (observed '" +
                  ErrorCode(rejected) + "')");
        Check(jobs.SubmitCount() == before,
              "A.5 rejected cross-project snapshot reuse created no job (submits=" +
                  std::to_string(jobs.SubmitCount()) + ")");

        // B 自身的正常路径仍然可用（拒绝没有破坏 B 的可用性）。
        const eda::Json okBody = eda::Json::object({{"snapshot_id", snapB},
                                                    {"expected_revision", revB},
                                                    {"capability", "eda.synth"},
                                                    {"grant_id", grantB},
                                                    {"params", eda::Json::object({{"top_module", "top"}})}});
        const eda::Json submitted = PostJson(host, port, "/api/v1/projects/" + idB + "/jobs", token,
                                             okBody, status, Idempotency("ng10-b-job-1"));
        Check(status == 202, "A.5 project B still accepts its own snapshot (observed " +
                                 std::to_string(status) + ")");
        Check(!submitted["data"].value("job_id", std::string()).empty(),
              "A.5 project B's job got an id");
    }

    // ==================================================================
    // B. 产物失效（无陈旧缓存）
    // ==================================================================
    std::cout << "\n[B] artifact expiry\n";
    {
        const fs::path liveFile = projectA / "artifacts" / "ng10-live.json";
        fs::create_directories(liveFile.parent_path());
        std::ofstream(liveFile, std::ios::binary | std::ios::trunc) << R"({"live":1})";

        eda::agent::ArtifactRecord record;
        record.artifactId = "artifact-ng10-live";
        record.projectId = idA;
        record.revision = revA;
        record.jobId = jobA;
        record.path = liveFile;
        record.schema = "eda.report.json.v1";
        record.sha256 = eda::platform::Sha256FileHex(liveFile);
        record.role = "primary";
        server.RegisterArtifact(record);

        const eda::Json content = GetJson(
            host, port, "/api/v1/artifacts/artifact-ng10-live/content", token, status);
        Check(status == 200 && content["data"].value("content", std::string()) == R"({"live":1})",
              "B.1 GET /artifacts/{a}/content returns 200 with the file content");

        fs::remove(liveFile);
        const eda::Json afterDelete = GetJson(
            host, port, "/api/v1/artifacts/artifact-ng10-live/content", token, status);
        Check(status == 410, "B.2 deleted artifact content returns 410 (observed " +
                                 std::to_string(status) + ", not a cached 200)");
        Check(ErrorCode(afterDelete) == "ARTIFACT_EXPIRED",
              "B.2 deleted artifact content carries ARTIFACT_EXPIRED (observed '" +
                  ErrorCode(afterDelete) + "')");
        Check(!afterDelete.contains("data") ||
                  afterDelete["data"].value("content", std::string()).empty(),
              "B.2 the 410 body does not smuggle stale content");
        const eda::Json metaGone = GetJson(host, port, "/api/v1/artifacts/artifact-ng10-live",
                                           token, status);
        Check(status == 410 && ErrorCode(metaGone) == "ARTIFACT_EXPIRED",
              "B.2 deleted artifact metadata is also 410 ARTIFACT_EXPIRED (observed " +
                  std::to_string(status) + ")");
    }
    {
        const fs::path waveFile = projectA / "sim" / "ng10-wave.vcd";
        fs::create_directories(waveFile.parent_path());
        std::ofstream(waveFile, std::ios::binary | std::ios::trunc) << "$timescale 1ns $end\n";

        eda::agent::WaveArtifactRecord record;
        record.artifactId = "wave-ng10-live";
        record.projectId = idA;
        record.revision = revA;
        record.jobId = jobA;
        record.path = waveFile;
        record.sha256 = eda::platform::Sha256FileHex(waveFile);
        record.schema = "eda.wave.vcd.v1";
        server.RegisterWaveArtifact(record);

        const eda::Json signals = GetJson(host, port, "/api/v1/waves/wave-ng10-live/signals",
                                          token, status);
        Check(status == 200 && signals["data"]["signals"].is_array() &&
                  signals["data"]["signals"].size() == 5,
              "B.3 GET /waves/{a}/signals returns 200 with the 5 registered signals");

        fs::remove(waveFile);
        const eda::Json gone = GetJson(host, port, "/api/v1/waves/wave-ng10-live/signals", token,
                                       status);
        Check(status == 410, "B.3 deleted wave file returns 410 (observed " +
                                 std::to_string(status) + ")");
        Check(ErrorCode(gone) == "ARTIFACT_EXPIRED",
              "B.3 deleted wave file carries ARTIFACT_EXPIRED (observed '" + ErrorCode(gone) + "')");
    }
    {
        eda::agent::ArtifactRecord ghost;
        ghost.artifactId = "artifact-ng10-ghost";
        ghost.projectId = idA;
        ghost.revision = revA;
        ghost.jobId = jobA;
        // 该路径从未存在过（父目录也不存在）。
        ghost.path = projectA / "artifacts" / "never-created" / "ghost.json";
        ghost.schema = "eda.report.json.v1";
        ghost.sha256 = "0";  // 故意与任何真实内容都不同
        ghost.role = "primary";
        server.RegisterArtifact(ghost);

        const eda::Json meta = GetJson(host, port, "/api/v1/artifacts/artifact-ng10-ghost", token,
                                       status);
        Check(status == 410, "B.4 never-existing artifact metadata fails explicitly (observed " +
                                 std::to_string(status) + ")");
        Check(ErrorCode(meta) == "ARTIFACT_EXPIRED",
              "B.4 never-existing artifact metadata carries ARTIFACT_EXPIRED (observed '" +
                  ErrorCode(meta) + "')");
        const eda::Json content = GetJson(
            host, port, "/api/v1/artifacts/artifact-ng10-ghost/content", token, status);
        Check(status == 410, "B.4 never-existing artifact content fails explicitly (observed " +
                                 std::to_string(status) + ")");
        Check(status != 200 && !content.contains("data"),
              "B.4 never-existing artifact is never a 200 with empty content");
    }

    // ==================================================================
    // C. 取消语义（只影响目标 run）
    // ==================================================================
    std::cout << "\n[C] cancel semantics\n";
    std::string cJob1;
    std::string cJob2;
    {
        const eda::Json grant = PostJson(
            host, port, "/api/v1/projects/" + idB + "/grants", uiToken,
            eda::Json::object({{"plan_hash", "plan-matrix-c"},
                               {"revision", revB},
                               {"snapshot_id", snapB},
                               {"max_jobs", 3}}),
            status);
        Check(status == 201, "C.1 grant for the cancel scenario issued");
        const std::string grantC = grant["data"].value("grant_id", std::string());

        const auto submitCancelJob = [&](const std::string& key, std::string& outJobId) {
            const eda::Json body = eda::Json::object(
                {{"snapshot_id", snapB},
                 {"expected_revision", revB},
                 {"capability", "eda.synth"},
                 {"grant_id", grantC},
                 {"params", eda::Json::object({{"top_module", "top"}})}});
            const eda::Json submitted = PostJson(host, port, "/api/v1/projects/" + idB + "/jobs",
                                                 token, body, status, Idempotency(key));
            Check(status == 202, "C.1 job submitted under key " + key + " (observed " +
                                     std::to_string(status) + ")");
            outJobId = submitted["data"].value("job_id", std::string());
        };
        submitCancelJob("ng10-c-job-1", cJob1);
        submitCancelJob("ng10-c-job-2", cJob2);
        Check(!cJob1.empty() && !cJob2.empty() && cJob1 != cJob2,
              "C.1 two distinct jobs exist (" + cJob1 + ", " + cJob2 + ")");
    }

    const int submitCountBeforeCancel = jobs.SubmitCount();
    const int cancelCallsBefore = jobs.CancelCount();
    {
        const eda::Json cancelled = PostJson(host, port, "/api/v1/jobs/" + cJob1 + "/cancel", token,
                                             eda::Json::object(), status);
        Check(status == 200, "C.2 cancel of job #1 accepted (observed " + std::to_string(status) +
                                 ")");
        Check(cancelled["data"].value("accepted", false),
              "C.2 the payload only means 'accepted' (accepted=true)");
        Check(cancelled["data"].value("job_id", std::string()) == cJob1,
              "C.2 the accepted payload echoes the target job id");
        Check(jobs.CancelCount() == cancelCallsBefore + 1,
              "C.2 exactly one cancel call reached the Job service");

        const eda::Json state1 = GetJson(host, port, "/api/v1/jobs/" + cJob1, token, status);
        Check(status == 200 && state1["data"].value("state", std::string()) == "Cancelled",
              "C.2 job #1 is Cancelled (observed '" +
                  state1["data"].value("state", std::string()) + "')");
        const eda::Json state2 = GetJson(host, port, "/api/v1/jobs/" + cJob2, token, status);
        Check(status == 200 && state2["data"].value("state", std::string()) == "Queued",
              "C.2 job #2 is still queryable and NOT cancelled (observed '" +
                  state2["data"].value("state", std::string()) + "')");
    }
    {
        const eda::Json listed = GetJson(host, port, "/api/v1/projects/" + idB + "/jobs", token,
                                        status);
        const int totalBefore = listed["data"].value("total", -1);
        const eda::Json again = PostJson(host, port, "/api/v1/jobs/" + cJob1 + "/cancel", token,
                                        eda::Json::object(), status);
        const int secondCancelStatus = status;
        Check(secondCancelStatus == 200,
              "C.3 second cancel returns the observed status without crashing (observed " +
                  std::to_string(secondCancelStatus) + ")");
        Check(again["data"].value("accepted", false),
              "C.3 the repeated cancel is still only an acknowledgement");
        Note("second cancel observed HTTP " + std::to_string(secondCancelStatus) +
             " (the in-memory Fake service reports the job as cancellable again; a real service "
             "may answer 409 for a terminal job).");
        Check(jobs.SubmitCount() == submitCountBeforeCancel,
              "C.3 second cancel created no new job (submits=" +
                  std::to_string(jobs.SubmitCount()) + ")");
        const eda::Json listedAgain = GetJson(host, port, "/api/v1/projects/" + idB + "/jobs",
                                              token, status);
        Check(listedAgain["data"].value("total", -1) == totalBefore,
              "C.3 job count is unchanged after the second cancel (" +
                  std::to_string(listedAgain["data"].value("total", -1)) + ")");
        Check(jobs.CancelCount() == cancelCallsBefore + 2,
              "C.3 both cancel calls reached the service, yet no second effect was created");
        const eda::Json state1 = GetJson(host, port, "/api/v1/jobs/" + cJob1, token, status);
        Check(state1["data"].value("state", std::string()) == "Cancelled",
              "C.3 the target job stays Cancelled after a repeated cancel");
    }
    {
        GetJson(host, port, "/api/v1/jobs/job-ng10-unknown/cancel", token, status);
        // cancel 是 POST 路由：GET 不得命中 -> 404；再用 POST 验证未知 id。
        Check(status == 404, "C.4 GET on the cancel route is 404 (observed " +
                                 std::to_string(status) + ")");
        const eda::Json unknown = PostJson(host, port, "/api/v1/jobs/job-ng10-unknown/cancel",
                                           token, eda::Json::object(), status);
        Check(status == 404, "C.4 cancel of an unknown job id returns 404 (observed " +
                                 std::to_string(status) + ")");
        Check(ErrorCode(unknown) == "NOT_FOUND", "C.4 unknown-job cancel carries NOT_FOUND");

        PostJson(host, port, "/api/v1/projects/" + idB + "/jobs/" + cJob1 + "/cancel", token,
                 eda::Json::object(), status);
        Check(status == 404,
              "C.4 no project-scoped cancel route exists (observed " + std::to_string(status) + ")");
        Note("cross-project cancel cannot be driven through a real route: POST "
             "/jobs/{jobId}/cancel carries no project id, and the only project-scoped job route "
             "is GET /projects/{p}/jobs. Ownership is enforced when the job is created "
             "(grant -> project) and when it is listed, not on cancel.");
    }

    // ==================================================================
    // D. 响应丢失 / 重复提交
    // ==================================================================
    std::cout << "\n[D] lost response / duplicate submit\n";
    {
        const int before = jobs.SubmitCount();
        const eda::Json body = eda::Json::object({{"snapshot_id", snapA},
                                                  {"expected_revision", revA},
                                                  {"capability", "eda.synth"},
                                                  {"grant_id", grantA},
                                                  {"params", eda::Json::object({{"top_module", "top"}})}});
        const eda::Json first = PostJson(host, port, "/api/v1/projects/" + idA + "/jobs", token,
                                         body, status, Idempotency("ng10-idem-1"));
        Check(status == 202, "D.1 first submit accepted (observed " + std::to_string(status) + ")");
        const std::string firstJob = first["data"].value("job_id", std::string());
        const eda::Json replay = PostJson(host, port, "/api/v1/projects/" + idA + "/jobs", token,
                                          body, status, Idempotency("ng10-idem-1"));
        Check(status == 202 && replay["data"].value("job_id", std::string()) == firstJob,
              "D.1 same key + identical body replays the same job id (observed " +
                  std::to_string(status) + ")");
        Check(replay["data"].value("replayed", false),
              "D.1 the replay is explicitly marked replayed=true");
        Check(jobs.SubmitCount() == before + 1,
              "D.1 exactly ONE job reached the Job service (submits=" +
                  std::to_string(jobs.SubmitCount()) + ")");

        eda::Json changed = body;
        changed["params"] = eda::Json::object({{"top_module", "top"}, {"strategy", "area"}});
        const eda::Json conflict = PostJson(host, port, "/api/v1/projects/" + idA + "/jobs", token,
                                            changed, status, Idempotency("ng10-idem-1"));
        Check(status == 409, "D.2 same key + different body returns 409 (observed " +
                                 std::to_string(status) + ")");
        Check(ErrorCode(conflict) == "IDEMPOTENCY_CONFLICT",
              "D.2 different body carries IDEMPOTENCY_CONFLICT (observed '" +
                  ErrorCode(conflict) + "')");
        Check(jobs.SubmitCount() == before + 1,
              "D.2 the conflicting request created no job (submits=" +
                  std::to_string(jobs.SubmitCount()) + ")");
    }

    // ==================================================================
    // E. 两个工程并发只读压力（轻量）
    // ==================================================================
    std::cout << "\n[E] concurrent read stress across both projects\n";
    {
        server.PublishEvent("job/created", idA, "trace-matrix-a", eda::Json::object({{"job_id", jobA}}));
        server.PublishEvent("snapshot/created", idA, "trace-matrix-a2",
                            eda::Json::object({{"snapshot_id", snapA}}));
        server.PublishEvent("job/created", idB, "trace-matrix-b", eda::Json::object({{"job_id", cJob1}}));

        struct StressResult {
            int requests = 0;
            int mismatches = 0;
            std::string firstProblem;
        };
        constexpr int kThreads = 4;
        constexpr int kIterations = 6;
        std::vector<StressResult> results(kThreads);
        const std::string projectIds[2] = {idA, idB};

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                StressResult& result = results[static_cast<std::size_t>(t)];
                int localStatus = 0;
                for (int i = 0; i < kIterations; ++i) {
                    // 轮换起始工程，让两个工程在时间上真正交错。
                    for (int k = 0; k < 2; ++k) {
                        const std::string& projectId =
                            projectIds[(static_cast<std::size_t>(t) + static_cast<std::size_t>(k) +
                                        static_cast<std::size_t>(i)) % 2];

                        const eda::Json context = GetJson(
                            host, port, "/api/v1/projects/" + projectId + "/context", token,
                            localStatus);
                        ++result.requests;
                        if (localStatus != 200) {
                            ++result.mismatches;
                            if (result.firstProblem.empty()) {
                                result.firstProblem = "context status " + std::to_string(localStatus);
                            }
                        } else if (context["data"].value("project_id", std::string()) != projectId) {
                            ++result.mismatches;
                            if (result.firstProblem.empty()) {
                                result.firstProblem = "context crossed project boundary";
                            }
                        }

                        const eda::Json state = GetJson(
                            host, port, "/api/v1/projects/" + projectId + "/state", token,
                            localStatus);
                        ++result.requests;
                        if (localStatus != 200) {
                            ++result.mismatches;
                            if (result.firstProblem.empty()) {
                                result.firstProblem = "state status " + std::to_string(localStatus);
                            }
                        } else if (state["data"].value("project_id", std::string()) != projectId) {
                            ++result.mismatches;
                            if (result.firstProblem.empty()) {
                                result.firstProblem = "state crossed project boundary";
                            }
                        }

                        const eda::Json events = GetJson(
                            host, port,
                            "/api/v1/projects/" + projectId + "/events?after=0&wait_ms=0", token,
                            localStatus);
                        ++result.requests;
                        if (localStatus != 200) {
                            ++result.mismatches;
                            if (result.firstProblem.empty()) {
                                result.firstProblem = "events status " + std::to_string(localStatus);
                            }
                        } else {
                            for (const auto& event : events["data"]["events"]) {
                                if (event.value("project_id", std::string()) != projectId) {
                                    ++result.mismatches;
                                    if (result.firstProblem.empty()) {
                                        result.firstProblem = "events crossed project boundary";
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            });
        }
        for (std::thread& worker : workers) worker.join();

        const int expectedRequests = kThreads * kIterations * 2 * 3;
        int totalRequests = 0;
        int totalMismatches = 0;
        std::string firstProblem;
        for (const StressResult& result : results) {
            totalRequests += result.requests;
            totalMismatches += result.mismatches;
            if (firstProblem.empty() && !result.firstProblem.empty()) {
                firstProblem = result.firstProblem;
            }
        }
        Check(totalRequests == expectedRequests,
              "E.1 all " + std::to_string(expectedRequests) + " concurrent reads completed (" +
                  std::to_string(totalRequests) + ")");
        Check(totalMismatches == 0,
              "E.1 every response carried the requested project (mismatches=" +
                  std::to_string(totalMismatches) +
                  (firstProblem.empty() ? std::string() : ", first: " + firstProblem) + ")");

        const eda::Json health = GetJson(host, port, "/api/v1/health", "", status);
        Check(status == 200 && health["data"].value("instance_id", std::string()) ==
                                   config.instanceId,
              "E.1 follow-up /health is 200 with the configured instance id");
    }

    server.Stop();
    Check(!server.Running(), "gateway stops cleanly");
    // Windows 上句柄释放略有延迟，清理做有限重试；最终仍必须干净（不清空即失败）。
    for (int attempt = 0; attempt < 3 && fs::exists(root); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        fs::remove_all(root, cleanupError);
    }
    Check(!fs::exists(root), "temp project tree is cleaned up");

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
