#include "FpgaYosysReport.h"

#include <json/json.h>

#include <wx/file.h>
#include <wx/filefn.h>

#include <memory>

namespace {

Json::Value ToJson(const YosysLogRecord& record)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["job_id"] = record.jobId.ToStdString();
    root["status"] = record.status.ToStdString();
    root["tool_version"] = record.toolVersion.ToStdString();
    root["strategy"] = record.strategy.ToStdString();
    root["duration_ms"] = record.durationMs;
    root["warning_count"] = record.warningCount;
    root["error_count"] = record.errorCount;
    root["failed_stage"] = record.failedStage.ToStdString();
    root["root_cause"] = record.rootCause.ToStdString();
    root["root_suggestion"] = record.rootSuggestion.ToStdString();
    Json::Value resources(Json::objectValue);
    resources["total_cells"] = record.resources.totalCells;
    resources["lut"] = record.resources.lutCount;
    resources["dff"] = record.resources.dffCount;
    resources["ibuf"] = record.resources.ibufCount;
    resources["obuf"] = record.resources.obufCount;
    Json::Value cellTypes(Json::arrayValue);
    for (const YosysResourceItem& item : record.resources.cellTypes) {
        Json::Value cell(Json::objectValue);
        cell["type"] = item.type.ToStdString();
        cell["count"] = item.count;
        cellTypes.append(cell);
    }
    resources["cell_types"] = cellTypes;
    root["resources"] = resources;
    Json::Value stages(Json::arrayValue);
    for (const YosysLogStage& stage : record.stages) {
        Json::Value item(Json::objectValue);
        item["name"] = stage.name.ToStdString();
        item["status"] = stage.status.ToStdString();
        item["first_line"] = stage.firstLine;
        item["last_line"] = stage.lastLine;
        stages.append(item);
    }
    root["stages"] = stages;
    Json::Value events(Json::arrayValue);
    for (const YosysLogEvent& event : record.events) {
        Json::Value item(Json::objectValue);
        item["sequence"] = event.sequence;
        item["line"] = event.lineNumber;
        item["severity"] = ToString(event.severity).ToStdString();
        item["stage"] = event.stage.ToStdString();
        item["rule_id"] = event.ruleId.ToStdString();
        item["raw"] = event.rawLine.ToStdString();
        item["source_file"] = event.sourceFile.ToStdString();
        item["source_line"] = event.sourceLine;
        item["source_column"] = event.sourceColumn;
        item["evidence"] = event.evidence.ToStdString();
        item["suggestion"] = event.suggestion.ToStdString();
        item["retryable"] = event.retryable;
        events.append(item);
    }
    root["events"] = events;
    Json::Value artifacts(Json::objectValue);
    artifacts["combined_log"] = record.combinedLogPath.ToStdString();
    artifacts["stdout_log"] = record.stdoutLogPath.ToStdString();
    artifacts["stderr_log"] = record.stderrLogPath.ToStdString();
    artifacts["json"] = record.artifactPath.ToStdString();
    root["artifacts"] = artifacts;
    Json::Value provenance(Json::objectValue);
    provenance["job_manifest"] = record.jobManifestPath.ToStdString();
    provenance["input_manifest"] = record.inputManifestPath.ToStdString();
    provenance["runtime_manifest"] = record.runtimeManifestPath.ToStdString();
    provenance["script"] = record.scriptPath.ToStdString();
    provenance["artifact_manifest"] = record.artifactManifestPath.ToStdString();
    Json::Value inputHashes(Json::arrayValue);
    if (!record.inputManifestPath.IsEmpty()) {
        wxFile inputManifest(record.inputManifestPath, wxFile::read);
        wxString manifestText;
        if (inputManifest.IsOpened() && inputManifest.ReadAll(&manifestText)) {
            const wxScopedCharBuffer utf8 = manifestText.ToUTF8();
            Json::Value manifest;
            Json::CharReaderBuilder readerBuilder;
            std::string errors;
            std::unique_ptr<Json::CharReader> reader(readerBuilder.newCharReader());
            if (utf8.data() && reader->parse(utf8.data(), utf8.data() + utf8.length(),
                                             &manifest, &errors) && manifest["files"].isArray()) {
                for (const Json::Value& source : manifest["files"]) {
                    Json::Value item(Json::objectValue);
                    item["name"] = source["name"];
                    item["path"] = source["path"];
                    item["sha256"] = source["sha256"];
                    inputHashes.append(item);
                }
            }
        }
    }
    provenance["input_hashes"] = inputHashes;
    root["provenance"] = provenance;
    return root;
}

