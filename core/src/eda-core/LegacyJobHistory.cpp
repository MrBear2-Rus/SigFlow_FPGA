#include "JobsMigration.h"
#include "Platform.h"
#include "LegacyJobHistory.h"

#include <algorithm>
#include <fstream>

namespace eda {
namespace {

bool ReadFileJson(const std::filesystem::path& path, Json& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "cannot open: " + platform::PathToUtf8(path);
        return false;
    }
    try {
        file >> out;
    } catch (const std::exception& exception) {
        error = "invalid JSON (" + platform::PathToUtf8(path) + "): " + exception.what();
        return false;
    }
    return true;
}

// 旧报告错误条目 → edu.jobreport.v1 诊断条目形状（与 ReportNormalizer 同字段名口径）。
Json LegacyErrorToDiagnostic(const Json& item) {
    Json diag = Json::object();
    diag["raw_code"] = item.value("code", std::string());
    diag["severity"] = item.value("severity", std::string("error"));
    diag["summary"] = item.value("summary", std::string());
    const std::string stage = item.value("stage", std::string());
    if (!stage.empty()) diag["stage"] = stage;
    const std::string coordinate = item.value("ir_coordinate", std::string());
    if (!coordinate.empty()) diag["location"] = coordinate;
    if (item.contains("log_line") && item["log_line"].is_number_integer()) {
        diag["log_line"] = item["log_line"];
    }
    diag["confidence_kind"] = "tool";
    diag["origin"] = "legacy";
    return diag;
}

} // namespace

