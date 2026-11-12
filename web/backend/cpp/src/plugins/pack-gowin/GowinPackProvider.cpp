#include "GowinPackProvider.h"
#include "eda-core/JobPaths.h"
#include "eda-core/JobRuntime.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"
#include <eda/api/process.hpp>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;
namespace eda::pack {
Json GowinPackProvider::paramsSchema() const {
    return {{"type", "object"},
            {"required", {"pnr_json", "device"}},
            {"properties",
             {{"pnr_json", {{"type", "string"}}},
              {"device", {{"type", "string"}}},
              {"output", {{"type", "string"}}},
              {"gowin_pack_path", {{"type", "string"}}}}}};
}
Json GowinPackProvider::resultSchema() const {
    return {{"type", "object"}, {"properties", {{"bitstream", {{"type", "string"}}}}}};
}
Error GowinPackProvider::startJob(const JobRequest& request, JobContext& context) {
    try {
        const auto stringParam = [&](const char* name) -> std::string {
            if (!request.params.contains(name))
                return {};
            if (!request.params.at(name).is_string())
                throw std::invalid_argument(std::string(name) + " must be a string");
            auto value = request.params.at(name).get<std::string>();
            if (value.find('\0') != std::string::npos)
                throw std::invalid_argument(std::string(name) + " contains NUL");
            return value;
        };
        auto inputParam = stringParam("pnr_json");
        auto device = stringParam("device");
        auto outputParam = stringParam("output");
        if (!outputParam.empty() && fs::path(outputParam).is_absolute())
            throw std::invalid_argument("output must be relative to artifacts");
        if (inputParam.empty() || device.empty() || device[0] == '-')
            throw std::invalid_argument("pack requires pnr_json and a valid device");
        const auto input = web::CheckedInput(context, inputParam);
        if (!fs::is_regular_file(input) || fs::file_size(input) == 0)
            throw std::invalid_argument("PnR JSON artifact is empty or not a regular file");
        const auto artifactRoot = context.jobDir() / "artifacts";
        auto output = outputParam.empty() ? artifactRoot / "design.fs" : fs::path(outputParam);
        output = web::CheckedPath(artifactRoot, output, false);
        if (output == artifactRoot)
            throw std::invalid_argument("output must name a file");
        auto manifest = artifactRoot / (output.stem().string() + ".pack.manifest.json");
        manifest = web::CheckedPath(artifactRoot, manifest, false);
        if (output == manifest)
            throw std::invalid_argument("output conflicts with pack manifest");
        ToolQuery query;
        query.name = "gowin_pack";
        query.environmentVariables = {"SIGFLOW_GOWIN_PACK"};
        query.configuredPath = stringParam("gowin_pack_path");
        const auto tool = web::ResolveExecutionTool(context, query);
        if (!tool.found)
            return {ErrorCode::NotFound, "gowin_pack not found: " + tool.reason, ""};
        fs::create_directories(output.parent_path());
        ProcessSpec spec;
        spec.executable = tool.path;
        spec.workingDirectory = context.jobDir();
        spec.arguments = {"-d", device, "-o", output.string(), input.string()};
        const auto process = context.processHost().Run(
            spec, [&](const auto& chunk, bool error) { context.log(chunk, error); });
        context.emitMetric(Json{{"exit_code", process.exitCode}});
        if (process.outcome == ProcessOutcome::Cancelled)
            return {ErrorCode::Cancelled, "pack cancelled", ""};
        if (process.outcome == ProcessOutcome::TimedOut)
            return {ErrorCode::TimedOut, "pack timed out", ""};
        if (!process.started)
            return {ErrorCode::Crashed, process.errorMessage, ""};
        if (process.exitCode != 0)
            return {ErrorCode::Internal,
                    "gowin_pack exited with code " + std::to_string(process.exitCode), ""};
        output = web::CheckedPath(artifactRoot, output, true);
        if (!fs::is_regular_file(output) || fs::file_size(output) < 32)
            throw std::runtime_error("generated .fs artifact is too small or cannot be read");
        const auto inputHash = platform::Sha256FileHex(input);
        const auto toolHash = platform::Sha256FileHex(tool.path);
        const auto outputHash = platform::Sha256FileHex(output);
        if (inputHash.size() != 64 || toolHash.size() != 64 || outputHash.size() != 64)
            throw std::runtime_error("unable to calculate SHA-256 for pack input or output");
        Json manifestData = {{"schema_version", "1.0"},
                             {"tool", "gowin_pack"},
                             {"success", true},
                             {"exit_code", process.exitCode},
                             {"message", "Apicula .fs artifact is valid."},
                             {"completed_at", platform::UtcTimestamp()},
                             {"device", device},
                             {"input",
                              {{"pnr_json", input.string()},
                               {"size_bytes", fs::file_size(input)},
                               {"sha256", inputHash}}},
                             {"output",
                              {{"fs", output.string()},
                               {"size_bytes", fs::file_size(output)},
                               {"sha256", outputHash}}},
                             {"executable", {{"path", tool.path.string()}, {"sha256", toolHash}}}};
        web::AtomicJson(artifactRoot, manifest.lexically_relative(artifactRoot), manifestData);
        Artifact artifact;
        artifact.id = "bitstream";
        artifact.path = output;
        artifact.schema = "eda.bitstream.apicula.v1";
        artifact.sha256 = outputHash;
        artifact.role = "primary";
        context.registerArtifact(artifact);
        context.progress(100, "pack complete");
        return Error::Ok();
    } catch (const std::invalid_argument& error) {
        return {ErrorCode::InvalidArgument, error.what(), ""};
    } catch (const std::exception& error) {
        return {ErrorCode::Internal, error.what(), ""};
    }
}
}  // namespace eda::pack
