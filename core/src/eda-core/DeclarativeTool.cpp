#include "DeclarativeTool.h"

#include <eda/api/abi.h>
#include <eda/api/process.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace eda {
namespace {

std::string Substitute(const std::string& tmpl, const Json& params, std::string& error) {
    std::string out;
    std::size_t index = 0;
    while (index < tmpl.size()) {
        if (tmpl[index] == '$' && index + 1 < tmpl.size() && tmpl[index + 1] == '{') {
            const std::size_t end = tmpl.find('}', index + 2);
            if (end == std::string::npos) {
                error = "unterminated ${...} in declarative tool spec";
                return {};
            }
            const std::string key = tmpl.substr(index + 2, end - index - 2);
            if (!params.is_object() || !params.contains(key)) {
                error = "missing declarative tool parameter: " + key;
                return {};
            }
            const Json& value = params[key];
            if (value.is_string()) {
                out += value.get<std::string>();
            } else if (value.is_array()) {
                bool first = true;
                for (const auto& entry : value) {
                    if (!entry.is_string()) continue;
                    if (!first) out += ' ';
                    out += entry.get<std::string>();
                    first = false;
                }
            } else {
                out += value.dump();
            }
            index = end + 1;
        } else {
            out += tmpl[index++];
        }
    }
    return out;
}

std::string OptionalString(const Json& object, const char* key, const std::string& fallback = {}) {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return fallback;
}

} // namespace

DeclarativeJobProvider::DeclarativeJobProvider(DeclarativeToolSpec spec) : spec_(std::move(spec)) {}

bool DeclarativeJobProvider::ParseSpec(const Json& json, DeclarativeToolSpec& spec,
                                       std::string& error) {
    if (!json.is_object()) {
        error = "declarative tool spec must be a JSON object";
        return false;
    }
    DeclarativeToolSpec parsed;
    parsed.id = OptionalString(json, "id");
    parsed.jobType = OptionalString(json, "job_type");
    parsed.capability = OptionalString(json, "capability");
    parsed.executable = OptionalString(json, "executable");
    parsed.workingDirectory = OptionalString(json, "working_directory");
    parsed.requiresConfirm = json.value("requires_confirm", false);
    if (parsed.id.empty() || parsed.jobType.empty() || parsed.executable.empty()) {
        error = "declarative tool spec requires id, job_type and executable";
        return false;
    }

    if (json.contains("arguments") && json["arguments"].is_array()) {
        for (const auto& entry : json["arguments"]) {
            if (entry.is_string()) parsed.arguments.push_back(entry.get<std::string>());
        }
    }
    if (json.contains("requires_params") && json["requires_params"].is_array()) {
        for (const auto& entry : json["requires_params"]) {
            if (entry.is_string()) parsed.requiresParams.push_back(entry.get<std::string>());
        }
    }
    if (json.contains("artifacts") && json["artifacts"].is_array()) {
        for (const auto& entry : json["artifacts"]) {
            if (!entry.is_object()) continue;
            DeclarativeArtifactSpec artifact;
            artifact.id = OptionalString(entry, "id");
            artifact.path = OptionalString(entry, "path");
            artifact.schema = OptionalString(entry, "schema");
            artifact.role = OptionalString(entry, "role", "primary");
            if (!artifact.id.empty() && !artifact.path.empty()) {
                parsed.artifacts.push_back(std::move(artifact));
            }
        }
    }

    spec = std::move(parsed);
    return true;
}

std::string DeclarativeJobProvider::jobType() const { return spec_.jobType; }

Json DeclarativeJobProvider::paramsSchema() const {
    Json properties = Json::object();
    for (const auto& name : spec_.requiresParams) {
        properties[name] = Json{{"type", "string"}};
    }
    return Json{{"type", "object"}, {"required", spec_.requiresParams}, {"properties", properties}};
}

Json DeclarativeJobProvider::resultSchema() const {
    return Json{{"type", "object"}};
}

bool DeclarativeJobProvider::requiresConfirm() const { return spec_.requiresConfirm; }

Error DeclarativeJobProvider::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;
    for (const auto& name : spec_.requiresParams) {
        if (!params.is_object() || !params.contains(name)) {
            return Error{ErrorCode::InvalidArgument, "missing required parameter: " + name, ""};
        }
    }

    std::string error;
    const std::string executable = Substitute(spec_.executable, params, error);
    if (!error.empty()) return Error{ErrorCode::InvalidArgument, error, ""};

    std::vector<std::string> arguments;
    arguments.reserve(spec_.arguments.size());
    for (const auto& argument : spec_.arguments) {
        arguments.push_back(Substitute(argument, params, error));
        if (!error.empty()) return Error{ErrorCode::InvalidArgument, error, ""};
    }

    std::filesystem::path workingDirectory = spec_.workingDirectory.empty()
        ? ctx.jobDir()
        : std::filesystem::path(Substitute(spec_.workingDirectory, params, error));
    if (!error.empty()) return Error{ErrorCode::InvalidArgument, error, ""};
    std::error_code dirError;
    std::filesystem::create_directories(workingDirectory, dirError);

    ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = workingDirectory;
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "tool cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "tool timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }
    if (process.exitCode != 0) {
        return Error{ErrorCode::Internal,
                     "tool exited with code " + std::to_string(process.exitCode), ""};
    }

    for (const auto& artifactSpec : spec_.artifacts) {
        const std::string path = Substitute(artifactSpec.path, params, error);
        if (!error.empty()) return Error{ErrorCode::InvalidArgument, error, ""};
        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError)) {
            return Error{ErrorCode::Internal, "declared artifact missing: " + path, ""};
        }
        Artifact artifact;
        artifact.id = artifactSpec.id;
        artifact.path = path;
        artifact.schema = artifactSpec.schema;
        artifact.role = artifactSpec.role;
        ctx.registerArtifact(artifact);
    }

    ctx.progress(100, "tool complete");
    return Error::Ok();
}

void DeclarativeJobProvider::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    onReply(Error{ErrorCode::Unsupported, "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t DeclarativeJobProvider::subscribe(const std::string&, EventHandler<const Json&>) {
    return 0;
}

void DeclarativeJobProvider::unsubscribe(std::uint64_t) {}

PluginInfo DeclarativeJobProvider::info() const {
    PluginInfo info;
    info.id = spec_.id;
    info.version = "1.0.0";
    info.displayName = spec_.id;
    info.vendor = "declarative";
    info.location = "external";
    info.runtime = "inprocess";
    if (!spec_.capability.empty()) info.capabilities = {spec_.capability};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace eda
