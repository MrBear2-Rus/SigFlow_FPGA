#include "YosysSynthesizer.h"

#include "YosysArtifactValidator.h"
#include "YosysScriptGenerator.h"

#include "eda-core/Toolchain.h"

#include <eda/api/abi.h>
#include <eda/api/plugin_registry.h>
#include <eda/api/process.hpp>
#include <eda/api/toolchain.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace eda {
namespace synth {
namespace {

std::string ParamString(const Json& params, const char* key, const std::string& fallback = {}) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

} // namespace

Json YosysSynthesizer::paramsSchema() const {
    return Json{
        {"type", "object"},
        {"required", {"top_module", "source_files"}},
        {"properties", {
            {"top_module", {{"type", "string"}}},
            {"source_files", {{"type", "array"}}},
            {"strategy", {{"type", "string"}}},
            {"yosys_family", {{"type", "string"}}},
            {"output_json", {{"type", "string"}}},
            {"yosys_path", {{"type", "string"}}},
        }},
    };
}

Json YosysSynthesizer::resultSchema() const {
    return Json{
        {"type", "object"},
        {"properties", {{"netlist", {{"type", "string"}}}}},
    };
}

Error YosysSynthesizer::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;

    YosysScriptRequest scriptRequest;
    scriptRequest.topModule = ParamString(params, "top_module");
    scriptRequest.targetProfileId = ParamString(params, "target_profile");
    scriptRequest.targetProfileVersion = ParamString(params, "target_version");
    scriptRequest.yosysFamily = ParamString(params, "yosys_family", "gw1n");
    scriptRequest.outputJsonPath = ParamString(params, "output_json");
    {
        YosysStrategy strategy = YosysStrategy::Baseline;
        ParseStrategy(ParamString(params, "strategy", "baseline"), strategy);
        scriptRequest.strategy = strategy;
    }
    if (params.is_object() && params.contains("source_files") &&
        params["source_files"].is_array()) {
        for (const auto& entry : params["source_files"]) {
            if (entry.is_string()) scriptRequest.sourceFiles.push_back(entry.get<std::string>());
        }
    }
    if (scriptRequest.outputJsonPath.empty()) {
        scriptRequest.outputJsonPath = (ctx.jobDir() / "artifacts" / "top.json").string();
    }

    const YosysScriptResult generated = YosysScriptGenerator().Generate(scriptRequest);
    if (!generated.success) {
        return Error{ErrorCode::InvalidArgument, generated.errorMessage, ""};
    }

    const std::filesystem::path scriptPath = ctx.jobDir() / "run_yosys.ys";
    {
        std::error_code error;
        std::filesystem::create_directories(scriptPath.parent_path(), error);
        std::ofstream out(scriptPath, std::ios::trunc | std::ios::binary);
        if (!out) {
            return Error{ErrorCode::Internal, "unable to write yosys script", ""};
        }
        out << generated.script;
    }
    ctx.log("Yosys script written: " + scriptPath.string(), false);

    std::string yosysPath = ParamString(params, "yosys_path");
    if (yosysPath.empty()) {
        ToolQuery query;
        query.name = "yosys";
        query.environmentVariables = {"SIGFLOW_YOSYS"};
        query.searchPath = true;
        const ToolResolution resolution = DefaultToolchain().Resolve(query);
        if (!resolution.found) {
            return Error{ErrorCode::NotFound, "yosys not found: " + resolution.reason, ""};
        }
        yosysPath = resolution.path.string();
    }

    ProcessSpec spec;
    spec.executable = yosysPath;
    spec.arguments = {"-s", scriptPath.string()};
    spec.workingDirectory = ctx.jobDir();
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "synthesis cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "synthesis timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }
    if (process.exitCode != 0) {
        return Error{ErrorCode::Internal,
                     "yosys exited with code " + std::to_string(process.exitCode), ""};
    }

    std::error_code existsError;
    if (!std::filesystem::exists(scriptRequest.outputJsonPath, existsError)) {
        return Error{ErrorCode::Internal,
                     "yosys did not produce the expected netlist: " + scriptRequest.outputJsonPath,
                     ""};
    }

    ArtifactValidator validator;
    NetlistArtifactReport validation;
    if (!validator.ValidateYosysJson(scriptRequest.outputJsonPath, scriptRequest.topModule,
                                     validation)) {
        return Error{ErrorCode::Internal, "netlist validation failed: " + validation.message, ""};
    }
    std::string manifestError;
    validator.WriteManifest(
        (ctx.jobDir() / "artifacts" / (scriptRequest.topModule + ".manifest.json")).string(),
        validation, manifestError);
    ctx.emitMetric(Json{{"ports", validation.portCount},
                        {"cells", validation.cellCount},
                        {"netnames", validation.netnameCount}});

    Artifact artifact;
    artifact.id = "netlist";
    artifact.path = scriptRequest.outputJsonPath;
    artifact.schema = "eda.netlist.yosys-json.v1";
    artifact.sha256 = validation.sha256;
    artifact.role = "primary";
    ctx.registerArtifact(artifact);
    ctx.progress(100, "synthesis complete");
    return Error::Ok();
}

void YosysSynthesizer::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    // 类型化方法经 invoke 的默认路径不在本插件使用（Job 执行走 startJob）。
    onReply(Error{ErrorCode::Unsupported,
                  "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t YosysSynthesizer::subscribe(const std::string&,
                                          EventHandler<const Json&>) {
    return 0;
}

void YosysSynthesizer::unsubscribe(std::uint64_t) {}

PluginInfo YosysSynthesizer::info() const {
    PluginInfo info;
    info.id = "eda-synth-yosys";
    info.version = "1.0.0";
    info.displayName = "Yosys Synthesizer";
    info.vendor = "SigFlow";
    info.location = "inner";
    info.runtime = "inprocess";
    info.capabilities = {"synth/yosys"};
    info.methods = {"synth_run"};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace synth
} // namespace eda

EDA_REGISTER_PLUGIN(YosysSynthesizer, eda::synth::YosysSynthesizer, "eda-synth-yosys");
