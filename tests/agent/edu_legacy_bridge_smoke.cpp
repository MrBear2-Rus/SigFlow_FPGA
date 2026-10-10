// NG-09：旧 `main/jobs` 历史只读桥冒烟测试。
//
// 覆盖：
//   * 旧记录一律 origin=legacy / completeness=legacy_unverified，不冒充当前 revision 证据；
//   * 旧报告 errors[] → 规范 Diagnostic（保留 raw_code），未知状态串不被当成 Succeeded；
//   * 产物只给 opaque artifact_id + sha256，绝不返回本机绝对路径；
//   * 缺报告/报告损坏是显式状态，不是“电路无问题”；
//   * 只读：桥不修改旧目录（前后文件 hash 不变）。
#include "eda-agent-gateway/GatewayServer.h"

#include <eda/api/Types.h>

#include "eda-platform/Sha256.h"

#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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

// 允许为 null 的字符串字段：null/缺失都读成空串（value() 遇到 null 会抛异常）。
std::string OptionalString(const eda::Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

void WriteText(const std::filesystem::path& path, const std::string& text) {    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// 记录旧历史子树的文件 hash，用于证明桥是只读的。
std::map<std::string, std::string> SnapshotHashes(const std::filesystem::path& root) {
    std::map<std::string, std::string> hashes;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return hashes;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || error) continue;
        hashes[entry.path().filename().string() + "|" +
               entry.path().parent_path().filename().string()] =
            eda::platform::Sha256FileHex(entry.path());
    }
    return hashes;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path projectRoot =
        fs::temp_directory_path() /
        ("sigflow_legacy_bridge_" + std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::error_code cleanupError;
    fs::remove_all(projectRoot, cleanupError);
    fs::create_directories(projectRoot / "rtl");
    WriteText(projectRoot / "rtl" / "top.v", "module top; endmodule\n");
    WriteText(projectRoot / "sigflow.project", R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/top.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON");

    const fs::path jobs = projectRoot / ".sigflow" / "jobs";
    const std::string absoluteArtifact = "C:/definitely/not/here/bitstream.fs";

    // simulation：带真实错误与绝对路径产物。
    WriteText(jobs / "simulation" / "20260913T164120-000001" / "manifest.json", R"JSON({
  "created_at": "2026-09-13T08:41:20Z",
  "exit_code": 0,
  "job_id": "20260913T164120-000001",
  "job_type": "simulation",
  "parameters": {"executable": "C:/definitely/not/here/sh.exe", "arguments": ["-x"]},
  "schema_version": "1.0",
  "state": "Failed",
  "updated_at": "2026-09-13T08:41:21Z"
})JSON");
    WriteText(jobs / "simulation" / "20260913T164120-000001" / "reports" / "job-report.json",
              R"JSON({
  "artifacts": [
    {"kind": "bitstream", "path": "C:/definitely/not/here/bitstream.fs",
     "sha256": "42359f845a31fd7997cbe1d37f4fff978de1ad68b87caefc5661a72561c24275",
     "size_bytes": 1024}
  ],
  "completed_at": "2026-09-13T08:41:21Z",
  "errors": [
    {"code": "JOB_NOT_STARTABLE", "ir_coordinate": "", "log_line": 7, "severity": "error",
     "stage": "Validating", "summary": "Concurrency limit reached for simulation."}
  ],
  "exit_code": 0,
  "job_id": "20260913T164120-000001",
  "job_type": "simulation",
  "schema_version": "1.0",
  "started_at": "2026-09-13T08:41:20Z",
  "state": "Failed",
  "summary": "Failed"
})JSON");

    // synthesis：干净报告（errors 为空），但仍是 legacy_unverified。
    WriteText(jobs / "synthesis" / "20260914T101010-000001" / "manifest.json", R"JSON({
  "created_at": "2026-09-14T10:10:10Z",
  "job_id": "20260914T101010-000001",
  "job_type": "synthesis",
  "schema_version": "1.0",
  "state": "Succeeded"
})JSON");
    WriteText(jobs / "synthesis" / "20260914T101010-000001" / "reports" / "job-report.json",
              R"JSON({
  "artifacts": [],
  "errors": [],
  "exit_code": 0,
  "job_id": "20260914T101010-000001",
  "job_type": "synthesis",
  "schema_version": "1.0",
  "state": "Succeeded",
  "summary": "ok"
})JSON");

    // pack：只有 manifest，报告缺失。
    WriteText(jobs / "pack" / "20260915T120000-000001" / "manifest.json", R"JSON({
  "created_at": "2026-09-15T12:00:00Z",
  "job_id": "20260915T120000-000001",
  "job_type": "pack",
  "schema_version": "1.0",
  "state": "Succeeded"
})JSON");

    // flash：报告损坏（不是合法 JSON）。
    WriteText(jobs / "flash" / "20260916T120000-000001" / "manifest.json", R"JSON({
  "created_at": "2026-09-16T12:00:00Z",
  "job_id": "20260916T120000-000001",
  "job_type": "flash",
  "schema_version": "1.0",
  "state": "Succeeded"
})JSON");
    WriteText(jobs / "flash" / "20260916T120000-000001" / "reports" / "job-report.json",
              "{ this is not json");

    // debug：未知状态串不得被当成 Succeeded。
    WriteText(jobs / "debug" / "20260917T120000-000001" / "manifest.json", R"JSON({
  "created_at": "2026-09-17T12:00:00Z",
  "job_id": "20260917T120000-000001",
  "job_type": "debug",
  "schema_version": "1.0",
  "state": "SomethingElse"
})JSON");
    WriteText(jobs / "debug" / "20260917T120000-000001" / "reports" / "job-report.json",
              R"JSON({
  "artifacts": [],
  "errors": [],
  "exit_code": 0,
  "job_id": "20260917T120000-000001",
  "job_type": "debug",
  "schema_version": "1.0",
  "state": "SomethingElse",
  "summary": "unknown"
})JSON");

    const auto before = SnapshotHashes(jobs);

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-legacy";
    config.edition = "edu";
    config.token = "agent-token";
    config.port = 0;
    const auto provider = []() { return std::vector<eda::agent::ReadyPlugin>{}; };
    eda::agent::GatewayServer server(config, provider);
    std::string error;
    std::string projectId;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true, projectId, error),
          "gateway builds project context for the legacy-history project");
    if (!server.Start(error)) {
        std::cout << "start error: " << error << "\nFAILURES\n";
        return 1;
    }
    server.RunAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int port = server.Port();
    const std::string base = "/api/v1/projects/" + projectId;

    int status = 0;
    {
        const eda::Json body = GetJson(port, base + "/legacy/jobs", "agent-token", status);
        Check(status == 200, "GET /legacy/jobs returns 200");
        Check(body["data"].value("origin", std::string()) == "legacy" &&
                  body["data"].value("completeness", std::string()) == "legacy_unverified",
              "legacy list is explicitly marked legacy_unverified");
        Check(body["data"]["jobs"].size() == 5, "all five legacy jobs are listed");
        Check(body["data"].value("total", 0) == 5, "legacy list reports the full total");
        bool everyLegacy = true;
        bool orderingOk = true;
        std::string previousCreated;
        for (const auto& job : body["data"]["jobs"]) {
            if (job.value("origin", std::string()) != "legacy" ||
                job.value("completeness", std::string()) != "legacy_unverified") {
                everyLegacy = false;
            }
            const std::string created = OptionalString(job, "created_at");
            if (!created.empty() && !previousCreated.empty() && created > previousCreated) {
                orderingOk = false;
            }
            if (!created.empty()) previousCreated = created;
        }
        Check(everyLegacy, "every legacy entry carries origin/completeness markers");
        Check(orderingOk, "legacy list is ordered newest first");
        Check(body.dump().find("C:/definitely/not/here") == std::string::npos,
              "legacy list never leaks absolute local paths from old manifests");
        Check(body.dump().find(projectRoot.string()) == std::string::npos,
              "legacy list never leaks the project root");
        bool missingReport = false;
        bool unreadableReport = false;
        for (const auto& job : body["data"]["jobs"]) {
            const std::string problem = OptionalString(job, "problem");
            if (job.value("job_id", std::string()) == "20260915T120000-000001" &&
                problem == "report_missing") {
                missingReport = true;
            }
            if (job.value("job_id", std::string()) == "20260916T120000-000001" &&
                problem == "report_unreadable") {
                unreadableReport = true;
            }
        }
        Check(missingReport, "a legacy job without a report is flagged report_missing");
        Check(unreadableReport, "a corrupt legacy report is flagged report_unreadable");
    }
    {
        const eda::Json body = GetJson(port, base + "/legacy/jobs", "agent-token", status);
        (void)body;
        const eda::Json page =
            GetJson(port, base + "/legacy/jobs?limit=2", "agent-token", status);
        Check(page["data"]["jobs"].size() == 2 && page["data"].value("has_more", false),
              "legacy list honours limit and reports has_more");
        Check(page["data"].value("next_cursor", std::string()) == "2",
              "legacy list returns a usable next_cursor");
        const eda::Json next =
            GetJson(port, base + "/legacy/jobs?limit=2&cursor=2", "agent-token", status);
        Check(next["data"].value("offset", 0) == 2, "legacy cursor advances the offset");
        const eda::Json clamped =
            GetJson(port, base + "/legacy/jobs?limit=99999", "agent-token", status);
        Check(clamped["data"].value("limit", 0) == 500, "legacy limit is clamped to the maximum");
    }
    {
        const eda::Json body = GetJson(
            port, base + "/legacy/jobs/20260913T164120-000001/report", "agent-token", status);
        if (status != 200) {
            std::cout << "  [debug] legacy report status=" << status << " body=" << body.dump(2)
                      << "\n";
        }
        Check(status == 200, "legacy report route returns 200");
        const eda::Json data = body["data"];
        Check(data.value("origin", std::string()) == "legacy",
              "legacy report keeps origin=legacy");
        Check(data.value("completeness", std::string()) == "legacy_unverified",
              "legacy report is completeness=legacy_unverified");
        Check(data["revision"].is_null(),
              "legacy report does not claim a revision");
        Check(data["input_fingerprint"].is_null(),
              "legacy report does not claim an input fingerprint");
        Check(data.value("raw_report_schema", std::string()) == "main/jobs 1.0",
              "legacy report records the original report schema");
        Check(data.value("reason", std::string()) ==
                  "legacy job has no input fingerprint or revision",
              "legacy report states why it cannot be verified");
        Check(data.value("capability", std::string()) == "simulation",
              "legacy report maps the old job type into the capability field");
        Check(!data["diagnostics"].empty() &&
                  data["diagnostics"][0].value("code", std::string()) == "JOB_NOT_STARTABLE" &&
                  data["diagnostics"][0].value("raw_code", std::string()) == "JOB_NOT_STARTABLE",
              "legacy errors[] become normalised diagnostics with the raw code preserved");
        Check(data["diagnostics"][0]["location"].value("log_line", 0) == 7,
              "legacy diagnostics keep the original log line");
        Check(data["artifacts"].size() == 1, "legacy artifacts are normalised");
        Check(data["artifacts"][0].value("artifact_id", std::string()).rfind("legacy-", 0) == 0,
              "legacy artifacts get an opaque artifact_id");
        Check(!data["artifacts"][0].contains("path"),
              "legacy artifacts never expose the recorded local path");
        Check(body.dump().find("C:/definitely/not/here") == std::string::npos &&
                  body.dump().find(projectRoot.string()) == std::string::npos,
              "legacy report never leaks absolute local paths");
        Check(data.value("state", std::string()) == "Failed",
              "legacy report state is mapped into the public vocabulary");
    }
    {
        const eda::Json clean = GetJson(
            port, base + "/legacy/jobs/20260914T101010-000001/report", "agent-token", status);
        Check(status == 200, "clean legacy report returns 200");
        Check(clean["data"]["diagnostics"].empty(), "clean legacy report has no diagnostics");
        Check(clean["data"].value("completeness", std::string()) == "legacy_unverified",
              "a clean legacy report still cannot claim to be verified");
    }
    {
        const eda::Json unknown = GetJson(
            port, base + "/legacy/jobs/20260917T120000-000001/report", "agent-token", status);
        Check(unknown["data"].value("state", std::string()) == "Failed",
              "an unknown legacy state string is never reported as Succeeded");
    }
    {
        GetJson(port, base + "/legacy/jobs/20260915T120000-000001/report", "agent-token", status);
        Check(status == 404, "a legacy job without a report answers 404, not a fake clean report");
        GetJson(port, base + "/legacy/jobs/20260916T120000-000001/report", "agent-token", status);
        Check(status == 404, "a corrupt legacy report answers 404 with a reason");
        GetJson(port, base + "/legacy/jobs/no-such-job/report", "agent-token", status);
        Check(status == 404, "an unknown legacy job id answers 404");
        GetJson(port, base + "/legacy/jobs", "", status);
        Check(status == 401, "legacy history requires the bearer token");
        GetJson(port, "/api/v1/projects/project-000000000000000000000000/legacy/jobs",
                "agent-token", status);
        Check(status == 404, "legacy history for an unknown project answers 404");
    }

    server.Stop();
    const auto after = SnapshotHashes(jobs);
    Check(before == after, "the read-only bridge left the legacy history byte-identical");

    fs::remove_all(projectRoot, cleanupError);
    std::cout << (g_failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return g_failures == 0 ? 0 : 1;
}
