#include "LegacyJobBridge.h"

#include "ReportNormalizer.h"

#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace eda {
namespace agent {
namespace {

// 每次都用全新的 error_code：不存在的路径会把 ec 置为 ENOENT，
// 复用同一个 ec 会把「不存在」误当成「目录不可读」并提前中止遍历。
bool IsDirectoryNoThrow(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

bool IsRegularFileNoThrow(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool LoadJsonFile(const std::filesystem::path& path, Json& out) {    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    if (text.empty()) return false;
    try {
        out = Json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    return out.is_object();
}

// 旧报告状态串 → 公开 JobState。未知串一律不当作 Succeeded（失败关闭）。
JobState LegacyStateFromString(const std::string& state) {
    if (state == "Created") return JobState::Created;
    if (state == "Validating") return JobState::Validating;
    if (state == "Queued") return JobState::Queued;
    if (state == "Running") return JobState::Running;
    if (state == "ValidatingArtifact") return JobState::ValidatingArtifact;
    if (state == "Succeeded") return JobState::Succeeded;
    if (state == "Cancelled") return JobState::Cancelled;
    if (state == "TimedOut") return JobState::TimedOut;
    return JobState::Failed;
}

// 产物 ID：只暴露 opaque ID 与 hash；本机路径永不进入归一化结果。
std::string LegacyArtifactId(const Json& artifact) {
    const std::string sha = artifact.value("sha256", std::string());
    if (!sha.empty()) return "legacy-" + sha.substr(0, 16);
    const std::string path = artifact.value("path", std::string());
    const std::string kind = artifact.value("kind", std::string("artifact"));
    if (!path.empty()) {
        return "legacy-" + platform::Sha256Hex(path.data(), path.size()).substr(0, 16);
    }
    return "legacy-" + platform::Sha256Hex(kind.data(), kind.size()).substr(0, 16);
}

} // namespace

LegacyJobBridge::LegacyJobBridge(std::filesystem::path projectRoot)
    : projectRoot_(std::move(projectRoot)) {}

bool LegacyJobBridge::FindJobDirectory(const std::string& jobId, std::filesystem::path& directory,
                                       std::string& jobType) const {
    if (jobId.empty()) return false;
    if (jobId.find('/') != std::string::npos || jobId.find('\\') != std::string::npos ||
        jobId == "." || jobId == "..") {
        return false;
    }
    const std::filesystem::path jobsRoot = projectRoot_ / ".sigflow" / "jobs";
    if (!IsDirectoryNoThrow(jobsRoot)) return false;
    // jobId 来自 URL（UTF-8）：作为路径分量时必须解码，否则中文 id 找不到目录。
    const std::filesystem::path jobDirectory = platform::PathFromUtf8(jobId);
    std::error_code iterationError;
    for (const auto& typeEntry : std::filesystem::directory_iterator(jobsRoot, iterationError)) {
        if (!IsDirectoryNoThrow(typeEntry.path())) continue;
        const std::filesystem::path candidate = typeEntry.path() / jobDirectory;
        if (!IsDirectoryNoThrow(candidate)) continue;
        directory = candidate;
        jobType = platform::PathToUtf8(typeEntry.path().filename());
        return true;
    }
    return false;
}

bool LegacyJobBridge::List(std::vector<LegacyJobEntry>& out, std::string& error) const {
    out.clear();
    error.clear();
    const std::filesystem::path jobsRoot = projectRoot_ / ".sigflow" / "jobs";
    if (!IsDirectoryNoThrow(jobsRoot)) {
        // 没有旧历史不是错误：返回空列表，让调用方区分"空"与"不可读"。
        return true;
    }
    std::error_code iterationError;
    for (const auto& typeEntry : std::filesystem::directory_iterator(jobsRoot, iterationError)) {
        if (!IsDirectoryNoThrow(typeEntry.path())) continue;
        const std::string jobType = platform::PathToUtf8(typeEntry.path().filename());
        std::error_code jobIterationError;
        for (const auto& jobEntry :
             std::filesystem::directory_iterator(typeEntry.path(), jobIterationError)) {
            if (!IsDirectoryNoThrow(jobEntry.path())) continue;
            LegacyJobEntry entry;
            entry.jobId = platform::PathToUtf8(jobEntry.path().filename());
            entry.jobType = jobType;
            Json manifest;
            if (LoadJsonFile(jobEntry.path() / "manifest.json", manifest)) {
                entry.hasManifest = true;
                entry.jobType = manifest.value("job_type", entry.jobType);
                entry.state = manifest.value("state", std::string());
                entry.createdAt = manifest.value("created_at", std::string());
                entry.updatedAt = manifest.value("updated_at", std::string());
            } else {
                entry.problem = "manifest_unreadable";
            }
            const std::filesystem::path reportPath =
                jobEntry.path() / "reports" / "job-report.json";
            entry.hasReport = IsRegularFileNoThrow(reportPath);
            if (!entry.hasReport && entry.problem.empty()) entry.problem = "report_missing";
            if (entry.hasReport) {
                Json report;
                if (LoadJsonFile(reportPath, report)) {
                    if (entry.state.empty()) entry.state = report.value("state", std::string());
                    entry.jobType = report.value("job_type", entry.jobType);
                } else if (entry.problem.empty()) {
                    entry.problem = "report_unreadable";
                }
            }
            out.push_back(std::move(entry));
        }
    }
    std::sort(out.begin(), out.end(), [](const LegacyJobEntry& a, const LegacyJobEntry& b) {
        if (a.createdAt != b.createdAt) {
            if (a.createdAt.empty()) return false;
            if (b.createdAt.empty()) return true;
            return a.createdAt > b.createdAt;
        }
        return a.jobId > b.jobId;
    });
    return true;
}

bool LegacyJobBridge::ReadReport(const std::string& jobId, const std::string& projectId,
                                 Json& normalized, std::string& error) const {
    normalized = Json();
    error.clear();
    std::filesystem::path directory;
    std::string jobType;
    if (!FindJobDirectory(jobId, directory, jobType)) {
        error = "legacy_job_not_found";
        return false;
    }
    Json report;
    if (!LoadJsonFile(directory / "reports" / "job-report.json", report)) {
        error = "report_missing";
        return false;
    }

    JobReport parsed;
    parsed.schemaVersion = "main/jobs " + report.value("schema_version", std::string("1.0"));
    parsed.jobId = report.value("job_id", jobId);
    parsed.jobType = report.value("job_type", jobType);
    parsed.pluginId = report.value("plugin_id", std::string());
    parsed.state = LegacyStateFromString(report.value("state", std::string()));
    if (report.contains("exit_code") && report["exit_code"].is_number_integer()) {
        parsed.exitCode = report["exit_code"].get<int>();
    }
    // 旧 errors[] → 规范 Diagnostic[]（保留原始码与定位事实）。
    parsed.diagnostics = report.contains("errors") ? report["errors"] : Json::array();
    // 旧 diagnostics（若有）优先于 errors：新格式回写过的记录也走这条只读桥。
    if (report.contains("diagnostics") && !report["diagnostics"].is_null()) {
        parsed.diagnostics = report["diagnostics"];
    }
    if (report.contains("artifacts") && report["artifacts"].is_array()) {
        for (const auto& artifact : report["artifacts"]) {
            if (!artifact.is_object()) continue;
            Artifact mapped;
            mapped.id = LegacyArtifactId(artifact);
            mapped.schema = artifact.value("schema", std::string());
            mapped.sha256 = artifact.value("sha256", std::string());
            mapped.role = artifact.value("kind", std::string());
            // 注意：不复制 path。旧记录的 path 是本机绝对路径。
            parsed.artifacts.push_back(std::move(mapped));
        }
    }

    ReportNormalizeInput input;
    input.report = std::move(parsed);
    input.projectId = projectId;
    input.rawReportSchema = "main/jobs " + report.value("schema_version", std::string("1.0"));
    // 旧记录没有指纹/版本/归属 revision：一律标 legacy_unverified。
    // 归因只靠 schema 内既有字段（origin/completeness/raw_report_schema/reason），
    // 不往 additionalProperties:false 的 JobReportView 里塞私有字段。
    input.legacyUnverified = true;
    input.diagnosticsProvided = true;
    normalized = NormalizeJobReport(input);
    return true;
}

Json LegacyJobBridge::ToJson(const LegacyJobEntry& entry) {
    return Json{{"job_id", entry.jobId},
                {"job_type", entry.jobType},
                {"state", entry.state},
                {"origin", "legacy"},
                {"created_at", entry.createdAt.empty() ? Json(nullptr) : Json(entry.createdAt)},
                {"updated_at", entry.updatedAt.empty() ? Json(nullptr) : Json(entry.updatedAt)},
                {"has_manifest", entry.hasManifest},
                {"has_report", entry.hasReport},
                {"completeness", "legacy_unverified"},
                {"problem", entry.problem.empty() ? Json(nullptr) : Json(entry.problem)}};
}

} // namespace agent
} // namespace eda