bool LoadLegacyJobHistory(const std::filesystem::path& projectPath,
                          std::vector<LegacyJobHistoryEntry>& out,
                          std::vector<std::string>& unreadableJobIds,
                          std::string& error) {
    out.clear();
    unreadableJobIds.clear();
    error.clear();

    const std::filesystem::path jobsRoot = projectPath / ".sigflow" / "jobs";
    std::error_code probeError;
    const std::filesystem::file_status status = std::filesystem::status(jobsRoot, probeError);
    if (std::filesystem::status_known(status) && !std::filesystem::exists(status)) {
        // 从未跑过旧 Job：空白历史不是错误。
        return true;
    }
    if (!std::filesystem::is_directory(jobsRoot, probeError)) {
        error = "cannot inspect legacy jobs root: " + probeError.message();
        return false;
    }

    const std::vector<std::string> legacyTypeDirectories = {
        "simulation", "synthesis", "pnr", "pack", "flash",
    };

    for (const std::string& typeName : legacyTypeDirectories) {
        const std::filesystem::path typeRoot = jobsRoot / typeName;
        std::error_code typeError;
        if (!std::filesystem::is_directory(typeRoot, typeError)) continue;

        // 契约 jobType（如 simulation→sim.build）；目录名是旧层序列化名，与其映射同表。
        const auto contracted = jobs_migration::LegacyJobTypeToJobType(typeName);
        if (!contracted.has_value()) {
            error = "unknown legacy type directory: " + typeName;
            return false;
        }

        std::error_code iterateError;
        for (const auto& entry : std::filesystem::directory_iterator(typeRoot, iterateError)) {
            if (!entry.is_directory()) continue;

            const std::string jobId = platform::PathToUtf8(entry.path().filename());
            const std::filesystem::path manifest = entry.path() / "manifest.json";
            Json manifestJson;
            std::string manifestError;
            if (!ReadFileJson(manifest, manifestJson, manifestError) ||
                !manifestJson.is_object()) {
                unreadableJobIds.push_back(jobId);
                continue;
            }
            LegacyJobHistoryEntry item;
            try {
                const auto state = jobs_migration::LegacyJobStateFromName(
                    manifestJson.value("state", std::string()));
                const std::string manifestJobId = manifestJson.value("job_id", std::string());
                const std::string manifestType = manifestJson.value("job_type", std::string());
                if (!state.has_value() || manifestJobId.empty() || manifestJobId != jobId ||
                    manifestType != typeName ||
                    (manifestJson.contains("parameters") &&
                     !manifestJson["parameters"].is_object()) ||
                    (manifestJson.contains("transitions") &&
                     !manifestJson["transitions"].is_array())) {
                    unreadableJobIds.push_back(jobId);
                    continue;
                }

                item.record.id = manifestJobId;
                item.record.retryOf = manifestJson.value("retry_of", std::string());
                item.record.createdAt = manifestJson.value("created_at", std::string());
                item.record.updatedAt = manifestJson.value("updated_at", std::string());
                item.record.exitCode = manifestJson.value("exit_code", 0);
                item.record.state = *state;
                item.record.request.jobType = *contracted;
                if (manifestJson.contains("parameters") &&
                    manifestJson["parameters"].is_object()) {
                    item.record.request.params = manifestJson["parameters"];
                }
                if (manifestJson.contains("transitions") &&
                    manifestJson["transitions"].is_array()) {
                    bool invalidTransition = false;
                    for (const auto& raw : manifestJson["transitions"]) {
                        if (!raw.is_object()) {
                            invalidTransition = true;
                            break;
                        }
                        const auto transitionState = jobs_migration::LegacyJobStateFromName(
                            raw.value("state", std::string()));
                        if (!transitionState.has_value()) {
                            invalidTransition = true;
                            break;
                        }
                        JobTransition transition;
                        transition.state = *transitionState;
                        transition.timestamp = raw.value("timestamp", std::string());
                        transition.op = "legacy";
                        transition.reason = raw.value("reason", std::string());
                        transition.exitCode = raw.value("exit_code", 0);
                        item.record.transitions.push_back(transition);
                    }
                    if (invalidTransition) {
                        unreadableJobIds.push_back(jobId);
                        continue;
                    }
                }
            } catch (const Json::exception&) {
                // A syntactically valid JSON file can still have fields of the wrong type.
                // Report this record and continue scanning the remaining jobs.
                unreadableJobIds.push_back(jobId);
                continue;
            }

            // 报告可选；存在则归一化到 edu.jobreport.v1 形态。
            const std::filesystem::path reportPath = entry.path() / "reports" / "job-report.json";
            Json reportJson;
            std::error_code reportProbeError;
            const bool hasReport = std::filesystem::exists(reportPath, reportProbeError);
            if (reportProbeError) {
                unreadableJobIds.push_back(jobId);
            } else if (hasReport &&
                       (!ReadFileJson(reportPath, reportJson, manifestError) ||
                        !reportJson.is_object())) {
                unreadableJobIds.push_back(jobId);
            } else if (hasReport) {
                try {
                    const auto reportState = jobs_migration::LegacyJobStateFromName(
                        reportJson.value("state", std::string()));
                    const std::string reportJobId = reportJson.value("job_id", std::string());
                    const std::string reportType = reportJson.value("job_type", std::string());
                    if (reportState.has_value() && reportJobId == item.record.id &&
                        reportType == typeName) {
                        item.report.schemaVersion = "edu.jobreport.v1";
                        item.report.jobId = item.record.id;
                        item.report.jobType = *contracted;
                        item.report.state = *reportState;
                        item.report.exitCode = reportJson.value("exit_code", item.record.exitCode);
                        for (const auto& raw : reportJson["artifacts"]) {
                            if (!raw.is_object()) continue;
                            Artifact artifact;
                            artifact.id = "artifact-" + std::to_string(item.report.artifacts.size());
                            artifact.path = platform::PathFromUtf8(raw.value("path", std::string()));
                            artifact.role = raw.value("kind", std::string());
                            item.report.artifacts.push_back(artifact);
                        }
                        Json diagnostics = Json::array();
                        for (const auto& raw : reportJson["errors"]) {
                            if (raw.is_object()) diagnostics.push_back(LegacyErrorToDiagnostic(raw));
                        }
                        const std::string summary = reportJson.value("summary", std::string());
                        if (!summary.empty()) {
                            Json summaryDiag = Json::object();
                            summaryDiag["severity"] =
                                item.report.exitCode == 0 ? std::string("information") : std::string("error");
                            summaryDiag["summary"] = summary;
                            summaryDiag["confidence_kind"] = "tool";
                            summaryDiag["origin"] = "legacy";
                            diagnostics.push_back(summaryDiag);
                        }
                        item.report.diagnostics = diagnostics;
                        Json metrics = Json::object();
                        if (reportJson.contains("started_at"))
                            metrics["started_at"] = reportJson["started_at"];
                        if (reportJson.contains("completed_at"))
                            metrics["completed_at"] = reportJson["completed_at"];
                        if (reportJson.contains("duration_ms"))
                            metrics["duration_ms"] = reportJson["duration_ms"];
                        item.report.metrics = metrics;
                    } else {
                        unreadableJobIds.push_back(jobId);
                    }
                } catch (const Json::exception&) {
                    // Keep the valid manifest visible even when its optional report is corrupt.
                    item.report = JobReport{};
                    unreadableJobIds.push_back(jobId);
                }
            }

            out.push_back(std::move(item));
        }
        if (iterateError) {
            error = "cannot scan legacy jobs: " + iterateError.message();
            return false;
        }
    }

    std::sort(out.begin(), out.end(), [](const LegacyJobHistoryEntry& left,
                                         const LegacyJobHistoryEntry& right) {
        if (left.record.createdAt != right.record.createdAt) {
            return left.record.createdAt < right.record.createdAt;
        }
        return left.record.id < right.record.id;
    });
    return true;
}

