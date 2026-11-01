#pragma once
#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include "eda-core/JobPaths.h"
#include "eda-core/JobRuntime.h"
#include <fstream>
#include <thread>
#include <chrono>
class RuntimeFixtureProvider final : public eda::IJobProvider {
  public:
    std::string jobType() const override { return "synth"; }
    eda::Json paramsSchema() const override { return eda::Json::object(); }
    eda::Json resultSchema() const override { return eda::Json::object(); }
    eda::Error startJob(const eda::JobRequest& request, eda::JobContext& ctx) override {
        const auto& execution = eda::web::ExecutionFor(ctx);
        ctx.emitMetric(eda::Json{{"data_dir", execution.dataDir.string()},
                                 {"tool_root", execution.toolRoot.string()}});
        if (request.params.contains("input")) {
            auto path = eda::web::CheckedInput(ctx, request.params.at("input").get<std::string>());
            std::ifstream stream(path);
            std::string line;
            std::getline(stream, line);
            if (!stream)
                throw std::runtime_error("fixture input read failed");
        }
        if (request.params.contains("resolve_tool")) {
            eda::ToolQuery query;
            query.name = request.params.at("resolve_tool").get<std::string>();
            query.environmentVariables = {"SIGFLOW_YOSYS"};
            auto resolved = eda::web::ResolveExecutionTool(ctx, query);
            if (!resolved.found)
                throw std::runtime_error(resolved.reason);
            ctx.emitMetric(eda::Json{{"resolved_tool", resolved.path.string()}});
        }
        auto mode = request.params.value("mode", std::string("success"));
        eda::ProcessSpec spec;
        spec.executable = "/bin/sh";
        spec.workingDirectory = ctx.jobDir();
        spec.arguments = {"-c", "printf 'quoted \"RTL\" 中文\\n'; printf 'warning\\n' >&2"};
        if (mode == "nonzero")
            spec.arguments = {"-c", "exit 17"};
        if (mode == "launch")
            spec.executable = "/sigflow/no-such-tool";
        if (mode == "sleep")
            spec.arguments = {"-c", "echo $$ > tool.pid; sleep 30 & echo $! >> tool.pid; wait"};
        if (mode == "long") {
            spec.executable = "/usr/bin/python3";
            spec.arguments = {"-c", "print('x'*100000)"};
        }
        if (mode == "multi")
            spec.arguments = {"-c", "sleep 0.7"};
        if (mode == "bad-cwd")
            spec.workingDirectory = ctx.jobDir().parent_path();
        if (mode == "exception")
            throw std::runtime_error("fixture exception");
        if (mode == "input-escape")
            eda::web::CheckedPath(ctx.jobDir(), "../outside.sv", true);
        auto result = ctx.processHost().Run(
            spec, [&](const auto& text, bool error) { ctx.log(text, error); });
        if (mode == "multi" && result.outcome == eda::ProcessOutcome::Success)
            result = ctx.processHost().Run(
                spec, [&](const auto& text, bool error) { ctx.log(text, error); });
        if (result.outcome != eda::ProcessOutcome::Success) {
            auto code = result.outcome == eda::ProcessOutcome::TimedOut ? eda::ErrorCode::TimedOut
                        : result.outcome == eda::ProcessOutcome::Cancelled
                            ? eda::ErrorCode::Cancelled
                            : eda::ErrorCode::Internal;
            return {code, "fixture tool failed", ""};
        }
        if (mode == "bad-report")
            std::filesystem::create_directories(ctx.jobDir() / "reports/report.json");
        auto path = eda::web::CheckedPath(ctx.jobDir(), "artifacts/result.txt", false);
        std::ofstream(path) << "verified artifact\n";
        if (mode == "symlink") {
            std::filesystem::remove(path);
            std::filesystem::create_symlink("../request.json", path);
        }
        eda::Artifact artifact;
        artifact.id = "fixture";
        artifact.path = path;
        artifact.schema = "test.fixture.v1";
        artifact.role = "primary";
        if (mode == "artifact-escape")
            artifact.path = ctx.jobDir().parent_path() / "outside.txt";
        if (mode == "digest")
            artifact.sha256 = std::string(64, '0');
        if (mode != "empty")
            ctx.registerArtifact(artifact);
        if (mode == "duplicate")
            ctx.registerArtifact(artifact);
        if (mode == "mutate")
            std::ofstream(path) << "mutated\n";
        ctx.emitMetric(eda::Json{{"fixture", true}});
        ctx.progress(mode == "bad-progress" ? 101 : 100, "complete");
        return eda::Error::Ok();
    }
};
