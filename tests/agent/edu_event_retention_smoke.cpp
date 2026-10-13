// NG-08：可信事件源与快照保留调度冒烟测试。
//
// 覆盖：
//   * 工程打开/保存/关闭、dirty 翻转、产物产生、选择变化都产生规范事件，且可经
//     GET /projects/{p}/events 用游标读到；同一事实重复上报不重复投递；
//   * 事件里不出现本机绝对路径（产物只给 opaque artifact_id）；
//   * 快照保留调度：被活跃 grant 或 Agent Job 绑定引用的快照不被清理，当前 revision 的
//     快照不被清理，只有无人引用的旧快照会被清理；撤销授权后它才可被清理；
//   * 调度按 minIntervalSeconds 节流，且清理报告逐工程给出结果（失败不假装成功）。
#include "eda-agent-gateway/GatewayServer.h"

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* message) {
    if (ok) {
        std::cout << "  ok: " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << message << "\n";
    }
}

class FakeJobService final : public eda::IJobService {
public:
    std::string submit(const eda::JobRequest& request) override {
        const std::string id = "job-" + std::to_string(++sequence_);
        eda::JobRecord record;
        record.id = id;
        record.request = request;
        record.state = eda::JobState::Running;
        records_[id] = record;
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
        return report;
    }
    void setConcurrency(std::size_t) override {}

private:
    std::atomic<int> sequence_{0};
    std::map<std::string, eda::JobRecord> records_;
};

eda::Json GetJson(int port, const std::string& path, const std::string& token, int& status) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    const auto res = client.Get(path.c_str(), headers);
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

