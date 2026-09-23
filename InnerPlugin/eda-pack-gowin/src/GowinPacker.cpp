#include "GowinPacker.h"

#include "GowinPackService.h"

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
namespace pack {
namespace {

std::string ParamString(const Json& params, const char* key, const std::string& fallback = {}) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

} // namespace

Json GowinPacker::paramsSchema() const {
    return Json{
        {"type", "object"},
        {"required", {"pnr_json", "device"}},
        {"properties", {
            {"pnr_json", {{"type", "string"}}},
            {"device", {{"type", "string"}}},
            {"output", {{"type", "string"}}},
            {"gowin_pack_path", {{"type", "string"}}},
        }},
    };
}

Json GowinPacker::resultSchema() const {
    return Json{{"type", "object"}, {"properties", {{"bitstream", {{"type", "string"}}}}}};
}

Error GowinPacker::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;
    const std::string pnrJson = ParamString(params, "pnr_json");
    const std::string device = ParamString(params, "device");
    std::string output = ParamString(params, "output");
    if (pnrJson.empty() || device.empty()) {
        return Error{ErrorCode::InvalidArgument, "pack requires pnr_json and device", ""};
    }
    if (output.empty()) {
        output = (ctx.jobDir() / "artifacts" / "design.fs").string();
    }

    std::string packerPath = ParamString(params, "gowin_pack_path");
    if (packerPath.empty()) {
        ToolQuery query;
        query.name = "gowin_pack";
        query.environmentVariables = {"SIGFLOW_GOWIN_PACK"};
        query.searchPath = true;
        const ToolResolution resolution = DefaultToolchain().Resolve(query);
        if (!resolution.found) {
            return Error{ErrorCode::NotFound, "gowin_pack not found: " + resolution.reason, ""};
        }
        packerPath = resolution.path.string();
    }

    ProcessSpec spec;
    spec.executable = packerPath;
    spec.arguments = {"-d", device, "-o", output, pnrJson};
    spec.workingDirectory = ctx.jobDir();

    PackService packService;
    std::string validationError;
    if (!packService.ValidateInput(pnrJson, validationError)) {
        return Error{ErrorCode::InvalidArgument, validationError, ""};
    }

    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "pack cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "pack timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }

    PackReport packReport;
    if (!packService.Finalize(PackRequest{pnrJson, output, packerPath, device},
                              process.exitCode, packReport)) {
        return Error{ErrorCode::Internal, packReport.message, ""};
    }
    std::string manifestError;
    packService.WriteManifest(
        (ctx.jobDir() / "artifacts" /
         (std::filesystem::path(output).stem().string() + ".pack.manifest.json"))
            .string(),
        packReport, manifestError);

    Artifact artifact;
    artifact.id = "bitstream";
    artifact.path = output;
    artifact.schema = "eda.bitstream.apicula.v1";
    artifact.sha256 = packReport.bitstreamSha256;
    artifact.role = "primary";
    ctx.registerArtifact(artifact);
    ctx.progress(100, "pack complete");
    return Error::Ok();
}

void GowinPacker::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    onReply(Error{ErrorCode::Unsupported, "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t GowinPacker::subscribe(const std::string&, EventHandler<const Json&>) { return 0; }

void GowinPacker::unsubscribe(std::uint64_t) {}

PluginInfo GowinPacker::info() const {
    PluginInfo info;
    info.id = "eda-pack-gowin";
    info.version = "1.0.0";
    info.displayName = "Apicula Gowin Packer";
    info.vendor = "SigFlow";
    info.location = "inner";
    info.runtime = "inprocess";
    info.capabilities = {"pack/gowin"};
    info.methods = {"pack_run"};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace pack
} // namespace eda

EDA_REGISTER_PLUGIN(GowinPacker, eda::pack::GowinPacker, "eda-pack-gowin");
