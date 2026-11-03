#include "VerilatorProviders.h"
#include "SimMainCodeGen.h"
#include "SimTimeline.h"
#include "StimulusModel.h"
#include "eda-core/JobPaths.h"
#include "eda-core/JobRuntime.h"
#include "eda-platform/Sha256.h"
#include <eda/api/process.hpp>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;
namespace eda::sim {
namespace {
std::string StringParam(const Json& params, const char* key) {
    if (!params.contains(key))
        return {};
    if (!params.at(key).is_string())
        throw std::invalid_argument(std::string(key) + " must be a string");
    auto value = params.at(key).get<std::string>();
    if (value.find('\0') != std::string::npos)
        throw std::invalid_argument(std::string(key) + " contains NUL");
    return value;
}
bool Identifier(const std::string& value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_'))
        return false;
    for (unsigned char c : value)
        if (!(std::isalnum(c) || c == '_'))
            return false;
    return true;
}
Error ProcessError(const ProcessResult& result, const std::string& name) {
    if (result.outcome == ProcessOutcome::Cancelled)
        return {ErrorCode::Cancelled, name + " cancelled", ""};
    if (result.outcome == ProcessOutcome::TimedOut)
        return {ErrorCode::TimedOut, name + " timed out", ""};
    if (!result.started)
        return {ErrorCode::Crashed, result.errorMessage, ""};
    if (result.exitCode != 0)
        return {ErrorCode::Internal, name + " exited with code " + std::to_string(result.exitCode),
                ""};
    return Error::Ok();
}
Json ReadJson(const fs::path& path) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > 2 * 1024 * 1024)
        throw std::invalid_argument("invalid build metadata file");
    std::ifstream stream(path, std::ios::binary);
    Json result;
    stream >> result;
    if (!stream || !result.is_object())
        throw std::invalid_argument("invalid build metadata JSON");
    return result;
}
fs::path VerifiedExecutable(JobContext& ctx, const std::string& name) {
    const auto& execution = web::ExecutionFor(ctx);
    auto path = web::CheckedPath(execution.projectPath, name, true);
    auto builds = execution.projectPath / ".sigflow/agent/jobs/sim.build";
    path = web::CheckedPath(builds, path, true);
    auto relative = path.lexically_relative(builds);
    auto it = relative.begin();
    if (it == relative.end())
        throw std::invalid_argument("sim_exe must be a build artifact");
    const auto jobId = it->string();
    ++it;
    if (it == relative.end() || *it != "artifacts")
        throw std::invalid_argument("sim_exe must be a build artifact");
    auto buildDir = web::CheckedPath(builds, builds / jobId, true);
    auto report = ReadJson(web::CheckedPath(buildDir, "reports/report.json", true));
    auto request = ReadJson(web::CheckedPath(buildDir, "request.json", true));
    if (report.value("schema_version", "") != "eda.jobreport.v1" ||
        report.value("state", "") != "Succeeded" || report.value("job_id", "") != jobId ||
        report.value("job_type", "") != "sim.build" ||
        report.value("plugin_id", "") != "eda-sim-verilator" ||
        request.value("schema_version", "") != "sigflow.execution-request.v1" ||
        request.value("job_id", "") != jobId || request.value("job_type", "") != "sim.build" ||
        request.value("project_id", "") != execution.request.projectId ||
        request.value("project_path", "") != execution.projectPath.string() ||
        request.value("work_dir", "") != buildDir.string())
        throw std::invalid_argument("simulation build identity or state mismatch");
    if (!report.contains("artifacts") || !report.at("artifacts").is_array())
        throw std::invalid_argument("simulation build has no artifacts");
    const auto digest = platform::Sha256FileHex(path);
    if (digest.size() != 64)
        throw std::invalid_argument("unable to hash simulation executable");
    bool found = false;
    for (const auto& artifact : report.at("artifacts")) {
        if (!artifact.is_object() || artifact.value("id", "") != "sim-executable" ||
            artifact.value("schema", "") != "eda.sim.verilator-executable.v1")
            continue;
        auto candidate =
            web::CheckedPath(buildDir / "artifacts", buildDir / artifact.value("path", ""), true);
        if (candidate == path && artifact.value("sha256", "") == digest)
            found = true;
    }
    if (!found || !fs::is_regular_file(path) || fs::file_size(path) == 0 ||
        access(path.c_str(), X_OK) != 0)
        throw std::invalid_argument(
            "simulation executable does not match a runnable build artifact");
    return path;
}
}  // namespace
Json BuildProvider::paramsSchema() const {
    return {{"type", "object"},
            {"required", {"top_module", "source_files"}},
            {"properties",
             {{"top_module", {{"type", "string"}}},
              {"source_files", {{"type", "array"}}},
              {"testbench", {{"type", "string"}}},
              {"out_dir", {{"type", "string"}}},
              {"executable", {{"type", "string"}}},
              {"verilator_path", {{"type", "string"}}}}}};
}
Json BuildProvider::resultSchema() const {
    return {{"type", "object"}, {"properties", {{"executable", {{"type", "string"}}}}}};
}
Error BuildProvider::startJob(const JobRequest& request, JobContext& ctx) {
    try {
        const auto& p = request.params;
        auto top = StringParam(p, "top_module");
        auto testbench = StringParam(p, "testbench");
        if (!Identifier(top) || (!testbench.empty() && !Identifier(testbench)))
            throw std::invalid_argument("invalid top_module or testbench identifier");
        if (!p.contains("source_files") || !p.at("source_files").is_array() ||
            p.at("source_files").empty())
            throw std::invalid_argument("sim.build requires source_files");
        std::vector<fs::path> sources;
        for (const auto& source : p.at("source_files")) {
            if (!source.is_string() || source.get<std::string>().empty() ||
                source.get<std::string>().find('\0') != std::string::npos)
                throw std::invalid_argument("source_files entries must be nonempty strings");
            sources.push_back(web::CheckedInput(ctx, source.get<std::string>()));
        }
        auto outParam = StringParam(p, "out_dir");
        auto outDir = web::CheckedPath(
            ctx.jobDir(), outParam.empty() ? ctx.jobDir() / "sim" : fs::path(outParam), false);
        auto exeParam = StringParam(p, "executable");
        auto artifactRoot = ctx.jobDir() / "artifacts";
        auto executable = web::CheckedPath(
            artifactRoot, exeParam.empty() ? artifactRoot / "sim_main" : fs::path(exeParam), false);
        if (outDir == ctx.jobDir() || executable == artifactRoot)
            throw std::invalid_argument("invalid build output path");
        Timeline timeline;
        timeline.topModuleName = top;
        timeline.maxSimTime = 200;
        if (!testbench.empty()) {
            bool found = false;
            const std::regex module("\\bmodule\\s+" + testbench + "\\b");
            for (const auto& source : sources) {
                std::ifstream input(source, std::ios::binary);
                std::string code((std::istreambuf_iterator<char>(input)), {});
                if (!std::regex_search(code, module))
                    continue;
                TestbenchInfo info;
                StimulusParser parser;
                if (!parser.ParseCode(code, info))
                    throw std::invalid_argument("unable to parse testbench");
                timeline = TimelineGenerator().Generate(info, top);
                found = true;
                break;
            }
            if (!found)
                throw std::invalid_argument("testbench module not found in sources");
        }
        ToolQuery query;
        query.name = "verilator";
        query.alternativeNames = {"verilator_bin", "verilator_bin_dbg"};
        query.environmentVariables = {"SIGFLOW_VERILATOR", "VERILATOR_BIN"};
        query.configuredPath = StringParam(p, "verilator_path");
        auto tool = web::ResolveExecutionTool(ctx, query);
        if (!tool.found)
            return {ErrorCode::NotFound, "verilator not found: " + tool.reason, ""};
        fs::create_directories(outDir);
        fs::create_directories(executable.parent_path());
        auto traceMain = web::CheckedPath(outDir, "sigflow_trace_main.cpp", false);
        SimMainGenerator generator;
        if (!generator.Generate(timeline, traceMain.string(), "waveform/wave.vcd"))
            throw std::runtime_error(generator.LastError());
        ProcessSpec spec;
        spec.executable = tool.path;
        spec.workingDirectory = outDir;
        spec.arguments = {"--cc", "--exe", "--build", "--trace", "-Wno-fatal", "--top-module", top};
        for (const auto& source : sources)
            spec.arguments.push_back(source.string());
        spec.arguments.insert(spec.arguments.end(),
                              {traceMain.string(), "-o", executable.string()});
        auto result = ctx.processHost().Run(
            spec, [&](const auto& chunk, bool error) { ctx.log(chunk, error); });
        ctx.emitMetric(Json{{"exit_code", result.exitCode}});
        auto error = ProcessError(result, "simulation build");
        if (error.code != ErrorCode::None)
            return error;
        executable = web::CheckedPath(artifactRoot, executable, true);
        if (!fs::is_regular_file(executable) || fs::file_size(executable) == 0 ||
            access(executable.c_str(), X_OK) != 0)
            throw std::runtime_error("verilator did not produce a runnable executable");
        Artifact artifact;
        artifact.id = "sim-executable";
        artifact.path = executable;
        artifact.schema = "eda.sim.verilator-executable.v1";
        artifact.role = "primary";
        ctx.registerArtifact(artifact);
        ctx.progress(100, "simulation build complete");
        return Error::Ok();
    } catch (const std::invalid_argument& error) {
        return {ErrorCode::InvalidArgument, error.what(), ""};
    } catch (const std::exception& error) {
        return {ErrorCode::Internal, error.what(), ""};
    }
}
Json RunProvider::paramsSchema() const {
    return {{"type", "object"},
            {"required", {"sim_exe"}},
            {"properties",
             {{"sim_exe", {{"type", "string"}}},
              {"working_dir", {{"type", "string"}}},
              {"vcd_out", {{"type", "string"}}},
              {"duration_ticks", {{"type", "string"}}},
              {"build_input_fingerprint", {{"type", "string"}}}}}};
}
Json RunProvider::resultSchema() const {
    return {{"type", "object"}, {"properties", {{"vcd", {{"type", "string"}}}}}};
}
Error RunProvider::startJob(const JobRequest& request, JobContext& ctx) {
    try {
        const auto& p = request.params;
        auto exe = StringParam(p, "sim_exe");
        if (exe.empty())
            throw std::invalid_argument("sim.run requires sim_exe");
        auto executable = VerifiedExecutable(ctx, exe);
        auto workingParam = StringParam(p, "working_dir");
        auto artifactRoot = ctx.jobDir() / "artifacts";
        auto working = web::CheckedPath(
            artifactRoot, workingParam.empty() ? artifactRoot : fs::path(workingParam), false);
        auto vcdParam = StringParam(p, "vcd_out");
        auto vcd = web::CheckedPath(
            artifactRoot, vcdParam.empty() ? working / "waveform/wave.vcd" : fs::path(vcdParam),
            false);
        auto fingerprint = StringParam(p, "build_input_fingerprint");
        std::string duration;
        if (p.contains("duration_ticks")) {
            duration = StringParam(p, "duration_ticks");
            std::uint64_t parsed = 0;
            auto [end, ec] =
                std::from_chars(duration.data(), duration.data() + duration.size(), parsed);
            if (duration.empty() || duration.size() > 20 || ec != std::errc() ||
                end != duration.data() + duration.size())
                throw std::invalid_argument("duration_ticks must be a decimal uint64 string");
            duration = std::to_string(parsed);
        }
        fs::create_directories(working / "waveform");
        ProcessSpec spec;
        spec.executable = executable;
        spec.workingDirectory = working;
        if (p.contains("duration_ticks"))
            spec.arguments = {"--sigflow-duration-ticks", duration};
        auto result = ctx.processHost().Run(
            spec, [&](const auto& chunk, bool error) { ctx.log(chunk, error); });
        ctx.emitMetric(Json{{"exit_code", result.exitCode}});
        if (p.contains("duration_ticks"))
            ctx.emitMetric(Json{{"duration_ticks", duration}});
        if (p.contains("build_input_fingerprint"))
            ctx.emitMetric(Json{{"build_input_fingerprint", fingerprint}});
        auto error = ProcessError(result, "simulation");
        if (error.code != ErrorCode::None)
            return error;
        vcd = web::CheckedPath(artifactRoot, vcd, true);
        if (!fs::is_regular_file(vcd) || fs::file_size(vcd) == 0)
            throw std::runtime_error("simulation did not produce a nonempty VCD waveform");
        Artifact artifact;
        artifact.id = "waveform";
        artifact.path = vcd;
        artifact.schema = "eda.wave.vcd.v1";
        artifact.role = "primary";
        ctx.registerArtifact(artifact);
        ctx.progress(100, "simulation complete");
        return Error::Ok();
    } catch (const std::invalid_argument& error) {
        return {ErrorCode::InvalidArgument, error.what(), ""};
    } catch (const std::exception& error) {
        return {ErrorCode::Internal, error.what(), ""};
    }
}
}  // namespace eda::sim
