// SF-02：Job 生命周期事件源测试（wx 无关，直接驱动 JobEventSource + Gateway 轮询）。
// 目标：证明"外部状态变化 → 规范事件"这条链路可用、幂等、且不会伪造终态。
#include "eda-agent-gateway/GatewayServer.h"
#include "eda-agent-gateway/JobEventSource.h"

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>

#include <httplib.h>

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

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

// 可控状态的假 Job 服务：测试显式推进状态，模拟真实 worker。
class SteppableJobService final : public eda::IJobService {
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
        report.state = it->second.state;
        return report;
    }
    void setConcurrency(std::size_t) override {}

    void SetState(const std::string& jobId, eda::JobState state, int exitCode = 0) {
        const auto it = records_.find(jobId);
        if (it == records_.end()) return;
        it->second.state = state;
        it->second.exitCode = exitCode;
    }
    void Remove(const std::string& jobId) { records_.erase(jobId); }

private:
    int sequence_ = 0;
    std::map<std::string, eda::JobRecord> records_;
};

eda::Json GetJson(const std::string& host, int port, const std::string& path,
                  const std::string& token, int& status) {
    httplib::Client client(host, port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers;
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

std::size_t CountEvents(const eda::Json& page, const std::string& type) {
    std::size_t count = 0;
    for (const auto& event : page["data"]["events"]) {
        if (event.value("type", std::string()) == type) ++count;
    }
    return count;
}

} // namespace

int main() {
    // ---- 纯事件源语义（不涉及 HTTP）----
    {
        eda::agent::JobEventSource source;
        SteppableJobService jobs;
        eda::JobRequest request;
        request.jobType = "synth";
        request.projectId = "prj-ev";
        const std::string id = jobs.submit(request);

        Check(source.Poll(&jobs).empty(), "untracked job produces no events");

        source.Track(id, "prj-ev", "eda.synth", "snap-1", "rev-1");
        Check(source.TrackedCount() == 1, "tracked job counted");

        // 首次轮询：Queued（与登记时一致）仍应投递一次，让订阅方拿到初始状态。
        const auto first = source.Poll(&jobs);
        Check(first.size() == 1 && first[0].type == "job/state-changed" &&
                  first[0].state == "Queued",
              "first poll emits the current state");
        Check(first[0].projectId == "prj-ev" && first[0].capability == "eda.synth" &&
                  first[0].snapshotId == "snap-1" && first[0].revision == "rev-1",
              "emitted event carries project/capability/snapshot/revision");

        // 幂等：状态未变不重复投递。
        Check(source.Poll(&jobs).empty(), "unchanged state is not re-emitted (deduplicated)");

        // 状态推进 → 投递新状态。
        jobs.SetState(id, eda::JobState::Running);
        const auto running = source.Poll(&jobs);
        Check(running.size() == 1 && running[0].state == "Running",
              "state change emits a new event");
        Check(source.Poll(&jobs).empty(), "repeated poll after change is quiet");

        // 终态：额外投递一次 job/finished，且只投一次。
        jobs.SetState(id, eda::JobState::Succeeded, /*exitCode=*/0);
        const auto finished = source.Poll(&jobs);
        Check(finished.size() == 2, "terminal state emits state-changed plus finished");
        Check(finished[0].type == "job/state-changed" && finished[1].type == "job/finished",
              "finished event follows the state change");
        Check(finished[1].state == "Succeeded" && finished[1].exitCode == 0,
              "finished event carries the terminal state and exit code");
        Check(source.Poll(&jobs).empty(), "finished is emitted at most once");

        // 服务已不持有该 Job：跳过，不得伪造终态。
        eda::agent::JobEventSource unknown;
        unknown.Track("job-missing", "prj-ev", "eda.synth", "snap-1", "rev-1");
        Check(unknown.Poll(&jobs).empty(),
              "job unknown to the service is skipped (no fabricated terminal state)");

        // Forget / ForgetProject。
        source.Forget(id);
        Check(source.TrackedCount() == 0, "Forget drops a single job");
        source.Track("job-a", "prj-1", "c", "s", "r");
        source.Track("job-b", "prj-2", "c", "s", "r");
        source.ForgetProject("prj-1");
        Check(source.TrackedCount() == 1, "ForgetProject drops only that project");

        // 空服务指针安全。
        eda::agent::JobEventSource safe;
        safe.Track("job-x", "prj", "c", "s", "r");
        Check(safe.Poll(nullptr).empty(), "null service yields no events instead of crashing");
    }

    // ---- Gateway 集成：PollJobEvents 把变化写进真实事件流 ----
    {
        eda::agent::GatewayConfig config;
        config.instanceId = "inst-events";
        config.edition = "edu";
        config.token = "event-token";
        config.uiToken = "event-ui-token";
        config.port = 0;

        const auto provider = []() {
            std::vector<eda::agent::ReadyPlugin> ready;
            ready.push_back({"eda-synth-yosys", "1.0.0", {"synth"}});
            return ready;
        };

        eda::agent::GatewayServer server(config, provider);
        SteppableJobService jobs;
        server.SetJobServiceProvider([&jobs]() -> eda::IJobService* { return &jobs; });

        std::string error;
        Check(server.Start(error), "event gateway starts");
        server.RunAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int status = 0;
        const auto state0 = GetJson("127.0.0.1", server.Port(),
                                    "/api/v1/projects/prj-ev/state", "event-token", status);
        Check(status == 200, "state readable before events");
        const std::string watermark = state0["data"].value("high_watermark", std::string("0"));

        Check(server.PollJobEvents() == 0, "no tracked jobs -> no events");
        const auto state1 = GetJson("127.0.0.1", server.Port(),
                                    "/api/v1/projects/prj-ev/state", "event-token", status);
        Check(state1["data"].value("high_watermark", std::string()) == watermark,
              "polling without changes does not advance the event watermark");

        // 端到端：经真实提交路径登记 Job，再用 PollJobEvents 观察状态变化进入事件流。
        // 需要一个真实工程上下文 + 快照 + grant，才能走通 POST /jobs。
        namespace fs = std::filesystem;
        std::error_code cleanupError;
        const fs::path projectRoot = fs::temp_directory_path() /
                                     ("sigflow_events_" + std::to_string(
                                         std::chrono::high_resolution_clock::now()
                                             .time_since_epoch().count()));
        fs::remove_all(projectRoot, cleanupError);
        fs::create_directories(projectRoot / "rtl");
        std::ofstream(projectRoot / "rtl" / "top.v", std::ios::binary)
            << "module top; endmodule\n";
        std::ofstream(projectRoot / "sigflow.project", std::ios::binary)
            << R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/top.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON";

        std::string projectId;
        Check(server.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true, projectId,
                                                         error),
              "integration project context built");
        const std::string route = "/api/v1/projects/" + projectId;
        const eda::Json context = GetJson("127.0.0.1", server.Port(), route + "/context",
                                          "event-token", status);
        const std::string revision = context["data"].value("revision", std::string());

        httplib::Client client("127.0.0.1", server.Port());
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(5, 0);
        httplib::Headers auth{{"Authorization", "Bearer event-token"}};
        httplib::Headers uiAuth{{"Authorization", "Bearer event-ui-token"}};

        const auto snapshotRes = client.Post(
            (route + "/snapshots").c_str(), auth,
            eda::Json{{"expected_revision", revision}, {"require_saved", true}}.dump(),
            "application/json");
        Check(snapshotRes && snapshotRes->status == 201, "integration snapshot created");
        const std::string snapshotId =
            snapshotRes ? eda::Json::parse(snapshotRes->body)["data"].value("snapshot_id",
                                                                           std::string())
                        : std::string();

        const auto grantRes = client.Post(
            (route + "/grants").c_str(), uiAuth,
            eda::Json{{"plan_hash", "ph-events"}, {"revision", revision},
                      {"snapshot_id", snapshotId}, {"max_jobs", 2}}.dump(),
            "application/json");
        Check(grantRes && grantRes->status == 201, "integration grant issued");
        const std::string grantId =
            grantRes ? eda::Json::parse(grantRes->body)["data"].value("grant_id", std::string())
                     : std::string();

        httplib::Headers jobHeaders{{"Authorization", "Bearer event-token"},
                                    {"Idempotency-Key", "key-events-1"}};
        const auto jobRes = client.Post(
            (route + "/jobs").c_str(), jobHeaders,
            eda::Json{{"snapshot_id", snapshotId},
                      {"expected_revision", revision},
                      {"capability", "eda.synth"},
                      {"grant_id", grantId},
                      {"params", eda::Json{{"top_module", "top"}}}}
                .dump(),
            "application/json");
        Check(jobRes && jobRes->status == 202, "integration job submitted via POST /jobs");
        const std::string jobId =
            jobRes ? eda::Json::parse(jobRes->body)["data"].value("job_id", std::string())
                   : std::string();

        if (!jobId.empty()) {
            // 提交路径已发 job/created；此处推进状态并轮询。
            jobs.SetState(jobId, eda::JobState::Running);
            const std::size_t published = server.PollJobEvents();
            Check(published >= 1, "PollJobEvents publishes observed state changes");

            const eda::Json started = GetJson("127.0.0.1", server.Port(),
                                              route + "/events?after=" + watermark, "event-token",
                                              status);
            Check(status == 200, "event stream readable after polling");
            Check(CountEvents(started, "job/created") >= 1,
                  "submission emitted job/created into the stream");
            Check(CountEvents(started, "job/state-changed") >= 1,
                  "state change emitted job/state-changed into the stream");

            std::string observedState;
            for (const auto& event : started["data"]["events"]) {
                if (event.value("type", std::string()) == "job/state-changed") {
                    observedState = event["data"].value("state", std::string());
                }
            }
            Check(observedState == "Running", "event stream carries the observed Running state");

            // 再轮询一次：无变化时不得重复发事件（幂等）。
            Check(server.PollJobEvents() == 0, "second poll with no change publishes nothing");

            // 终态：state-changed + finished 各一次。
            jobs.SetState(jobId, eda::JobState::Succeeded, 0);
            const std::size_t finishedPublished = server.PollJobEvents();
            Check(finishedPublished == 2, "terminal transition publishes state-changed + finished");
            Check(server.PollJobEvents() == 0, "finished is not published twice");

            const eda::Json done = GetJson("127.0.0.1", server.Port(),
                                           route + "/events?after=" + watermark, "event-token",
                                           status);
            Check(CountEvents(done, "job/finished") == 1, "exactly one job/finished in the stream");
        }

        server.Stop();
        Check(!server.Running(), "event gateway stops cleanly");
        fs::remove_all(projectRoot, cleanupError);
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
