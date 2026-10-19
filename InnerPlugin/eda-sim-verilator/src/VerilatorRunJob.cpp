#include "VerilatorRunJob.h"
#include "Sha256.h"

#include <eda/api/abi.h>
#include <eda/api/plugin_registry.h>
#include <eda/api/process.hpp>

#include <cstdint>
#include <charconv>
#include <algorithm>
#include <cctype>
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

bool ParseDuration(const Json& params, std::uint64_t& duration, std::string& error) {
    if (!params.is_object() || !params.contains("duration_ticks")) return true;
    if (!params["duration_ticks"].is_string()) {
        error = "duration_ticks must be a decimal uint64 string";
        return false;
    }
    const std::string value = params["duration_ticks"].get<std::string>();
    if (value.empty() || value.size() > 20 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        error = "duration_ticks must be a decimal uint64 string";
        return false;
    }
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), duration);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) {
        error = "duration_ticks exceeds uint64";
        return false;
    }
    return true;
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
            {"duration_ticks", {{"type", "string"}, {"pattern", "^[0-9]{1,20}$"}}},
            {"build_input_fingerprint", {{"type", "string"}}},
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
    std::uint64_t duration = 0;
    std::string durationError;
    if (!ParseDuration(params, duration, durationError)) {
        return Error{ErrorCode::InvalidArgument, durationError, ""};
    }
    std::filesystem::path workingDir = ParamString(params, "working_dir");
    // The generated harness intentionally uses a relative waveform/wave.vcd
    // path.  Run beside the trusted executable by default so the build/run
    // pair has one stable runtime layout.  Gateway callers cannot set this
    // field; it is derived from a succeeded sim.build artifact.
    if (workingDir.empty()) workingDir = std::filesystem::path(simExe).parent_path();
    std::error_code dirError;
    std::filesystem::create_directories(workingDir, dirError);
    // SimMainCodeGen writes the trace to waveform/wave.vcd relative to the
    // process working directory.  A run job owns a fresh work directory, so
    // create that relative destination before starting the trusted executable.
    // Without this, a successful executable can silently leave no VCD and the
    // gateway has nothing safe to publish through /waves.
    if (!dirError) std::filesystem::create_directories(workingDir / "waveform", dirError);
    if (dirError) {
        return Error{ErrorCode::Internal,
                     "unable to create simulation waveform directory: " + dirError.message(), ""};
    }

    ProcessSpec spec;
    spec.executable = simExe;
    if (params.contains("duration_ticks")) {
        spec.arguments = {"--sigflow-duration-ticks", std::to_string(duration)};
    }
    spec.workingDirectory = workingDir;
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (params.contains("duration_ticks")) {
        ctx.emitMetric(Json{{"duration_ticks", std::to_string(duration)}});
    }
    if (params.contains("build_input_fingerprint") && params["build_input_fingerprint"].is_string()) {
        ctx.emitMetric(Json{{"build_input_fingerprint", params["build_input_fingerprint"]}});
    }
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
    artifact.sha256 = platform::Sha256FileHex(artifact.path);
    if (artifact.sha256.empty()) {
        return Error{ErrorCode::Internal, "unable to hash simulation waveform", ""};
    }
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
