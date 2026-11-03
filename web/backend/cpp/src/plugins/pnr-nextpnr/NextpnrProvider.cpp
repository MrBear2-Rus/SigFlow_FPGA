#include "NextpnrProvider.h"
#include "NextpnrAnalysis.h"
#include "eda-core/JobPaths.h"
#include "eda-core/JobRuntime.h"
#include <eda/api/process.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace eda::pnr {
namespace {
bool IsIdentifier(const std::string& value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_'))
        return false;
    for (unsigned char c : value)
        if (!(std::isalnum(c) || c == '_' || c == '$'))
            return false;
    return true;
}
}  // namespace

Json NextpnrProvider::paramsSchema() const {
    return {{"type", "object"},
            {"required", {"top_module", "netlist", "device"}},
            {"properties",
             {{"top_module", {{"type", "string"}}},
              {"netlist", {{"type", "string"}}},
              {"device", {{"type", "string"}}},
              {"family", {{"type", "string"}}},
              {"cst", {{"type", "string"}}},
              {"output", {{"type", "string"}}},
              {"nextpnr_path", {{"type", "string"}}}}}};
}

Json NextpnrProvider::resultSchema() const {
    return {{"type", "object"}, {"properties", {{"pnr_json", {{"type", "string"}}}}}};
}

Error NextpnrProvider::startJob(const JobRequest& request, JobContext& context) {
    try {
        const auto& params = request.params;
        const auto stringParam = [&](const char* name) -> std::string {
            if (!params.contains(name))
                return {};
            if (!params.at(name).is_string())
                throw std::invalid_argument(std::string(name) + " must be a string");
            const auto value = params.at(name).get<std::string>();
            if (value.find('\0') != std::string::npos)
                throw std::invalid_argument(std::string(name) + " contains NUL");
            return value;
        };
        const auto top = stringParam("top_module");
        const auto device = stringParam("device");
        const auto family = stringParam("family");
        const auto input = stringParam("netlist");
        const auto cst = stringParam("cst");
        const auto outputParam = stringParam("output");
        if (!IsIdentifier(top) || device.empty() || device[0] == '-' || input.empty())
            throw std::invalid_argument("pnr requires a valid top_module, netlist and device");
        const auto netlist = web::CheckedInput(context, input);
        fs::path constraints;
        if (!cst.empty())
            constraints = web::CheckedInput(context, cst);
        auto output = outputParam.empty() ? context.jobDir() / "artifacts" / (top + ".pnr.json")
                                          : fs::path(outputParam);
        output = web::CheckedPath(context.jobDir() / "artifacts", output, false);
        if (output == context.jobDir() / "artifacts")
            throw std::invalid_argument("output must name a file");

        ToolQuery query;
        query.name = "nextpnr-himbaechel";
        query.alternativeNames = {"nextpnr-gowin"};
        query.environmentVariables = {"SIGFLOW_NEXTPNR"};
        query.configuredPath = stringParam("nextpnr_path");
        const auto tool = web::ResolveExecutionTool(context, query);
        if (!tool.found)
            return {ErrorCode::NotFound, "nextpnr not found: " + tool.reason, ""};
        fs::create_directories(output.parent_path());
        ProcessSpec spec;
        spec.executable = tool.path;
        spec.workingDirectory = context.jobDir();
        spec.arguments = {"--json",   netlist.string(), "--write", output.string(),
                          "--device", device,           "--vopt",  "family=" + family};
        if (!constraints.empty()) {
            spec.arguments.push_back("--vopt");
            spec.arguments.push_back("cst=" + constraints.string());
        }
        const auto process = context.processHost().Run(
            spec, [&](const auto& chunk, bool error) { context.log(chunk, error); });
        context.emitMetric(Json{{"exit_code", process.exitCode}});
        context.emitMetric(
            Json{{"nextpnr", AnalyzeNextpnrLog(process.output, process.outputTruncated, device,
                                               family, process.exitCode)}});
        if (process.outcome == ProcessOutcome::Cancelled)
            return {ErrorCode::Cancelled, "place and route cancelled", ""};
        if (process.outcome == ProcessOutcome::TimedOut)
            return {ErrorCode::TimedOut, "place and route timed out", ""};
        if (!process.started)
            return {ErrorCode::Crashed, process.errorMessage, ""};
        if (process.exitCode != 0)
            return {ErrorCode::Internal,
                    "nextpnr exited with code " + std::to_string(process.exitCode), ""};
        output = web::CheckedPath(context.jobDir() / "artifacts", output, true);
        if (!fs::is_regular_file(output))
            throw std::runtime_error("nextpnr output is not a regular file");
        std::ifstream file(output, std::ios::binary);
        Json design;
        file >> design;
        if (!file || !design.is_object() || !design.contains("modules") ||
            !design.at("modules").is_object() || !design.at("modules").contains(top) ||
            !design.at("modules").at(top).is_object())
            throw std::runtime_error("nextpnr output missing top module");
        Artifact artifact;
        artifact.id = "pnr-json";
        artifact.path = output;
        artifact.schema = "eda.pnr.nextpnr-json.v1";
        artifact.role = "primary";
        context.registerArtifact(artifact);
        context.progress(100, "place and route complete");
        return Error::Ok();
    } catch (const std::invalid_argument& error) {
        return {ErrorCode::InvalidArgument, error.what(), ""};
    } catch (const std::exception& error) {
        return {ErrorCode::Internal, error.what(), ""};
    }
}
}  // namespace eda::pnr
