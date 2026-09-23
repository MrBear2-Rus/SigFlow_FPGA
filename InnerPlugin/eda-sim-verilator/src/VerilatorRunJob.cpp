#include "VerilatorRunJob.h"

#include <eda/api/abi.h>
#include <eda/api/plugin_registry.h>
#include <eda/api/process.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace eda {
namespace sim {
namespace {

std::string ParamString(const Json& params, const char* key, const std::string& fallback = {}) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

std::string FindVcd(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) return {};
    for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end;
         it.increment(error)) {
        if (error) break;
        if (it->is_regular_file(error) && it->path().extension() == ".vcd") {
            return it->path().string();
        }
    }
    return {};
}

} // namespace

Json VerilatorRunJob::paramsSchema() const {
    return Json{
        {"type", "object"},
        {"required", {"sim_exe"}},
        {"properties", {
            {"sim_exe", {{"type", "string"}}},
            {"working_dir", {{"type", "string"}}},
            {"vcd_out", {{"type", "string"}}},
        }},
    };
}

Json VerilatorRunJob::resultSchema() const {
    return Json{{"type", "object"}, {"properties", {{"vcd", {{"type", "string"}}}}}};
}

Error VerilatorRunJob::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;
    const std::string simExe = ParamString(params, "sim_exe");
    if (simExe.empty()) {
        return Error{ErrorCode::InvalidArgument, "sim.run requires sim_exe", ""};
    }
    std::filesystem::path workingDir = ParamString(params, "working_dir");
    if (workingDir.empty()) workingDir = ctx.jobDir() / "sim";
    std::error_code dirError;
    std::filesystem::create_directories(workingDir, dirError);

    ProcessSpec spec;
    spec.executable = simExe;
    spec.workingDirectory = workingDir;
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "simulation cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "simulation timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }
    if (process.exitCode != 0) {
        return Error{ErrorCode::Internal,
                     "simulation exited with code " + std::to_string(process.exitCode), ""};
    }

    std::string vcd = ParamString(params, "vcd_out");
    std::error_code existsError;
    if (vcd.empty() || !std::filesystem::exists(vcd, existsError)) {
        vcd = FindVcd(workingDir);
    }
    if (vcd.empty()) {
        return Error{ErrorCode::Internal, "simulation did not produce a VCD waveform", ""};
    }

    Artifact artifact;
    artifact.id = "waveform";
    artifact.path = vcd;
    artifact.schema = "eda.wave.vcd.v1";
    artifact.role = "primary";
    ctx.registerArtifact(artifact);
    ctx.progress(100, "simulation complete");
    return Error::Ok();
}

void VerilatorRunJob::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    onReply(Error{ErrorCode::Unsupported, "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t VerilatorRunJob::subscribe(const std::string&, EventHandler<const Json&>) {
    return 0;
}

void VerilatorRunJob::unsubscribe(std::uint64_t) {}

PluginInfo VerilatorRunJob::info() const {
    PluginInfo info;
    info.id = "eda-sim-verilator-run";
    info.version = "1.0.0";
    info.displayName = "Verilator Run";
    info.vendor = "SigFlow";
    info.location = "inner";
    info.runtime = "inprocess";
    info.capabilities = {"sim/verilator"};
    info.methods = {"sim_run"};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace sim
} // namespace eda

EDA_REGISTER_PLUGIN(VerilatorRunJob, eda::sim::VerilatorRunJob, "eda-sim-verilator-run");
