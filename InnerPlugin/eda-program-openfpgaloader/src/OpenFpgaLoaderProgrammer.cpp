#include "OpenFpgaLoaderProgrammer.h"

#include "eda-core/Toolchain.h"

#include <eda/api/abi.h>
#include <eda/api/plugin_registry.h>
#include <eda/api/process.hpp>
#include <eda/api/toolchain.hpp>

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
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

bool GowinCrcError(const std::string& output) {
    constexpr const char* marker = "after program sram: displayReadReg ";
    const std::size_t at = output.rfind(marker);
    if (at == std::string::npos) return false;
    const char* hex = output.c_str() + at + std::char_traits<char>::length(marker);
    char* end = nullptr;
    const unsigned long status = std::strtoul(hex, &end, 16);
    return end != hex && (status & 1UL) != 0;
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
            {"arguments", {{"type", "array"}}},
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
    if (params.is_object() && params.contains("arguments") && params["arguments"].is_array()) {
        for (const auto& argument : params["arguments"]) {
            if (argument.is_string()) spec.arguments.push_back(argument.get<std::string>());
        }
    }
    if (spec.arguments.empty()) spec.arguments = {"-b", board, bitstream};
    if (std::find(spec.arguments.begin(), spec.arguments.end(), "-v") == spec.arguments.end()) {
        spec.arguments.insert(spec.arguments.begin(), "-v");
    }
    if (std::find(spec.arguments.begin(), spec.arguments.end(), bitstream) == spec.arguments.end()) {
        spec.arguments.push_back(bitstream);
    }
    spec.workingDirectory = ctx.jobDir();
    std::string output;
    std::mutex outputMutex;
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx, &output, &outputMutex](const std::string& line, bool isError) {
            {
                std::lock_guard<std::mutex> lock(outputMutex);
                output += line;
                if (output.size() > 1024 * 1024) output.erase(0, output.size() - 1024 * 1024);
            }
            ctx.log(line, isError);
        });

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
    if (GowinCrcError(output)) {
        return Error{ErrorCode::Internal,
                     "FPGA rejected bitstream: Gowin status register reports CRC Error", ""};
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