bool WriteUtf8(const wxString& path, const wxString& content, wxString& errorMessage)
{
    wxFile file(path, wxFile::write);
    if (!file.IsOpened()) {
        errorMessage = "Unable to write Yosys report: " + path;
        return false;
    }
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    const size_t length = utf8.data() ? utf8.length() : 0;
    if (length > 0 && file.Write(utf8.data(), length) != static_cast<wxFileOffset>(length)) {
        errorMessage = "Unable to write Yosys report: " + path;
        file.Close();
        return false;
    }
    file.Close();
    return true;
}

} // namespace

wxString FpgaYosysReport::GenerateJson(const YosysLogRecord& record)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return wxString::FromUTF8(Json::writeString(writer, ToJson(record))) + "\n";
}

wxString FpgaYosysReport::GenerateSummary(const YosysLogRecord& record)
{
    wxString summary;
    summary << "# Yosys 综合摘要\n\n";
    summary << "- 状态: " << (record.status.IsEmpty() ? wxString("Unknown") : record.status) << "\n";
    summary << "- Job: " << record.jobId << "\n";
    summary << "- 耗时: " << record.durationMs << " ms\n";
    summary << "- 警告/错误: " << record.warningCount << "/" << record.errorCount << "\n";
    if (record.resources.totalCells >= 0) {
        summary << "- 资源: Cells " << record.resources.totalCells
                << ", LUT " << record.resources.lutCount
                << ", DFF " << record.resources.dffCount
                << ", IBUF " << record.resources.ibufCount
                << ", OBUF " << record.resources.obufCount << "\n";
    }
    if (!record.failedStage.IsEmpty()) summary << "- 失败阶段: " << record.failedStage << "\n";
    if (!record.rootCause.IsEmpty()) {
        summary << "- 首个根因: " << record.rootCause << "\n";
        summary << "- 建议: " << record.rootSuggestion << "\n";
    }
    summary << "\n## 阶段\n\n";
    for (const YosysLogStage& stage : record.stages) {
        summary << "- " << stage.name << ": " << stage.status
                << " (日志 " << stage.firstLine << "-" << stage.lastLine << ")\n";
    }
    summary << "\n## 可追溯性\n\n";
    if (!record.inputManifestPath.IsEmpty()) {
        summary << "- 输入清单（含 SHA-256）: " << record.inputManifestPath << "\n";
    }
    if (!record.runtimeManifestPath.IsEmpty()) {
        summary << "- Runtime 清单: " << record.runtimeManifestPath << "\n";
    }
    if (!record.scriptPath.IsEmpty()) summary << "- 受控脚本: " << record.scriptPath << "\n";
    if (!record.artifactManifestPath.IsEmpty()) {
        summary << "- 网表校验清单: " << record.artifactManifestPath << "\n";
    }
    if (record.errorCount > 0) {
        summary << "\n## 关键诊断\n\n";
        for (const YosysLogEvent& event : record.events) {
            if (event.severity == YosysLogSeverity::Info) continue;
            summary << "- [" << ToString(event.severity) << "] " << event.ruleId
                    << ": " << event.suggestion << "\n";
            if (!event.sourceFile.IsEmpty()) {
                summary << "  - 位置: " << event.sourceFile << ":" << event.sourceLine << "\n";
            }
        }
    }
    return summary;
}

bool FpgaYosysReport::Save(const YosysLogRecord& record, const wxString& jsonPath,
                            const wxString& summaryPath, wxString& errorMessage)
{
    return WriteUtf8(jsonPath, GenerateJson(record), errorMessage) &&
           WriteUtf8(summaryPath, GenerateSummary(record), errorMessage);
}
