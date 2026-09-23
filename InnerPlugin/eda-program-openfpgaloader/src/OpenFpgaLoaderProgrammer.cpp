#include "OpenFpgaLoaderProgrammer.h"

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
namespace program {
namespace {

std::string ParamString(const Json& params, const char* key, const std::string& fallback = {}) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

} // namespace

Json OpenFpgaLoaderProgrammer::paramsSchema() const {
    return Json{
        {"type", "object"},
        {"required", {"bitstream"}},
        {"properties", {
            {"bitstream", {{"type", "string"}}},
            {"board", {{"type", "string"}}},
            {"openfpgaloader_path", {{"type", "string"}}},
        }},
    };
}

Json OpenFpgaLoaderProgrammer::resultSchema() const {
    return Json{{"type", "object"}, {"properties", {{"programmed", {{"type", "boolean"}}}}}};
}

Error OpenFpgaLoaderProgrammer::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;
    const std::string bitstream = ParamString(params, "bitstream");
    const std::string board = ParamString(params, "board", "tangnano9k");
    if (bitstream.empty()) {
        return Error{ErrorCode::InvalidArgument, "flash requires a bitstream path", ""};
    }

    std::string loaderPath = ParamString(params, "openfpgaloader_path");
    if (loaderPath.empty()) {
        ToolQuery query;
        query.name = "openFPGALoader";
        query.alternativeNames = {"openFPGALoader", "openfpgaloader"};
        query.environmentVariables = {"SIGFLOW_OPENFPGALOADER"};
        query.searchPath = true;
        const ToolResolution resolution = DefaultToolchain().Resolve(query);
        if (!resolution.found) {
            return Error{ErrorCode::NotFound,
                         "openFPGALoader not found: " + resolution.reason, ""};
        }
        loaderPath = resolution.path.string();
    }

    ProcessSpec spec;
    spec.executable = loaderPath;
    spec.arguments = {"-b", board, bitstream};
    spec.workingDirectory = ctx.jobDir();
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "flash cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "flash timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }
    if (process.exitCode != 0) {
        return Error{ErrorCode::Internal,
                     "openFPGALoader exited with code " + std::to_string(process.exitCode), ""};
    }

    ctx.progress(100, "flash complete");
    return Error::Ok();
}

void OpenFpgaLoaderProgrammer::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    onReply(Error{ErrorCode::Unsupported, "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t OpenFpgaLoaderProgrammer::subscribe(const std::string&,
                                                  EventHandler<const Json&>) {
    return 0;
}

void OpenFpgaLoaderProgrammer::unsubscribe(std::uint64_t) {}

PluginInfo OpenFpgaLoaderProgrammer::info() const {
    PluginInfo info;
    info.id = "eda-program-openfpgaloader";
    info.version = "1.0.0";
    info.displayName = "openFPGALoader Programmer";
    info.vendor = "SigFlow";
    info.location = "inner";
    info.runtime = "inprocess";
    info.capabilities = {"program/openfpgaloader"};
    info.methods = {"flash_run"};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace program
} // namespace eda

EDA_REGISTER_PLUGIN(OpenFpgaLoaderProgrammer, eda::program::OpenFpgaLoaderProgrammer,
                    "eda-program-openfpgaloader");
