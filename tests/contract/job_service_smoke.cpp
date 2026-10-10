#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/schemas.hpp>

#include "eda-core/JobService.h"
#include "eda-core/SchemaRegistry.h"
#include "eda-core/CoreSchemas.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

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

template <typename F>
bool WaitUntil(F predicate, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

std::unique_ptr<eda::IProcessHost> MakeHost() {
    return eda::CreatePlatformProcessHost();
}

eda::JobState StateOf(eda::CoreJobService& service, const std::string& id) {
    const auto record = service.get(id);
    return record ? record->state : eda::JobState::Failed;
}

class SuccessProvider final : public eda::IJobProvider {
public:
    std::string jobType() const override { return "synth"; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest& request, eda::JobContext& ctx) override {
        ctx.log("working", false);
        ctx.progress(50, "half");
        eda::Artifact artifact;
        artifact.id = "netlist";
        artifact.path = request.projectId + "/top.json";
        artifact.schema = "eda.netlist.yosys-json.v1";
        artifact.role = "primary";
        ctx.registerArtifact(artifact);
        ctx.emitMetric(eda::Json{{"lut", 96}});
        return eda::Error::Ok();
    }
};

class ThrowingProvider final : public eda::IJobProvider {
public:
    std::string jobType() const override { return "boom"; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest&, eda::JobContext&) override {
        throw std::runtime_error("provider exploded");
    }
};

class SlowProcessProvider final : public eda::IJobProvider {
public:
    explicit SlowProcessProvider(std::string type) : type_(std::move(type)) {}
    std::string jobType() const override { return type_; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest&, eda::JobContext& ctx) override {
        eda::ProcessSpec spec;
#ifdef _WIN32
        spec.executable = "cmd.exe";
        spec.arguments = {"/C", "ping", "-n", "30", "127.0.0.1"};
#else
        spec.executable = "/bin/sh";
        spec.arguments = {"-c", "sleep 30"};
#endif
        const eda::ProcessResult result = ctx.processHost().Run(spec, nullptr);
        ctx.log(std::string("outcome=") + std::to_string(static_cast<int>(result.outcome)), false);
        return eda::Error::Ok();
    }

private:
    std::string type_;
};

} // namespace