eda::Json PostJson(int port, const std::string& path, const std::string& token,
                   const eda::Json& body, int& status, const httplib::Headers& extra = {}) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers = extra;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    const auto res = client.Post(path.c_str(), headers, body.dump(), "application/json");
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

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::size_t CountEventsOfType(const eda::Json& page, const std::string& type) {
    std::size_t count = 0;
    for (const auto& event : page["data"]["events"]) {
        if (event.value("type", std::string()) == type) ++count;
    }
    return count;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("sigflow_event_retention_" + std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::error_code cleanupError;
    fs::remove_all(projectRoot, cleanupError);
    fs::create_directories(projectRoot / "rtl");
    WriteText(projectRoot / "rtl" / "top.v", "module top; endmodule // r0\n");
    WriteText(projectRoot / "sigflow.project", R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/top.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON");

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-events";
    config.edition = "edu";
    config.token = "agent-token";
    config.uiToken = "ui-token";
    config.port = 0;
    const auto provider = []() {
        std::vector<eda::agent::ReadyPlugin> ready;
        ready.push_back({"eda-synth-yosys", "1.0.0", {"synth"}});
        return ready;
    };

    eda::agent::GatewayServer server(config, provider);
    FakeJobService fakeJobs;
    server.SetJobServiceProvider([&fakeJobs]() -> eda::IJobService* { return &fakeJobs; });
    std::string error;
    std::string projectId;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true, projectId, error),
          "gateway builds project context for the event/retention project");
    if (!server.Start(error)) {
        std::cout << "start error: " << error << "\nFAILURES\n";
        return 1;
    }
    server.RunAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int port = server.Port();
    const std::string base = "/api/v1/projects/" + projectId;

    // ---- 可信事件源 ----
    server.NotifyProjectOpened(projectId);
    server.NotifyProjectSaved(projectId);
    server.NotifyProjectSaved(projectId);  // 同一 revision 重复保存：不重复投递 revision/advanced
    server.MarkProjectDirty(projectId, true, "buffer-hash-1");
    server.MarkProjectDirty(projectId, true, "buffer-hash-1");  // 状态未翻转：不重复投递
    server.MarkProjectDirty(projectId, false);

    eda::agent::ArtifactRecord artifact;
    artifact.artifactId = "artifact-abc";
    artifact.projectId = projectId;
    artifact.jobId = "job-1";
    artifact.path = projectRoot / "rtl" / "top.v";  // 本机路径：事件里绝不能出现
    artifact.schema = "eda.synth.netlist.v1";
    artifact.sha256 = std::string(64, 'a');
    artifact.role = "primary";
    server.RegisterArtifact(artifact);

    eda::agent::SelectionContext selection;
    selection.revision = "rev-1-000000000000";
    selection.selectedNodeIds = {"node-1"};
    selection.stateVersion = 1;
    server.UpdateSelectionContext(projectId, selection);
    server.UpdateSelectionContext(projectId, selection);  // 同一选择：不重复投递
    selection.stateVersion = 2;
    server.UpdateSelectionContext(projectId, selection);

    int status = 0;
    {
        const eda::Json page = GetJson(port, base + "/events?after=0", "agent-token", status);
        Check(status == 200, "GET /events returns 200");
        Check(CountEventsOfType(page, "project/opened") == 1, "project/opened emitted once");
        Check(CountEventsOfType(page, "project/saved") == 2,
              "project/saved emitted for each save report");
        Check(CountEventsOfType(page, "revision/advanced") == 1,
              "revision/advanced is deduplicated for an unchanged revision");
        Check(CountEventsOfType(page, "project/dirty") == 2,
              "project/dirty only fires when the dirty flag actually flips");
        Check(CountEventsOfType(page, "artifact/produced") == 1,
              "artifact/produced emitted when the host registers a job artifact");
        Check(CountEventsOfType(page, "selection/changed") == 2,
              "selection/changed is deduplicated for an unchanged selection");
        Check(page.dump().find(projectRoot.string()) == std::string::npos,
              "events never leak the project root or artifact path");
        std::string previousSequence;
        bool monotonic = true;
        for (const auto& event : page["data"]["events"]) {
            const std::string sequence = event.value("sequence", std::string());
            if (!previousSequence.empty() && sequence <= previousSequence) monotonic = false;
            previousSequence = sequence;
        }
        Check(monotonic, "event sequences are strictly increasing for cursor recovery");
    }

    // ---- 快照保留调度 ----
    const auto makeSnapshot = [&](const std::string& revision, std::string& snapshotId) {
        const eda::Json body{{"expected_revision", revision}, {"require_saved", true}};
        const eda::Json response =
            PostJson(port, base + "/snapshots", "agent-token", body, status);
        if (status != 201) {
            std::cout << "  [debug] snapshot status=" << status << " revision=" << revision
                      << " body=" << response.dump() << "\n";
            return false;
        }
        snapshotId = response["data"].value("snapshot_id", std::string());
        return !snapshotId.empty();
    };
    const auto issueGrant = [&](const std::string& revision, const std::string& snapshotId,
                                std::string& grantId) {
        const eda::Json body{{"plan_hash", "ph-" + snapshotId},
                             {"revision", revision},
                             {"snapshot_id", snapshotId},
                             {"max_jobs", 3}};
        const eda::Json response = PostJson(port, base + "/grants", "ui-token", body, status);
        if (status != 201) return false;
        grantId = response["data"].value("grant_id", std::string());
        return !grantId.empty();
    };
    const auto advanceRevision = [&](const std::string& marker, std::string& revision) {
        WriteText(projectRoot / "rtl" / "top.v",
                  "module top; endmodule // " + marker + "\n");
        std::string newProjectId;
        if (!server.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true, newProjectId,
                                                        error)) {
            return false;
        }
        const eda::Json context = GetJson(port, base + "/context", "agent-token", status);
        revision = context["data"].value("revision", std::string());
        return status == 200 && !revision.empty();
    };

    std::string revision0;
    std::string revision1;
    std::string revision2;
    std::string revision3;
    std::string snapshot0;
    std::string snapshot1;
    std::string snapshot2;
    std::string snapshot3;
    std::string grant1;
    std::string grant2;
    {
        const eda::Json context = GetJson(port, base + "/context", "agent-token", status);
        revision0 = context["data"].value("revision", std::string());
    }
    Check(makeSnapshot(revision0, snapshot0), "snapshot at the first revision is created");
    Check(advanceRevision("r1", revision1) && revision1 != revision0,
          "editing a source advances the revision");
    Check(makeSnapshot(revision1, snapshot1), "snapshot at the second revision is created");
    Check(issueGrant(revision1, snapshot1, grant1), "a UI grant is issued for the second snapshot");
    {
        // 用该 grant 提交一个 Agent Job：Job 绑定必须保护 snapshot1。
        httplib::Headers idem;
        idem.emplace("Idempotency-Key", "key-retention-1");
        const eda::Json body{{"snapshot_id", snapshot1},
                             {"expected_revision", revision1},
                             {"capability", "eda.synth"},
                             {"grant_id", grant1},
                             {"params", eda::Json{{"top_module", "top"}}}};
        const eda::Json response =
            PostJson(port, base + "/jobs", "agent-token", body, status, idem);
        if (status != 202 && status != 201) {
            std::cout << "  [debug] job submit status=" << status << " body=" << response.dump()
                      << "\n";
        }
        Check(status == 202 || status == 201,
              "an Agent job is submitted against the second snapshot");
        Check(!response["data"].value("job_id", std::string()).empty(), "job id is returned");
    }
    Check(advanceRevision("r2", revision2) && revision2 != revision1,
          "a further edit advances the revision again");
    Check(makeSnapshot(revision2, snapshot2), "snapshot at the third revision is created");
    Check(issueGrant(revision2, snapshot2, grant2), "a second UI grant is issued");
    Check(advanceRevision("r3", revision3) && revision3 != revision2,
          "the current revision moves past every snapshot");
    Check(makeSnapshot(revision3, snapshot3), "snapshot at the current revision is created");

    {
        // keepRecent=0 / maxAge=0：只应清掉无人引用的 snapshot0。
        // 注入 nowEpoch：快照刚建立时真实年龄为 0，而 Prune 的语义是"年龄必须超过 maxAge"，
        // 因此用注入时间把"现在"推到 1 小时后，让清理判定确定可复现（生产传 0 用真实时间）。
        const std::uint64_t oneHourLater =
            static_cast<std::uint64_t>(std::time(nullptr)) + 3600;
        const eda::agent::GatewayServer::RetentionResult result =
            server.RunSnapshotRetention(0, 0, 0, oneHourLater);
        Check(!result.skippedByInterval, "the first retention run is not throttled");
        Check(result.projectsConsidered >= 1, "retention considered the registered project");
        if (result.snapshotsRemoved != 1) {
            std::cout << "  [debug] retention removed=" << result.snapshotsRemoved
                      << " report=" << result.report.dump() << "\n";
        }
        Check(result.snapshotsRemoved == 1,
              "retention removes exactly the unreferenced snapshot");
        Check(result.report.is_array() && !result.report.empty() &&
                  result.report[0].value("status", std::string()) == "ok",
              "retention reports a per-project result");
        const eda::agent::GatewayServer::RetentionResult second =
            server.RunSnapshotRetention(0, 0, 0, oneHourLater);
        Check(second.snapshotsRemoved == 0,
              "grant- and job-referenced snapshots plus the current revision survive pruning");
    }
    {
        const eda::agent::GatewayServer::RetentionResult throttled =
            server.RunSnapshotRetention(0, 0, 3600);
        Check(throttled.skippedByInterval,
              "retention is throttled by minIntervalSeconds (host can call it every tick)");
    }
    {
        // 撤销第二个 grant 后，它引用的快照不再受保护。
        const eda::Json response =
            PostJson(port, "/api/v1/grants/" + grant2 + "/revoke", "ui-token",
                     eda::Json::object(), status);
        if (status != 200) {
            std::cout << "  [debug] revoke status=" << status << " grant=" << grant2
                      << " body=" << response.dump() << "\n";
        }
        Check(status == 200, "the second grant is revoked");
        (void)response;
        const std::uint64_t oneHourLater =
            static_cast<std::uint64_t>(std::time(nullptr)) + 3600;
        const eda::agent::GatewayServer::RetentionResult afterRevoke =
            server.RunSnapshotRetention(0, 0, 0, oneHourLater);
        Check(afterRevoke.snapshotsRemoved == 1,
              "a snapshot referenced only by a revoked grant becomes prunable");
        const eda::agent::GatewayServer::RetentionResult finalRun =
            server.RunSnapshotRetention(0, 0, 0, oneHourLater);
        Check(finalRun.snapshotsRemoved == 0,
              "the job-referenced snapshot and the current revision stay protected");
    }

    // ---- 关工程事件 + /state 仍可重建 ----
    {
        const eda::Json state = GetJson(port, base + "/state", "agent-token", status);
        Check(status == 200, "GET /state returns 200 before closing the project");
        Check(state["data"].value("high_watermark", std::string()) != "0",
              "/state exposes a high watermark for cursor recovery");
        server.NotifyProjectClosed(projectId);
        const eda::Json page = GetJson(port, base + "/events?after=0", "agent-token", status);
        Check(status == 200, "events remain readable by cursor after the project closes");
        Check(CountEventsOfType(page, "project/closed") == 1, "project/closed emitted once");
        const eda::Json after = GetJson(port, base + "/context", "agent-token", status);
        Check(status == 404, "closing the project clears its runtime context");
        (void)after;
    }

    server.Stop();

    // ---- 重启恢复：事件落盘重放 + /state 可重建（断线/重启后不丢游标） ----
    {
        const fs::path eventLog = projectRoot / ".sigflow" / "agent" / "events.jsonl";
        fs::create_directories(eventLog.parent_path(), cleanupError);
        std::string restartProjectId;
        std::string restartError;
        std::uint64_t firstHighWatermark = 0;
        std::size_t firstEventCount = 0;
        {
            eda::agent::GatewayConfig restartConfig = config;
            restartConfig.eventLogPath = eventLog.string();
            eda::agent::GatewayServer restarted(restartConfig, provider);
            restarted.SetJobServiceProvider(
                [&fakeJobs]() -> eda::IJobService* { return &fakeJobs; });
            Check(restarted.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true,
                                                                restartProjectId, restartError),
                  "the restarted gateway rebuilds the project context from disk");
            Check(restarted.Start(restartError), "the first gateway instance starts with an event log");
            restarted.RunAsync();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const std::string restartBase = "/api/v1/projects/" + restartProjectId;
            restarted.NotifyProjectOpened(restartProjectId);
            restarted.NotifyProjectSaved(restartProjectId);
            eda::agent::SelectionContext selection;
            selection.selectedNodeIds = {"node-restart"};
            selection.stateVersion = 1;
            restarted.UpdateSelectionContext(restartProjectId, selection);
            int restartStatus = 0;
            const eda::Json state =
                GetJson(restarted.Port(), restartBase + "/state", "agent-token", restartStatus);
            Check(restartStatus == 200, "the first instance exposes /state");
            firstHighWatermark = 0;
            try {
                firstHighWatermark =
                    std::stoull(state["data"].value("high_watermark", std::string("0")));
            } catch (const std::exception&) {
                firstHighWatermark = 0;
            }
            const eda::Json page =
                GetJson(restarted.Port(), restartBase + "/events?after=0", "agent-token",
                        restartStatus);
            firstEventCount = page["data"]["events"].size();
            Check(firstHighWatermark > 0 && firstEventCount > 0,
                  "the first instance publishes events with a high watermark");
            restarted.Stop();
        }
        {
            // 第二个实例使用同一个事件日志：启动时必须重放，游标与 /state 可重建。
            eda::agent::GatewayConfig replayConfig = config;
            replayConfig.eventLogPath = eventLog.string();
            eda::agent::GatewayServer replayed(replayConfig, provider);
            replayed.SetJobServiceProvider([&fakeJobs]() -> eda::IJobService* { return &fakeJobs; });
            std::string replayedProjectId;
            Check(replayed.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true,
                                                               replayedProjectId, restartError),
                  "the second instance rebuilds the same project id");
            Check(replayedProjectId == restartProjectId,
                  "project id is stable across gateway restarts");
            Check(replayed.Start(restartError),
                  "the second instance starts and replays the event log");
            replayed.RunAsync();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const std::string replayBase = "/api/v1/projects/" + replayedProjectId;
            int replayStatus = 0;
            const eda::Json page =
                GetJson(replayed.Port(), replayBase + "/events?after=0", "agent-token",
                        replayStatus);
            Check(replayStatus == 200, "replayed events are readable after restart");
            Check(page["data"]["events"].size() >= firstEventCount,
                  "the replayed event log keeps at least the pre-restart events");
            Check(CountEventsOfType(page, "project/opened") >= 1,
                  "project/opened survives the restart");
            const eda::Json state =
                GetJson(replayed.Port(), replayBase + "/state", "agent-token", replayStatus);
            std::uint64_t replayedHighWatermark = 0;
            try {
                replayedHighWatermark =
                    std::stoull(state["data"].value("high_watermark", std::string("0")));
            } catch (const std::exception&) {
                replayedHighWatermark = 0;
            }
            Check(state["data"].value("oldest_sequence", std::string()) != "",
                  "/state exposes oldest_sequence so a client can detect cursor expiry");
            Check(replayedHighWatermark >= firstHighWatermark,
                  "/state high watermark is rebuilt from the replayed log");
            // 重启前拿到的游标在重启后仍然可用（不丢事件、不重放旧事件）。
            // 注意：/events 默认长轮询 20 s；"确认没有新事件"必须显式 wait_ms=0 立即返回。
            const eda::Json continuation =
                GetJson(replayed.Port(),
                        replayBase + "/events?after=" + std::to_string(firstHighWatermark) +
                            "&wait_ms=0",
                        "agent-token", replayStatus);
            if (replayStatus != 200) {
                std::cout << "  [debug] continuation status=" << replayStatus
                          << " hw=" << firstHighWatermark
                          << " body=" << continuation.dump() << "\n";
            }
            Check(replayStatus == 200 && continuation["data"]["events"].is_array(),
                  "a cursor taken before the restart is still valid afterwards");
            Check(continuation["data"]["events"].empty(),
                  "no already-delivered event is re-delivered to a client that is up to date");
            replayed.Stop();
        }
    }

    fs::remove_all(projectRoot, cleanupError);
    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
