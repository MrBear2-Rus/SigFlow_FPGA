#include "NextpnrPlaceRouter.h"

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

namespace eda {
namespace pnr {
namespace {

std::string ParamString(const Json& params, const char* key, const std::string& fallback = {}) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

} // namespace

Json NextpnrPlaceRouter::paramsSchema() const {
    return Json{
        {"type", "object"},
        {"required", {"top_module", "netlist", "device"}},
        {"properties", {
            {"top_module", {{"type", "string"}}},
            {"netlist", {{"type", "string"}}},
            {"device", {{"type", "string"}}},
            {"family", {{"type", "string"}}},
            {"cst", {{"type", "string"}}},
            {"output", {{"type", "string"}}},
            {"nextpnr_path", {{"type", "string"}}},
        }},
    };
}

Json NextpnrPlaceRouter::resultSchema() const {
    return Json{
        {"type", "object"},
        {"properties", {{"pnr_json", {{"type", "string"}}}}},
    };
}

Error NextpnrPlaceRouter::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;
    const std::string topModule = ParamString(params, "top_module");
    const std::string netlist = ParamString(params, "netlist");
    const std::string device = ParamString(params, "device");
    const std::string family = ParamString(params, "family");
    const std::string cst = ParamString(params, "cst");
    std::string output = ParamString(params, "output");

    if (topModule.empty() || netlist.empty() || device.empty()) {
        return Error{ErrorCode::InvalidArgument,
                     "pnr requires top_module, netlist and device", ""};
    }
    if (output.empty()) {
        output = (ctx.jobDir() / "artifacts" / (topModule + ".pnr.json")).string();
    }

    std::string nextpnrPath = ParamString(params, "nextpnr_path");
    if (nextpnrPath.empty()) {
        ToolQuery query;
        query.name = "nextpnr-himbaechel";
        query.alternativeNames = {"nextpnr-himbaechel", "nextpnr-gowin"};
        query.environmentVariables = {"SIGFLOW_NEXTPNR"};
        query.searchPath = true;
        const ToolResolution resolution = DefaultToolchain().Resolve(query);
        if (!resolution.found) {
            return Error{ErrorCode::NotFound,
                         "nextpnr not found: " + resolution.reason, ""};
        }
        nextpnrPath = resolution.path.string();
    }

    ProcessSpec spec;
    spec.executable = nextpnrPath;
    spec.arguments = {"--json", netlist, "--write", output, "--device", device,
                      "--vopt", "family=" + family};
    if (!cst.empty()) {
        spec.arguments.push_back("--vopt");
        spec.arguments.push_back("cst=" + cst);
    }
    spec.workingDirectory = ctx.jobDir();
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "place and route cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "place and route timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }
    if (process.exitCode != 0) {
        return Error{ErrorCode::Internal,
                     "nextpnr exited with code " + std::to_string(process.exitCode), ""};
    }

    std::error_code existsError;
    if (!std::filesystem::exists(output, existsError)) {
        return Error{ErrorCode::Internal,
                     "nextpnr did not produce the expected output: " + output, ""};
    }

    Artifact artifact;
    artifact.id = "pnr-json";
    artifact.path = output;
    artifact.schema = "eda.pnr.nextpnr-json.v1";
    artifact.role = "primary";
    ctx.registerArtifact(artifact);
    ctx.progress(100, "place and route complete");
    return Error::Ok();
}

void NextpnrPlaceRouter::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    onReply(Error{ErrorCode::Unsupported, "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t NextpnrPlaceRouter::subscribe(const std::string&, EventHandler<const Json&>) {
    return 0;
}

void NextpnrPlaceRouter::unsubscribe(std::uint64_t) {}

PluginInfo NextpnrPlaceRouter::info() const {
    PluginInfo info;
    info.id = "eda-pnr-nextpnr";
    info.version = "1.0.0";
    info.displayName = "nextpnr Place and Route";
    info.vendor = "SigFlow";
    info.location = "inner";
    info.runtime = "inprocess";
    info.capabilities = {"pnr/nextpnr"};
    info.methods = {"pnr_run"};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace pnr
} // namespace eda

EDA_REGISTER_PLUGIN(NextpnrPlaceRouter, eda::pnr::NextpnrPlaceRouter, "eda-pnr-nextpnr");
