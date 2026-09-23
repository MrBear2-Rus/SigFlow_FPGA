#include "CoreSchemas.h"

namespace eda {

namespace {

Json JobSchema() {
    return Json{
        {"schema_version", "eda.job.v1"},
        {"type", "object"},
        {"required", {"job_id", "job_type", "state"}},
        {"properties", {
            {"job_id", {{"type", "string"}}},
            {"job_type", {{"type", "string"}}},
            {"plugin_id", {{"type", "string"}}},
            {"state", {{"type", "string"}}},
            {"exit_code", {{"type", "number"}}},
            {"transitions", {{"type", "array"}}},
        }},
    };
}

Json JobReportSchema() {
    return Json{
        {"schema_version", "eda.jobreport.v1"},
        {"type", "object"},
        {"required", {"job_id", "job_type", "state"}},
        {"properties", {
            {"job_id", {{"type", "string"}}},
            {"job_type", {{"type", "string"}}},
            {"plugin_id", {{"type", "string"}}},
            {"state", {{"type", "string"}}},
            {"exit_code", {{"type", "number"}}},
            {"artifacts", {{"type", "array"}}},
            {"metrics", {{"type", "object"}}},
        }},
    };
}

Json ArtifactSchema() {
    return Json{
        {"schema_version", "eda.artifact.v1"},
        {"type", "object"},
        {"required", {"id", "path"}},
        {"properties", {
            {"id", {{"type", "string"}}},
            {"path", {{"type", "string"}}},
            {"schema", {{"type", "string"}}},
            {"sha256", {{"type", "string"}}},
            {"role", {{"type", "string"}}},
        }},
    };
}

} // namespace

void RegisterCoreSchemas(ISchemaRegistry& registry) {
    registry.RegisterSchema("eda.job.v1", JobSchema());
    registry.RegisterSchema("eda.jobreport.v1", JobReportSchema());
    registry.RegisterSchema("eda.artifact.v1", ArtifactSchema());
}

} // namespace eda