bool LoadLegacyFpgaJobHistory(const std::filesystem::path& projectPath,
                              std::vector<LegacyJobHistoryEntry>& out,
                              std::vector<std::string>& unreadableJobIds,
                              std::string& error) {
    out.clear();
    unreadableJobIds.clear();
    error.clear();

    const std::filesystem::path fpgaRoot = projectPath / ".sigflow" / "fpga";
    const struct {
        const char* directory;
        const char* jobType;
        const char* reportName;
    } layouts[] = {
        {"runs", "synth", "synthesis.analysis.json"},
        {"runs-nextpnr", "pnr", "route.analysis.json"},
    };
    for (const auto& layout : layouts) {
        const std::filesystem::path root = fpgaRoot / layout.directory;
        std::error_code iterateError;
        if (!std::filesystem::is_directory(root, iterateError)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, iterateError)) {
            if (!entry.is_directory()) continue;
            const std::string id = platform::PathToUtf8(entry.path().filename());
            Json manifest;
            std::string readError;
            if (!ReadFileJson(entry.path() / "manifest.json", manifest, readError) ||
                !manifest.is_object()) {
                unreadableJobIds.push_back(id);
                continue;
            }
            LegacyJobHistoryEntry item;
            try {
                const auto state = jobs_migration::LegacyJobStateFromName(
                    manifest.value("state", std::string()));
                if (!state || manifest.value("job_id", std::string()) != id ||
                    !manifest.contains("request") || !manifest["request"].is_object()) {
                    unreadableJobIds.push_back(id);
                    continue;
                }
                item.record.id = id;
                item.record.state = *state;
                item.record.request.jobType = layout.jobType;
                item.record.request.projectId = platform::PathToUtf8(projectPath);
                item.record.request.params = manifest["request"];
                item.record.retryOf = manifest.value("retry_of", std::string());
                item.record.createdAt = manifest.value("created_at", std::string());
                item.record.updatedAt = manifest.value("updated_at", std::string());
                item.record.exitCode = manifest.value("exit_code", 0);
                item.report.schemaVersion = "edu.jobreport.v1";
                item.report.jobId = id;
                item.report.jobType = layout.jobType;
                item.report.state = *state;
                item.report.exitCode = item.record.exitCode;
                item.report.metrics = Json::object();
                const std::filesystem::path reportPath =
                    entry.path() / "reports" / layout.reportName;
                item.report.metrics["legacy_report_path"] = platform::PathToUtf8(reportPath);
                item.report.metrics["legacy_log_path"] = platform::PathToUtf8(
                    entry.path() / "logs" /
                    (std::string(layout.jobType) == "synth" ? "yosys.combined.log"
                                                         : "nextpnr.combined.log"));
                std::error_code reportError;
                if (std::filesystem::exists(reportPath, reportError)) {
                    Json report;
                    if (ReadFileJson(reportPath, report, readError) && report.is_object()) {
                        item.report.diagnostics = std::move(report);
                    } else {
                        unreadableJobIds.push_back(id);
                    }
                } else if (reportError) {
                    unreadableJobIds.push_back(id);
                }
            } catch (const Json::exception&) {
                unreadableJobIds.push_back(id);
                continue;
            }
            out.push_back(std::move(item));
        }
        if (iterateError) {
            error = "cannot scan legacy FPGA jobs: " + iterateError.message();
            return false;
        }
    }
    std::sort(out.begin(), out.end(), [](const LegacyJobHistoryEntry& left,
                                         const LegacyJobHistoryEntry& right) {
        if (left.record.createdAt != right.record.createdAt)
            return left.record.createdAt < right.record.createdAt;
        return left.record.id < right.record.id;
    });
    return true;
}

} // namespace eda