int main() {
    {
        eda::CoreJobService service(&MakeHost);
        service.RegisterProvider(std::make_shared<SuccessProvider>());
        eda::JobRequest request;
        request.jobType = "synth";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return service.report(id).artifacts.size() == 1; }),
              "job succeeded and recorded artifact");
        const eda::JobReport report = service.report(id);
        Check(report.state == eda::JobState::Succeeded, "report state Succeeded");
        Check(report.artifacts[0].id == "netlist", "report artifact id");
        Check(report.metrics.value("lut", 0) == 96, "report metric recorded");
        const std::filesystem::path logPath =
            service.JobRoot() / "synth" / id / "logs" / "process.log";
        std::ifstream log(logPath);
        std::string logLine;
        Check(static_cast<bool>(std::getline(log, logLine)) && logLine == "working",
              "job log is persisted for history UI");
    }
    {
        eda::CoreJobService service(&MakeHost);
        service.RegisterProvider(std::make_shared<ThrowingProvider>());
        eda::JobRequest request;
        request.jobType = "boom";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Failed; }),
              "provider exception becomes Failed");
    }
    {
        eda::CoreJobService service(&MakeHost);
        eda::JobRequest request;
        request.jobType = "missing";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Failed; }),
              "missing provider becomes Failed");
    }
    {
        eda::CoreJobService service(&MakeHost);
        service.RegisterProvider(std::make_shared<SlowProcessProvider>("proc"));
        eda::JobRequest request;
        request.jobType = "proc";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Running; }),
              "process job reaches Running");
        Check(service.cancel(id, "user"), "cancel accepted");
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::Cancelled; }),
              "process job cancelled");
    }
    {
        eda::CoreJobService service(&MakeHost);
        service.SetDefaultTimeoutSeconds(1);
        service.RegisterProvider(std::make_shared<SlowProcessProvider>("slow"));
        eda::JobRequest request;
        request.jobType = "slow";
        request.projectId = "prj";
        const std::string id = service.submit(request);
        Check(WaitUntil([&]() { return StateOf(service, id) == eda::JobState::TimedOut; }),
              "process job timed out");
    }
    {
        eda::SimpleSchemaRegistry schemas;
        eda::RegisterCoreSchemas(schemas);
        std::string error;
        Check(schemas.HasSchema("eda.job.v1"), "core schema eda.job.v1 registered");
        Check(schemas.Validate("eda.job.v1",
                               eda::Json{{"job_id", "j"}, {"job_type", "synth"},
                                         {"state", "Succeeded"}},
                               error),
              "valid job record passes schema");
        Check(!schemas.Validate("eda.job.v1", eda::Json{{"job_id", "j"}}, error),
              "invalid job record rejected by schema");
    }

    // SF-03：提交前持久化与重启恢复。
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() /
                              ("sigflow-jobs-restart-" +
                               std::to_string(static_cast<long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::remove_all(root);
        std::string firstId;
        {
            eda::CoreJobService service(&MakeHost, root);
            service.RegisterProvider(std::make_shared<SuccessProvider>());
            eda::JobRequest request;
            request.jobType = "synth";
            request.projectId = "prj-restart";
            firstId = service.submit(request);
            Check(WaitUntil([&]() { return service.report(firstId).artifacts.size() == 1; }),
                  "restart: job completes before shutdown");
        }
        {
            // 新进程实例，同一 jobsRoot：应恢复记录与报告。
            eda::CoreJobService service(&MakeHost, root);
            service.RegisterProvider(std::make_shared<SuccessProvider>());
            const auto recovered = service.get(firstId);
            Check(recovered.has_value() && recovered->state == eda::JobState::Succeeded,
                  "restart: completed job recovered from disk");
            Check(service.report(firstId).artifacts.size() == 1,
                  "restart: report recovered from disk");
            bool listed = false;
            for (const auto& record : service.list("prj-restart")) {
                if (record.id == firstId) listed = true;
            }
            Check(listed, "restart: job appears in project listing");
            eda::JobRequest request;
            request.jobType = "synth";
            request.projectId = "prj-restart";
            const std::string secondId = service.submit(request);
            Check(secondId != firstId, "restart: id sequence does not collide");
            Check(WaitUntil([&]() { return service.report(secondId).artifacts.size() == 1; }),
                  "restart: new job still runs after recovery");
        }

        // 非终态恢复：模拟上次进程在 Running 时崩溃留下的 manifest。
        {
            eda::JobRequest request;
            request.jobType = "synth";
            request.projectId = "prj-restart";
            const fs::path jobDir = root / "synth" / "job-20000101000000-777";
            fs::create_directories(jobDir);
            std::ofstream(jobDir / "manifest.json", std::ios::binary)
                << R"({"schema_version":"eda.job.v1","job_id":"job-20000101000000-777",)"
                << R"("job_type":"synth","plugin_id":"p","project_id":"prj-restart",)"
                << R"("state":"Running","created_at":"t","updated_at":"t","exit_code":0,)"
                << R"("parameters":{},"transitions":[]})";
            eda::CoreJobService service(&MakeHost, root);
            const auto recovered = service.get("job-20000101000000-777");
            Check(recovered.has_value() && recovered->state == eda::JobState::Failed,
                  "restart: orphaned Running job becomes Failed, not fake-running");
        }
        fs::remove_all(root);
    }

    // SF-03：受控 Job 数据根目录解析（不再依赖系统临时目录作为可恢复来源）。
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() /
                              ("sigflow-jobs-root-" +
                               std::to_string(static_cast<long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::remove_all(base);
        const fs::path projectAgentRoot = base / "project" / ".sigflow" / "agent";
        const fs::path appDataRoot = base / "appdata";
        fs::create_directories(projectAgentRoot);
        std::string warning;

        // 1) 工程受控目录可用：优先使用它，且按 projectKey 隔离。
        {
            const fs::path resolved = eda::CoreJobService::ResolveJobRoot(
                projectAgentRoot, appDataRoot, "abc123", warning);
            Check(warning.empty(), "controlled root: project directory preferred without warning");
            Check(resolved == projectAgentRoot / "jobs" / "abc123",
                  "controlled root: project-scoped path layout");
            Check(fs::is_directory(resolved), "controlled root: directory created");
            // 不同工程 -> 不同目录（隔离）。
            const fs::path other = eda::CoreJobService::ResolveJobRoot(
                projectAgentRoot, appDataRoot, "def456", warning);
            Check(other != resolved, "controlled root: distinct projects are isolated");
        }

        // 2) 工程目录不可用（用文件占位使其无法成为目录）-> 回退应用数据目录并给出提示。
        {
            const fs::path blocked = base / "blocked" / ".sigflow" / "agent";
            fs::create_directories(blocked.parent_path());
            std::ofstream(blocked, std::ios::binary) << "not a directory";
            const fs::path resolved =
                eda::CoreJobService::ResolveJobRoot(blocked, appDataRoot, "abc123", warning);
            Check(resolved == appDataRoot / "sigflow-jobs",
                  "controlled root: falls back to application data root");
            Check(!warning.empty(), "controlled root: fallback reports a readable warning");
        }

        // 3) 两者都不可用 -> 系统临时目录兜底，并明确告警（不得静默当作可恢复来源）。
        {
            const fs::path blockedProject = base / "blocked2" / ".sigflow" / "agent";
            fs::create_directories(blockedProject.parent_path());
            std::ofstream(blockedProject, std::ios::binary) << "not a directory";
            const fs::path blockedApp = base / "blocked-app";
            std::ofstream(blockedApp, std::ios::binary) << "not a directory";
            const fs::path resolved =
                eda::CoreJobService::ResolveJobRoot(blockedProject, blockedApp, "abc123", warning);
            Check(resolved == fs::temp_directory_path() / "sigflow-jobs",
                  "controlled root: last resort is the system temp directory");
            Check(!warning.empty(), "controlled root: temp fallback is never silent");
        }

        // 4) 服务按解析结果落盘，并把根目录暴露给宿主。
        {
            const fs::path root = eda::CoreJobService::ResolveJobRoot(
                projectAgentRoot, appDataRoot, "svc-key", warning);
            eda::CoreJobService service(&MakeHost, root);
            Check(service.IsControlledRoot(), "controlled root: service reports controlled root");
            Check(service.JobRoot() == root, "controlled root: service reports its actual root");
            service.RegisterProvider(std::make_shared<SuccessProvider>());
            eda::JobRequest request;
            request.jobType = "synth";
            request.projectId = "prj-root";
            const std::string id = service.submit(request);
            Check(WaitUntil([&]() { return service.report(id).artifacts.size() == 1; }),
                  "controlled root: job runs under the resolved root");
            Check(fs::exists(root / "synth" / id / "manifest.json"),
                  "controlled root: manifest lands under the resolved root");
        }
        // 5) 未显式指定根目录时明确标记为非受控（避免误判为可恢复来源）。
        {
            eda::CoreJobService service(&MakeHost);
            Check(!service.IsControlledRoot(),
                  "controlled root: unconfigured service is flagged non-controlled");
        }
        fs::remove_all(base);
    }

    // SF-03：重启恢复加固——Job ID 唯一性、损坏 manifest、未知状态、提交即持久化。
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() /
                              ("sigflow-jobs-hardening-" +
                               std::to_string(static_cast<long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::remove_all(root);

        // 提交后立刻（未等执行完成）应已有 manifest，保证崩溃于执行阶段仍可恢复。
        // 注意：worker 是并发跑的，若只"提交后查一次"，即使实现把持久化放在入队之后，
        // worker 也可能恰好抢先写完而让断言假通过（已用变异验证过这个假阳性）。
        // 因此这里改为**直接断言 mp 内容**：manifest 必须已存在且已落盘为 Created 状态。
        std::string submittedId;
        {
            eda::CoreJobService service(&MakeHost, root);
            service.RegisterProvider(std::make_shared<SlowProcessProvider>("slow2"));
            eda::JobRequest request;
            request.jobType = "slow2";
            request.projectId = "prj-hard";
            submittedId = service.submit(request);
            Check(!submittedId.empty(), "hardening: submit returns an id");
            const fs::path manifest = root / "slow2" / submittedId / "manifest.json";
            Check(fs::exists(manifest),
                  "hardening: manifest exists by the time submit returns");

            // 关键：manifest 的内容必须与"刚提交"一致（Created），而不是被 worker 推进后的状态。
            // 若持久化发生在入队之后，worker 有可能已把 manifest 覆盖成 Queued/Running/终态，
            // 或存在"文件已存在但内容尚未写入完整"的中间态——两者都说明提交并没有原子落盘。
            std::ifstream manifestIn(manifest, std::ios::binary);
            eda::Json manifestJson;
            bool parsed = false;
            if (manifestIn) {
                try {
                    manifestIn >> manifestJson;
                    parsed = true;
                } catch (const std::exception&) {
                    parsed = false;
                }
            }
            Check(parsed, "hardening: manifest is complete valid JSON right after submit");
            Check(parsed && manifestJson.value("job_id", std::string()) == submittedId,
                  "hardening: manifest identifies the submitted job");
            Check(parsed && manifestJson.value("state", std::string()) == "Created",
                  "hardening: submit persists the pre-dispatch Created record");

            service.cancel(submittedId, "test");
            WaitUntil([&]() { return StateOf(service, submittedId) == eda::JobState::Cancelled; });
        }

        // 提交前持久化失败必须**不**把 Job 交给 worker：用不可写根目录验证 submit 返回空。
        {
            const fs::path blockedRoot = fs::temp_directory_path() /
                                         ("sigflow-jobs-blocked-" +
                                          std::to_string(static_cast<long long>(
                                              std::chrono::steady_clock::now()
                                                  .time_since_epoch()
                                                  .count())));
            fs::remove_all(blockedRoot);
            // 用文件占位使 <root>/synth 无法成为目录，PersistRecord 必然失败。
            std::ofstream(blockedRoot, std::ios::binary) << "not a directory";
            eda::CoreJobService service(&MakeHost, blockedRoot);
            service.RegisterProvider(std::make_shared<SuccessProvider>());
            eda::JobRequest request;
            request.jobType = "synth";
            request.projectId = "prj-blocked";
            const std::string blockedId = service.submit(request);
            Check(blockedId.empty(),
                  "hardening: submit returns empty when persistence fails (no phantom job)");
            Check(service.list("prj-blocked").empty(),
                  "hardening: failed submit leaves no in-memory record");
            fs::remove_all(blockedRoot);
        }

        // 损坏 manifest 必须被跳过，而不是让整个恢复流程失败。
        {
            const fs::path brokenDir = root / "synth" / "job-20000101000000-100";
            fs::create_directories(brokenDir);
            std::ofstream(brokenDir / "manifest.json", std::ios::binary) << "{ not json";
            eda::JobRequest good;
            good.jobType = "synth";
            good.projectId = "prj-hard";
            eda::CoreJobService service(&MakeHost, root);
            service.RegisterProvider(std::make_shared<SuccessProvider>());
            const auto broken = service.get("job-20000101000000-100");
            Check(!broken.has_value(), "hardening: corrupt manifest is skipped");
            const std::string fresh = service.submit(good);
            Check(WaitUntil([&]() { return service.report(fresh).artifacts.size() == 1; }),
                  "hardening: service still works after a corrupt manifest");
        }

        // 未知状态字符串不得被当成 Running/Succeeded 静默接受。
        {
            const fs::path oddDir = root / "synth" / "job-20000101000000-200";
            fs::create_directories(oddDir);
            std::ofstream(oddDir / "manifest.json", std::ios::binary)
                << R"({"schema_version":"eda.job.v1","job_id":"job-20000101000000-200",)"
                << R"("job_type":"synth","plugin_id":"p","project_id":"prj-hard",)"
                << R"("state":"Teleporting","created_at":"t","updated_at":"t","exit_code":0,)"
                << R"("parameters":{},"transitions":[]})";
            eda::CoreJobService service(&MakeHost, root);
            const auto odd = service.get("job-20000101000000-200");
            if (odd.has_value()) {
                Check(odd->state != eda::JobState::Succeeded,
                      "hardening: unknown persisted state is not reported as Succeeded");
            } else {
                Check(true, "hardening: unknown-state manifest rejected outright");
            }
        }

        // 恢复后新 Job 的 ID 不得与磁盘上已存在的 ID 冲突（含人工高序号）。
        {
            const fs::path highDir = root / "synth" / "job-20000101000000-999999";
            fs::create_directories(highDir);
            std::ofstream(highDir / "manifest.json", std::ios::binary)
                << R"({"schema_version":"eda.job.v1","job_id":"job-20000101000000-999999",)"
                << R"("job_type":"synth","plugin_id":"p","project_id":"prj-hard",)"
                << R"("state":"Succeeded","created_at":"t","updated_at":"t","exit_code":0,)"
                << R"("parameters":{},"transitions":[]})";
            eda::CoreJobService service(&MakeHost, root);
            service.RegisterProvider(std::make_shared<SuccessProvider>());
            eda::JobRequest request;
            request.jobType = "synth";
            request.projectId = "prj-hard";
            const std::string fresh = service.submit(request);
            Check(fresh != "job-20000101000000-999999",
                  "hardening: new job id avoids the recovered high sequence");
        }
        fs::remove_all(root);
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
