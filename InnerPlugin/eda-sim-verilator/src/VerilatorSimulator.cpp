#include "VerilatorSimulator.h"

#include "Platform.h"
#include "eda-core/Toolchain.h"

#include <eda/api/abi.h>
#include <eda/api/plugin_registry.h>
#include <eda/api/process.hpp>
#include <eda/api/toolchain.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace eda {
namespace sim {
namespace {

std::string ParamString(const Json& params, const char* key, const std::string& fallback = {}) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

} // namespace

Json VerilatorSimulator::paramsSchema() const {
    return Json{
        {"type", "object"},
        {"required", {"top_module", "source_files"}},
        {"properties", {
            {"top_module", {{"type", "string"}}},
            {"source_files", {{"type", "array"}}},
            {"testbench", {{"type", "string"}}},
            {"out_dir", {{"type", "string"}}},
            {"executable", {{"type", "string"}}},
            {"verilator_path", {{"type", "string"}}},
        }},
    };
}

Json VerilatorSimulator::resultSchema() const {
    return Json{{"type", "object"}, {"properties", {{"executable", {{"type", "string"}}}}}};
}

Error VerilatorSimulator::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;
    const std::string topModule = ParamString(params, "top_module");
    if (topModule.empty()) {
        return Error{ErrorCode::InvalidArgument, "sim.build requires top_module", ""};
    }
    std::vector<std::string> sourceFiles;
    if (params.is_object() && params.contains("source_files") &&
        params["source_files"].is_array()) {
        for (const auto& entry : params["source_files"]) {
            if (entry.is_string()) sourceFiles.push_back(entry.get<std::string>());
        }
    }
    if (sourceFiles.empty()) {
        return Error{ErrorCode::InvalidArgument, "sim.build requires at least one source file", ""};
    }
    const std::string testbench = ParamString(params, "testbench");

    std::filesystem::path outDir = ParamString(params, "out_dir");
    if (outDir.empty()) outDir = ctx.jobDir() / "sim";
    std::error_code dirError;
    std::filesystem::create_directories(outDir, dirError);

    std::string executable = ParamString(params, "executable");
    if (executable.empty()) {
        executable = (outDir / ("sim_main" + std::string(platform::ExecutableSuffix()))).string();
    }

    std::string verilatorPath = ParamString(params, "verilator_path");
    if (verilatorPath.empty()) {
        ToolQuery query;
        query.name = "verilator_bin";
        query.alternativeNames = {"verilator_bin_dbg", "verilator"};
        query.environmentVariables = {"VERILATOR_BIN", "SIGFLOW_VERILATOR"};
        query.searchPath = true;
        const ToolResolution resolution = DefaultToolchain().Resolve(query);
        if (!resolution.found) {
            return Error{ErrorCode::NotFound, "verilator not found: " + resolution.reason, ""};
        }
        verilatorPath = resolution.path.string();
    }

    ProcessSpec spec;
    spec.executable = verilatorPath;
    spec.arguments = {"--binary", "--trace", "-Wno-fatal", "--top-module", topModule};
    if (!testbench.empty()) spec.arguments.push_back(testbench);
    for (const std::string& source : sourceFiles) spec.arguments.push_back(source);
    spec.arguments.push_back("-o");
    spec.arguments.push_back(executable);
    spec.workingDirectory = outDir;
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "simulation build cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "simulation build timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }
    if (process.exitCode != 0) {
        return Error{ErrorCode::Internal,
                     "verilator exited with code " + std::to_string(process.exitCode), ""};
    }

    std::error_code existsError;
    if (!std::filesystem::exists(executable, existsError)) {
        return Error{ErrorCode::Internal,
                     "verilator did not produce the expected executable: " + executable, ""};
    }

    Artifact artifact;
    artifact.id = "sim-executable";
    artifact.path = executable;
    artifact.schema = "eda.sim.verilator-executable.v1";
    artifact.role = "primary";
    ctx.registerArtifact(artifact);
    ctx.progress(100, "simulation build complete");
    return Error::Ok();
}

void VerilatorSimulator::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    onReply(Error{ErrorCode::Unsupported, "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t VerilatorSimulator::subscribe(const std::string&, EventHandler<const Json&>) {
    return 0;
}

void VerilatorSimulator::unsubscribe(std::uint64_t) {}

PluginInfo VerilatorSimulator::info() const {
    PluginInfo info;
    info.id = "eda-sim-verilator";
    info.version = "1.0.0";
    info.displayName = "Verilator Simulator";
    info.vendor = "SigFlow";
    info.location = "inner";
    info.runtime = "inprocess";
    info.capabilities = {"sim/verilator"};
    info.methods = {"sim_build"};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace sim
} // namespace eda

EDA_REGISTER_PLUGIN(VerilatorSimulator, eda::sim::VerilatorSimulator, "eda-sim-verilator");
