#include "ReportNormalizer.h"

#include "DiagnosticParser.h"
#include "PathRedaction.h"

#include <string>

namespace eda {
namespace agent {
namespace {

// 将单条诊断规范化为 Diagnostic（spec §5.4）。
// 支持 core 现有诊断（对象/数组）与旧 errors[]（含 code/severity/stage/summary/log_line）。
Json NormalizeDiagnostic(const Json& raw, std::size_t index) {
    Json diagnostic = Json::object();
    if (raw.is_object()) {
        const std::string code = raw.value("code", std::string());
        const std::string severity = raw.value("severity", std::string("error"));
        const std::string stage = raw.value("stage", std::string());
        const std::string summary = raw.value("summary", raw.value("message", std::string()));
        diagnostic["id"] = raw.value("id", "diag-" + std::to_string(index + 1));
        // 未知原始码：保留 raw_code，并把 code 标为 UNCLASSIFIED——
        // C++ 侧不得替教学层假定根因（spec §5：不制造教学根因）。
        diagnostic["code"] = code.empty() ? "UNCLASSIFIED" : code;
        diagnostic["raw_code"] = code.empty() ? Json(nullptr) : Json(code);
        diagnostic["severity"] = (severity == "info" || severity == "warning" ||
                                  severity == "error")
                                     ? severity
                                     : "error";
        diagnostic["stage"] = stage;
        // 工具自由文本里由工具自己打印的本机路径同样要裁剪：Agent 不得看到本机路径。
        std::string summaryText = summary;
        if (RedactLocalPathsInText(summaryText) > 0) {
            diagnostic["summary_paths_redacted"] = true;
        }
        diagnostic["summary"] = summaryText;
        diagnostic["origin"] = raw.value("origin", std::string("core"));
        diagnostic["confidence_kind"] = raw.value("confidence_kind", std::string("tool"));

        // location：保留旧 reports 能给出的全部定位事实（IR 坐标与日志行），不丢字段。
        Json location = Json::object();
        bool hasLocation = false;
        if (raw.contains("ir_coordinate") && raw["ir_coordinate"].is_string() &&
            !raw["ir_coordinate"].get<std::string>().empty()) {
            location["ir_coordinate"] = raw["ir_coordinate"];
            hasLocation = true;
        }
        if (raw.contains("log_line") && raw["log_line"].is_number_integer()) {
            location["log_line"] = raw["log_line"];
            hasLocation = true;
        }
        if (raw.contains("file") && raw["file"].is_string() &&
            !raw["file"].get<std::string>().empty()) {
            location["file"] = raw["file"];
            hasLocation = true;
        }
        if (raw.contains("line") && raw["line"].is_number_integer()) {
            location["line"] = raw["line"];
            hasLocation = true;
        }
        if (raw.contains("column") && raw["column"].is_number_integer()) {
            location["column"] = raw["column"];
            hasLocation = true;
        }
        // 旧 schema 兼容：location 已是对象时原样并入（不做二次解释）。
        if (raw.contains("location") && raw["location"].is_object()) {
            for (auto it = raw["location"].begin(); it != raw["location"].end(); ++it) {
                location[it.key()] = it.value();
                hasLocation = true;
            }
        }
        // location 的结构化路径同样不得把本机绝对路径交给 Agent。
        if (hasLocation && location.contains("file") && location["file"].is_string() &&
            LooksLikeAbsolutePath(location["file"].get<std::string>())) {
            location["file"] = BasenameOfPath(location["file"].get<std::string>());
            location["file_redacted"] = true;
        }
        diagnostic["location"] = hasLocation ? location : Json(nullptr);
        diagnostic["evidence_refs"] = Json::array();
    } else if (raw.is_string()) {
        diagnostic["id"] = "diag-" + std::to_string(index + 1);
        diagnostic["code"] = "UNCLASSIFIED";
        diagnostic["raw_code"] = raw;
        diagnostic["severity"] = "error";
        diagnostic["stage"] = "";
        std::string summaryText = raw.get<std::string>();
        if (RedactLocalPathsInText(summaryText) > 0) {
            diagnostic["summary_paths_redacted"] = true;
        }
        diagnostic["summary"] = summaryText;
        diagnostic["origin"] = "legacy";
        diagnostic["confidence_kind"] = "tool";
        diagnostic["location"] = nullptr;
        diagnostic["evidence_refs"] = Json::array();
    }
    return diagnostic;
}

// 把任意 diagnostics 载荷（对象/数组/null）展开为 Diagnostic[]。
// 返回 false 表示"未提供"（调用方应标 unavailable）。
bool ExpandDiagnostics(const Json& diagnostics, Json& out, bool provided) {
    out = Json::array();
    if (!provided) return false;
    if (diagnostics.is_array()) {
        std::size_t index = 0;
        for (const auto& item : diagnostics) {
            const Json normalized = NormalizeDiagnostic(item, index);
            if (normalized.is_object()) out.push_back(normalized);
            ++index;
        }
        return true;
    }
    if (diagnostics.is_object()) {
        // core 现有 diagnostics 常为 {error: "...", timeout: true} 形态：
        // 若值为多行工具日志（yosys/verilator），先按行升格为真实诊断（origin=tool，
        // 保留 file/line/col 与原始码）；解析不出的行保留为框架级条目。
        std::size_t index = 0;
        for (auto it = diagnostics.begin(); it != diagnostics.end(); ++it) {
            if (it.value().is_string()) {
                const std::string text = it.value().get<std::string>();
                const auto parsed = DiagnosticParser::Parse(text);
                std::size_t parsedCount = 0;
                for (const auto& item : parsed) {
                    const Json expanded = ParsedDiagnosticsToJson({item}, "synth");
                    if (expanded.is_array() && !expanded.empty()) {
                        Json entry = expanded[0];
                        entry["id"] = "diag-" + std::to_string(index + 1);
                        // 契约 origin 枚举为 core|legacy：解析事实由 core 承载的日志产生，
                        // raw_code 保留工具原始码（verilator -CODE / unclassified）。
                        entry["origin"] = "core";
                        entry["raw_code"] = entry.contains("code") &&
                                                    entry["code"] != "unclassified"
                                                ? Json(entry["code"])
                                                : Json(it.key());
                        entry["code"] = entry["raw_code"].is_string()
                                            ? entry["raw_code"]
                                            : Json("UNCLASSIFIED");
                        out.push_back(std::move(entry));
                        ++parsedCount;
                    }
                }
                if (parsedCount > 0) continue;
            }
            Json item = Json::object();
            item["code"] = it.key();
            item["raw_code"] = it.key();
            item["severity"] = "error";
            item["stage"] = "";
            item["summary"] = it.value().is_string() ? it.value().get<std::string>()
                                                     : it.value().dump();
            item["origin"] = "core";
            item["confidence_kind"] = "tool";
            const Json normalized = NormalizeDiagnostic(item, index);
            if (normalized.is_object()) out.push_back(normalized);
            ++index;
        }
        return true;
    }
    // null：视为"未提供"。
    return false;
}

// metrics：确保每项带 value/unit/source/availability；缺失用 null+reason。
// 同时把结构化字段里的本机绝对路径替换为 basename：Agent 不得看到本机路径
// （spec 明确禁止）。替换发生时显式标记 path_redacted，不静默改写事实。
Json NormalizeMetrics(const Json& metrics) {
    Json out = Json::object();
    if (!metrics.is_object()) return out;
    for (auto it = metrics.begin(); it != metrics.end(); ++it) {
        const Json& value = it.value();
        if (value.is_object() && value.contains("value")) {
            Json metric = value;
            if (!metric.contains("unit")) metric["unit"] = nullptr;
            if (!metric.contains("source")) metric["source"] = nullptr;
            if (!metric.contains("availability")) {
                metric["availability"] = value["value"].is_null() ? "unavailable" : "available";
            }
            if (value["value"].is_null() && !metric.contains("reason")) {
                metric["reason"] = "not reported by tool";
            }
            if (RedactAbsolutePathsInJson(metric["value"])) {
                metric["path_redacted"] = true;
                metric["path_redacted_reason"] =
                    "absolute local path withheld from the agent; only the file name is reported";
            }
            out[it.key()] = metric;
        } else {
            // 裸值：包装为 metric；null → unavailable + reason（不填 0）。
            Json metric = Json{{"value", value},
                               {"unit", nullptr},
                               {"source", nullptr},
                               {"availability", value.is_null() ? "unavailable" : "available"}};
            if (value.is_null()) metric["reason"] = "not reported by tool";
            if (RedactAbsolutePathsInJson(metric["value"])) {
                metric["path_redacted"] = true;
                metric["path_redacted_reason"] =
                    "absolute local path withheld from the agent; only the file name is reported";
            }
            out[it.key()] = metric;
        }
    }
    return out;
}

} // namespace

const char* JobStateName(JobState state) {
    switch (state) {
        // CoreJobService has a more detailed internal state machine than the
        // public edu.jobreport.v1 contract.  Never leak internal-only strings:
        // the contract deliberately has a stable 9-state vocabulary.
        case JobState::Created:
        case JobState::Validating:
        case JobState::Queued:             return "Queued";
        case JobState::Running:
        case JobState::ValidatingArtifact: return "Running";
        case JobState::Succeeded:          return "Succeeded";
        case JobState::Failed:             return "Failed";
        case JobState::Cancelled:          return "Cancelled";
        case JobState::TimedOut:           return "TimedOut";
    }
    return "Unknown";
}

Json NormalizeJobReport(const ReportNormalizeInput& input) {
    const JobReport& report = input.report;
    Json data;
    data["schema_version"] = "edu.jobreport.v1";
    data["job_id"] = report.jobId;
    data["origin"] = input.rawReportSchema.empty() ? "core" : "legacy";
    data["capability"] = input.capability.empty() ? report.jobType : input.capability;
    data["plugin_id"] = report.pluginId.empty() ? Json(nullptr) : Json(report.pluginId);
    data["plugin_version"] =
        input.pluginVersion.empty() ? Json(nullptr) : Json(input.pluginVersion);
    data["project_id"] = input.projectId;
    data["revision"] = input.revision.empty() ? Json(nullptr) : Json(input.revision);
    data["snapshot_id"] = input.snapshotId.empty() ? Json(nullptr) : Json(input.snapshotId);
    data["state"] = JobStateName(report.state);
    data["exit_code"] = report.exitCode;

    Json diagnostics = Json::array();
    const bool diagnosticsPresent = ExpandDiagnostics(report.diagnostics, diagnostics,
                                                      input.diagnosticsProvided);
    data["diagnostics"] = diagnostics;

    data["metrics"] = NormalizeMetrics(report.metrics);

    Json artifacts = Json::array();
    for (const auto& artifact : report.artifacts) {
        // Paths are a host-internal implementation detail.  The agent gets an
        // opaque artifact_id and must use the bounded /artifacts or /waves
        // routes; returning `path` here would let an LLM discover local paths.
        artifacts.push_back(Json{{"artifact_id", artifact.id},
                                 {"schema", artifact.schema.empty() ? Json(nullptr)
                                                                    : Json(artifact.schema)},
                                 {"sha256", artifact.sha256.empty() ? Json(nullptr)
                                                                    : Json(artifact.sha256)},
                                 {"role", artifact.role.empty() ? Json(nullptr)
                                                                : Json(artifact.role)}});
    }
    data["artifacts"] = artifacts;

    // completeness：老产物 / 诊断未提供 / 正常。
    std::string completeness = "complete";
    if (input.legacyUnverified) {
        completeness = "legacy_unverified";
    } else if (!diagnosticsPresent) {
        completeness = "unavailable";
    }
    data["completeness"] = completeness;
    data["input_fingerprint"] =
        input.inputFingerprint.empty() ? Json(nullptr) : Json(input.inputFingerprint);
    data["raw_report_schema"] =
        input.rawReportSchema.empty() ? Json(nullptr) : Json(input.rawReportSchema);
    if (input.legacyUnverified) {
        data["reason"] = "legacy job has no input fingerprint or revision";
    } else if (!diagnosticsPresent) {
        data["reason"] = "diagnostics were not provided by the tool";
    }
    return data;
}

} // namespace agent
} // namespace eda
