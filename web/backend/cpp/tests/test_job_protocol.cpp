#include "eda-core/JobProtocol.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace eda;
using namespace eda::web;
namespace fs = std::filesystem;
Json load(const fs::path& p) {
    std::ifstream f(p);
    if (!f)
        throw std::runtime_error(p.string());
    Json j;
    f >> j;
    return j;
}
void check(bool b, const char* msg) {
    if (!b)
        throw std::runtime_error(msg);
}
int main(int argc, char** argv) {
    try {
        check(argc == 3, "fixture and output directories required");
        fs::path fixtures = argv[1], output = argv[2];
        fs::create_directories(output);
        auto verify = [&](const char* name, const Json& value) {
            check(value == load(fixtures / name), name);
            std::ofstream f(output / name);
            f << value.dump(2);
            check(bool(f), "write generated fixture");
        };
        JobReport report;
        report.jobId = "job-001";
        report.jobType = "synth";
        report.pluginId = "eda-synth-yosys";
        report.state = JobState::Succeeded;
        report.artifacts.push_back(Artifact{"netlist", "artifacts/top.json",
                                            "eda.netlist.yosys-json.v1", std::string(64, 'a'),
                                            "primary"});
        report.metrics = Json::array(
            {Json{{"exit_code", 0}}, Json{{"ports", 2}, {"cells", 1}, {"netnames", 3}}});
        verify("report-success.json", ReportToJson(report));
        auto event = ResultEvent(report);
        check(event == Json{{"type", "result"},
                            {"state", "Succeeded"},
                            {"exitCode", 0},
                            {"jobId", "job-001"}},
              "result event naming");
        report.state = JobState::Failed;
        report.exitCode = kNoToolExitCode;
        report.artifacts.clear();
        report.metrics = nullptr;
        report.diagnostics =
            Json::array({Json{{"code", "TOOL_NOT_FOUND"}, {"message", "yosys not found"}}});
        verify("report-failure.json", ReportToJson(report));
        ExecutionRequest input;
        input.jobId = "job-001";
        input.request.jobType = "synth";
        input.request.pluginId = "eda-synth-yosys";
        input.request.projectId = "project-001";
        input.request.params = Json{{"top_module", "top"},
                                    {"source_files", Json::array({"/snapshots/source/top.sv"})}};
        input.projectPath = "/projects/project-001";
        input.workDir = "/projects/project-001/.sigflow/agent/jobs/synth/job-001";
        verify("request.json", RequestToJson(input));
        JobRecord record;
        record.id = input.jobId;
        record.request = input.request;
        record.state = JobState::Succeeded;
        record.createdAt = "2026-10-31T12:00:00Z";
        record.updatedAt = "2026-10-31T12:00:01Z";
        record.transitions = {{JobState::Running, record.createdAt, "execute", "", 0},
                              {JobState::Succeeded, record.updatedAt, "finish", "", 0}};
        verify("manifest.json", RecordToJson(record));
        for (auto state :
             {JobState::Succeeded, JobState::Failed, JobState::Cancelled, JobState::TimedOut})
            check(IsTerminal(state), "terminal state");
        check(!IsTerminal(JobState::Running) && !IsTerminal(JobState::Queued), "nonterminal state");
        input.request.timeoutSec = 10;
        input.inputSnapshotId = "snapshot-001";
        input.maxOutputBytes = 42;
        auto request = RequestToJson(input);
        check(request["timeout_sec"] == 10 && request["input_snapshot_id"] == "snapshot-001" &&
                  request["max_output_bytes"] == 42,
              "explicit execution settings");
        auto log = Json{{"type", "log"}, {"level", "info"}, {"line", "quoted \"RTL\"\n中文"}};
        check(Json::parse(log.dump()) == log && log.dump().find('\n') == std::string::npos,
              "JSONL escaping");
        std::cout << "Job protocol serialization and compatibility checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
