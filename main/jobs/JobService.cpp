#include "JobService.h"

#include "PlatformProcess.h"
#include "Sha256.h"

#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace {

std::atomic<unsigned long> g_jobSequence{0};

// B-04 并发上限：默认同类 1 个 Running，由注册表按描述符覆盖。
std::mutex g_concurrencyMutex;
std::map<ToolJobType, std::size_t> g_concurrencyLimits;

std::mutex g_processMutex;
std::map<wxString, void*> g_runningProcesses;
std::set<wxString> g_cancelRequested;

wxString ProcessKey(const wxString& projectPath, const wxString& jobId)
{
    wxFileName fileName(projectPath);
    fileName.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS);
    return fileName.GetFullPath() + "|" + jobId;
}

std::size_t CountRunning(const wxString& projectPath, ToolJobType type)
{
    std::vector<ToolJob> jobs;
    wxString ignored;
    std::size_t count = 0;
    if (!JobService().List(projectPath, jobs, ignored)) return count;
    for (const ToolJob& job : jobs) {
        if (job.request.type == type && job.state == ToolJobState::Running) ++count;
    }
    return count;
}

wxString NowUtc()
{
    // 统一 UTC（此前把本地时间当 UTC，审计/回放会错时区偏移）。
    return wxDateTime::Now().ToUTC().FormatISOCombined('T') + "Z";
}

bool EnsureDirectory(const wxString& path)
{
    return wxDirExists(path) || wxFileName::Mkdir(path, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

wxString NormalizePath(const wxString& path)
{
    wxFileName fileName(path);
    fileName.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS);
    return fileName.GetFullPath();
}

bool IsWithinDirectory(const wxString& path, const wxString& directory)
{
    wxString normalizedPath = NormalizePath(path);
    wxString normalizedDirectory = NormalizePath(directory);
#ifdef __WXMSW__
    // Windows 路径大小写不敏感；POSIX 保留大小写比较。
    normalizedPath.MakeLower();
    normalizedDirectory.MakeLower();
#endif
    if (!normalizedDirectory.EndsWith(wxFileName::GetPathSeparator())) {
        normalizedDirectory += wxFileName::GetPathSeparator();
    }
    return normalizedPath.StartsWith(normalizedDirectory);
}

bool WriteJson(const wxString& path, const Json::Value& value, wxString& errorMessage)
{
    const wxString temporaryPath = path + ".tmp";
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const wxString content = wxString::FromUTF8(Json::writeString(writer, value)) + "\n";
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    wxFile file(temporaryPath, wxFile::write);
    if (!file.IsOpened() || !utf8.data() ||
        file.Write(utf8.data(), utf8.length()) != static_cast<wxFileOffset>(utf8.length())) {
        file.Close();
        wxRemoveFile(temporaryPath);
        errorMessage = "Unable to write job JSON: " + path;
        return false;
    }
    file.Close();
    if (!wxRenameFile(temporaryPath, path, true)) {
        wxRemoveFile(temporaryPath);
        errorMessage = "Unable to replace job JSON: " + path;
        return false;
    }
    return true;
}

bool ReadJson(const wxString& path, Json::Value& value, wxString& errorMessage)
{
    wxFile file(path, wxFile::read);
    wxString content;
    if (!file.IsOpened() || !file.ReadAll(&content)) {
        errorMessage = "Unable to read job JSON: " + path;
        return false;
    }
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!utf8.data() || !reader->parse(utf8.data(), utf8.data() + utf8.length(), &value, &errors)) {
        errorMessage = "Unable to parse job JSON: " + wxString::FromUTF8(errors);
        return false;
    }
    return true;
}

Json::Value ToJson(const ToolJob& job)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["job_type"] = ToString(job.request.type).ToStdString();
    root["job_id"] = job.id.ToStdString();
    root["retry_of"] = job.retryOf.ToStdString();
    root["state"] = ToString(job.state).ToStdString();
    root["created_at"] = job.createdAt.ToStdString();
    root["updated_at"] = job.updatedAt.ToStdString();
    root["exit_code"] = job.exitCode;
    root["operator"] = job.request.operatorName.ToStdString();
    root["parameters"] = job.request.parameters;
    Json::Value transitions(Json::arrayValue);
    for (const ToolJobTransition& transition : job.transitions) {
        Json::Value item(Json::objectValue);
        item["state"] = ToString(transition.state).ToStdString();
        item["timestamp"] = transition.timestamp.ToStdString();
        item["operator"] = transition.operatorName.ToStdString();
        item["reason"] = transition.reason.ToStdString();
        item["exit_code"] = transition.exitCode;
        transitions.append(item);
    }
    root["transitions"] = transitions;
    return root;
}

bool FromJson(const Json::Value& root, ToolJob& job, wxString& errorMessage)
{
    ToolJobType type;
    ToolJobState state;
    if (!root.isObject() || !root["job_id"].isString() || !root["job_type"].isString() ||
        !root["state"].isString() || !ParseToolJobType(wxString::FromUTF8(root["job_type"].asString()), type) ||
        !ParseToolJobState(wxString::FromUTF8(root["state"].asString()), state)) {
        errorMessage = "Invalid tool job manifest.";
        return false;
    }
    job = ToolJob();
    job.id = wxString::FromUTF8(root["job_id"].asString());
    if (job.id.IsEmpty()) {
        errorMessage = "Invalid tool job ID.";
        return false;
    }
    job.request.type = type;
    job.state = state;
    job.retryOf = wxString::FromUTF8(root.get("retry_of", "").asString());
    job.createdAt = wxString::FromUTF8(root.get("created_at", "").asString());
    job.updatedAt = wxString::FromUTF8(root.get("updated_at", "").asString());
    job.exitCode = root.get("exit_code", 0).asInt();
    job.request.operatorName = wxString::FromUTF8(root.get("operator", "local").asString());
    job.request.parameters = root.get("parameters", Json::Value(Json::objectValue));
    for (const Json::Value& item : root["transitions"]) {
        ToolJobTransition transition;
        if (!item.isObject() || !ParseToolJobState(
                wxString::FromUTF8(item.get("state", "").asString()), transition.state)) {
            errorMessage = "Invalid tool job transition.";
            return false;
        }
        transition.timestamp = wxString::FromUTF8(item.get("timestamp", "").asString());
        transition.operatorName = wxString::FromUTF8(item.get("operator", "local").asString());
        transition.reason = wxString::FromUTF8(item.get("reason", "").asString());
        transition.exitCode = item.get("exit_code", 0).asInt();
        job.transitions.push_back(transition);
    }
    job.request.projectPath = wxString::FromUTF8(
        root.get("project_path", "").asString());
    job.request.retryOf = job.retryOf;
    return true;
}

Json::Value ToJson(const JobReport& report)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = report.schemaVersion.ToStdString();
    root["job_type"] = ToString(report.jobType).ToStdString();
    root["job_id"] = report.jobId.ToStdString();
    root["state"] = ToString(report.state).ToStdString();
    root["started_at"] = report.startedAt.ToStdString();
    root["completed_at"] = report.completedAt.ToStdString();
    root["duration_ms"] = Json::Int64(report.durationMs);
    root["exit_code"] = report.exitCode;
    root["summary"] = report.summary.ToStdString();
    Json::Value artifacts(Json::arrayValue);
    for (const JobArtifact& artifact : report.artifacts) {
        Json::Value item(Json::objectValue);
        item["path"] = artifact.path.ToStdString();
        item["kind"] = artifact.kind.ToStdString();
        item["size_bytes"] = Json::UInt64(artifact.sizeBytes);
        item["sha256"] = artifact.sha256.ToStdString();
        artifacts.append(item);
    }
    root["artifacts"] = artifacts;
    Json::Value errors(Json::arrayValue);
    for (const JobError& error : report.errors) {
        Json::Value item(Json::objectValue);
        item["code"] = error.code.ToStdString();
        item["severity"] = error.severity.ToStdString();
        item["stage"] = error.stage.ToStdString();
        item["ir_coordinate"] = error.irCoordinate.ToStdString();
        item["summary"] = error.summary.ToStdString();
        item["log_line"] = error.logLine;
        errors.append(item);
    }
    root["errors"] = errors;
    return root;
}

bool FromJson(const Json::Value& root, JobReport& report, wxString& errorMessage)
{
    ToolJobType type;
    ToolJobState state;
    if (!root.isObject() || !ParseToolJobType(
            wxString::FromUTF8(root.get("job_type", "").asString()), type) ||
        !ParseToolJobState(wxString::FromUTF8(root.get("state", "").asString()), state)) {
        errorMessage = "Invalid job report.";
        return false;
    }
    report = JobReport();
    report.schemaVersion = wxString::FromUTF8(root.get("schema_version", "1.0").asString());
    report.jobType = type;
    report.state = state;
    report.jobId = wxString::FromUTF8(root.get("job_id", "").asString());
    report.startedAt = wxString::FromUTF8(root.get("started_at", "").asString());
    report.completedAt = wxString::FromUTF8(root.get("completed_at", "").asString());
    report.durationMs = root.get("duration_ms", 0).asInt64();
    report.exitCode = root.get("exit_code", 0).asInt();
    report.summary = wxString::FromUTF8(root.get("summary", "").asString());
    for (const Json::Value& item : root["artifacts"]) {
        if (!item.isObject() || !item["path"].isString() || !item["kind"].isString()) continue;
        JobArtifact artifact;
        artifact.path = wxString::FromUTF8(item["path"].asString());
        artifact.kind = wxString::FromUTF8(item["kind"].asString());
        artifact.sizeBytes = item.get("size_bytes", Json::UInt64(0)).asUInt64();
        artifact.sha256 = wxString::FromUTF8(item.get("sha256", "").asString());
        report.artifacts.push_back(artifact);
    }
    for (const Json::Value& item : root["errors"]) {
        if (!item.isObject()) continue;
        JobError error;
        error.code = wxString::FromUTF8(item.get("code", "").asString());
        error.severity = wxString::FromUTF8(item.get("severity", "error").asString());
        error.stage = wxString::FromUTF8(item.get("stage", "").asString());
        error.irCoordinate = wxString::FromUTF8(item.get("ir_coordinate", "").asString());
        error.summary = wxString::FromUTF8(item.get("summary", "").asString());
        error.logLine = item.get("log_line", 0).asInt();
        report.errors.push_back(error);
    }
    return true;
}

bool WriteManifest(const ToolJob& job, wxString& errorMessage)
{
    return WriteJson(JobService::GetPaths(job.request.projectPath, job.request.type, job.id).manifest,
                     ToJson(job), errorMessage);
}

bool GetFileSize(const wxString& path, std::uint64_t& sizeBytes)
{
    wxFileName fileName(path);
    const wxULongLong size = fileName.GetSize();
    if (size == wxInvalidSize) return false;
    sizeBytes = static_cast<std::uint64_t>(size.GetValue());
    return true;
}

} // namespace

wxString ToString(ToolJobType type)
{
    switch (type) {
    case ToolJobType::Simulation: return "simulation";
    case ToolJobType::Synthesis: return "synthesis";
    case ToolJobType::PnR: return "pnr";
    case ToolJobType::Pack: return "pack";
    case ToolJobType::Flash: return "flash";
    }
    return "unknown";
}

bool ParseToolJobType(const wxString& value, ToolJobType& type)
{
    for (ToolJobType candidate : { ToolJobType::Simulation, ToolJobType::Synthesis,
                                   ToolJobType::PnR, ToolJobType::Pack, ToolJobType::Flash }) {
        if (value == ToString(candidate)) {
            type = candidate;
            return true;
        }
    }
    return false;
}

wxString ToString(ToolJobState state)
{
    switch (state) {
    case ToolJobState::Created: return "Created";
    case ToolJobState::Validating: return "Validating";
    case ToolJobState::Queued: return "Queued";
    case ToolJobState::Running: return "Running";
    case ToolJobState::ValidatingArtifact: return "ValidatingArtifact";
    case ToolJobState::Succeeded: return "Succeeded";
    case ToolJobState::Failed: return "Failed";
    case ToolJobState::Cancelled: return "Cancelled";
    case ToolJobState::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

bool ParseToolJobState(const wxString& value, ToolJobState& state)
{
    for (ToolJobState candidate : { ToolJobState::Created, ToolJobState::Validating,
            ToolJobState::Queued, ToolJobState::Running, ToolJobState::ValidatingArtifact,
            ToolJobState::Succeeded, ToolJobState::Failed, ToolJobState::Cancelled,
            ToolJobState::TimedOut }) {
        if (value == ToString(candidate)) {
            state = candidate;
            return true;
        }
    }
    return false;
}

bool IsTerminalToolJobState(ToolJobState state)
{
    return state == ToolJobState::Succeeded || state == ToolJobState::Failed ||
           state == ToolJobState::Cancelled || state == ToolJobState::TimedOut;
}

ToolJobPaths JobService::GetPaths(const wxString& projectPath, ToolJobType type,
                                  const wxString& jobId)
{
    const wxString separator = wxFileName::GetPathSeparator();
    ToolJobPaths paths;
    paths.root = projectPath + separator + ".sigflow" + separator + "jobs" + separator +
                 ToString(type) + separator + jobId;
    paths.inputs = paths.root + separator + "inputs";
    paths.scripts = paths.root + separator + "scripts";
    paths.logs = paths.root + separator + "logs";
    paths.artifacts = paths.root + separator + "artifacts";
    paths.reports = paths.root + separator + "reports";
    paths.manifest = paths.root + separator + "manifest.json";
    paths.jobReport = paths.reports + separator + "job-report.json";
    return paths;
}

bool JobService::IsSafeJobId(const wxString& value)
{
    if (value.IsEmpty()) return false;
    for (wxChar character : value) {
        if (!(wxIsalnum(character) || character == '-' || character == '_')) return false;
    }
    return true;
}

bool JobService::Create(const ToolJobRequest& request, ToolJob& job, wxString& errorMessage) const
{
    errorMessage.clear();
    if (request.projectPath.IsEmpty() || !wxDirExists(request.projectPath)) {
        errorMessage = "A project directory is required.";
        return false;
    }
    job = ToolJob();
    job.id = wxDateTime::UNow().Format("%Y%m%dT%H%M%S") + "-" +
        wxString::Format("%06lu", ++g_jobSequence);
    job.retryOf = request.retryOf;
    job.request = request;
    job.createdAt = NowUtc();
    job.updatedAt = job.createdAt;
    job.transitions.push_back({ ToolJobState::Created, job.createdAt,
                                request.operatorName, "Job created.", 0 });
    const ToolJobPaths paths = GetPaths(request.projectPath, request.type, job.id);
    for (const wxString& directory : { paths.inputs, paths.scripts, paths.logs,
                                       paths.artifacts, paths.reports }) {
        if (!EnsureDirectory(directory)) {
            errorMessage = "Unable to create job directory: " + directory;
            return false;
        }
    }
    Json::Value requestJson = request.parameters;
    requestJson["job_type"] = ToString(request.type).ToStdString();
    requestJson["project_path"] = NormalizePath(request.projectPath).ToStdString();
    if (!WriteJson(paths.inputs + wxFileName::GetPathSeparator() + "request.json",
                   requestJson, errorMessage)) return false;
    return WriteManifest(job, errorMessage);
}

bool JobService::Load(const wxString& projectPath, const wxString& jobId, ToolJob& job,
                      wxString& errorMessage) const
{
    errorMessage.clear();
    if (!IsSafeJobId(jobId)) {
        errorMessage = "Invalid tool job ID.";
        return false;
    }
    Json::Value root;
    for (ToolJobType type : { ToolJobType::Simulation, ToolJobType::Synthesis, ToolJobType::PnR,
                              ToolJobType::Pack, ToolJobType::Flash }) {
        const wxString path = GetPaths(projectPath, type, jobId).manifest;
        if (!wxFileExists(path)) continue;
        if (!ReadJson(path, root, errorMessage)) return false;
        if (!FromJson(root, job, errorMessage)) return false;
        job.request.projectPath = projectPath;
        return true;
    }
    errorMessage = "Tool job manifest not found: " + jobId;
    return false;
}

bool JobService::List(const wxString& projectPath, std::vector<ToolJob>& jobs,
                      wxString& errorMessage) const
{
    errorMessage.clear();
    jobs.clear();
    for (ToolJobType type : { ToolJobType::Simulation, ToolJobType::Synthesis, ToolJobType::PnR,
                              ToolJobType::Pack, ToolJobType::Flash }) {
        const wxString root = projectPath + wxFileName::GetPathSeparator() + ".sigflow" +
                              wxFileName::GetPathSeparator() + "jobs" +
                              wxFileName::GetPathSeparator() + ToString(type);
        if (!wxDirExists(root)) continue;
        wxDir directory(root);
        wxString name;
        bool found = directory.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
        while (found) {
            ToolJob job;
            wxString loadError;
            if (Load(projectPath, name, job, loadError)) jobs.push_back(job);
            else if (errorMessage.IsEmpty()) errorMessage = loadError;
            found = directory.GetNext(&name);
        }
    }
    return errorMessage.IsEmpty();
}

bool JobService::IsLegalTransition(ToolJobState source, ToolJobState target)
{
    switch (source) {
    case ToolJobState::Created:
        return target == ToolJobState::Validating || target == ToolJobState::Cancelled ||
               target == ToolJobState::Failed;
    case ToolJobState::Validating:
        return target == ToolJobState::Queued || target == ToolJobState::Cancelled ||
               target == ToolJobState::Failed;
    case ToolJobState::Queued:
        return target == ToolJobState::Running || target == ToolJobState::Cancelled ||
               target == ToolJobState::Failed;
    case ToolJobState::Running:
        return target == ToolJobState::ValidatingArtifact || target == ToolJobState::Cancelled ||
               target == ToolJobState::Failed || target == ToolJobState::TimedOut;
    case ToolJobState::ValidatingArtifact:
        return target == ToolJobState::Succeeded || target == ToolJobState::Failed;
    default:
        return false;
    }
}

bool JobService::Transition(const wxString& projectPath, const wxString& jobId,
                            ToolJobState targetState, const wxString& reason, int exitCode,
                            wxString& errorMessage) const
{
    ToolJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (!IsLegalTransition(job.state, targetState)) {
        errorMessage = "Illegal tool job transition: " + ToString(job.state) + " -> " +
                       ToString(targetState);
        return false;
    }
    job.state = targetState;
    job.updatedAt = NowUtc();
    job.exitCode = exitCode;
    job.transitions.push_back({ targetState, job.updatedAt, job.request.operatorName,
                                reason, exitCode });
    return WriteManifest(job, errorMessage);
}

bool JobService::Start(const wxString& projectPath, const wxString& jobId,
                       wxString& errorMessage) const
{
    ToolJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (job.state == ToolJobState::Running) return true;
    if (IsTerminalToolJobState(job.state)) {
        errorMessage = "A terminal tool job cannot be started: " + ToString(job.state);
        return false;
    }
    if (CountRunning(projectPath, job.request.type) >= ConcurrencyLimit(job.request.type)) {
        errorMessage = "Concurrency limit reached for " + ToString(job.request.type) +
                       "; job remains queued.";
        return false;
    }
    if (job.state == ToolJobState::Created &&
        !Transition(projectPath, jobId, ToolJobState::Validating,
                    "Job validation started.", 0, errorMessage)) return false;
    if (job.state != ToolJobState::Queued &&
        !Transition(projectPath, jobId, ToolJobState::Queued,
                    "Job queued.", 0, errorMessage)) return false;
    return Transition(projectPath, jobId, ToolJobState::Running,
                      "Job execution started.", 0, errorMessage);
}

bool JobService::Cancel(const wxString& projectPath, const wxString& jobId,
                        const wxString& reason, wxString& errorMessage) const
{
    ToolJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (job.state != ToolJobState::Created && job.state != ToolJobState::Validating &&
        job.state != ToolJobState::Queued && job.state != ToolJobState::Running) {
        errorMessage = "Only an active tool job can be cancelled.";
        return false;
    }
    if (job.state == ToolJobState::Running) {
        std::lock_guard<std::mutex> lock(g_processMutex);
        const wxString key = ProcessKey(projectPath, jobId);
        g_cancelRequested.insert(key);
        const auto found = g_runningProcesses.find(key);
        if (found != g_runningProcesses.end() && found->second != nullptr) {
            // 真正终止工具进程（含其子进程树），否则 manifest 说 Cancelled 而进程仍在跑。
            // 必须在同一把锁内调用，防止执行线程在 Terminate 前注销并关闭句柄。
            PlatformProcess::Terminate(found->second);
        }
    }
    return Transition(projectPath, jobId, ToolJobState::Cancelled, reason, -1, errorMessage);
}

void JobService::RegisterProcess(const wxString& projectPath, const wxString& jobId,
                                 void* processHandle)
{
    bool cancelImmediately = false;
    {
        std::lock_guard<std::mutex> lock(g_processMutex);
        const wxString key = ProcessKey(projectPath, jobId);
        g_runningProcesses[key] = processHandle;
        cancelImmediately = g_cancelRequested.find(key) != g_cancelRequested.end();
    }
    if (cancelImmediately) PlatformProcess::Terminate(processHandle);
}

void JobService::UnregisterProcess(const wxString& projectPath, const wxString& jobId)
{
    std::lock_guard<std::mutex> lock(g_processMutex);
    g_runningProcesses.erase(ProcessKey(projectPath, jobId));
}

bool JobService::IsCancelRequested(const wxString& projectPath, const wxString& jobId)
{
    std::lock_guard<std::mutex> lock(g_processMutex);
    return g_cancelRequested.find(ProcessKey(projectPath, jobId)) != g_cancelRequested.end();
}

void JobService::ClearCancelRequested(const wxString& projectPath, const wxString& jobId)
{
    std::lock_guard<std::mutex> lock(g_processMutex);
    g_cancelRequested.erase(ProcessKey(projectPath, jobId));
}

bool JobService::Retry(const wxString& projectPath, const wxString& jobId,
                       ToolJob& retryJob, wxString& errorMessage) const
{
    ToolJob original;
    if (!Load(projectPath, jobId, original, errorMessage)) return false;
    if (!IsTerminalToolJobState(original.state)) {
        errorMessage = "Only terminal tool jobs can be retried.";
        return false;
    }
    ToolJobRequest request = original.request;
    request.retryOf = original.id;
    if (!Create(request, retryJob, errorMessage)) return false;
    return true;
}

wxString JobService::Sha256File(const wxString& path)
{
    return Sha256FileHex(path);
}

void JobService::SetConcurrencyLimit(ToolJobType type, std::size_t limit)
{
    std::lock_guard<std::mutex> lock(g_concurrencyMutex);
    g_concurrencyLimits[type] = limit == 0 ? 1 : limit;
}

std::size_t JobService::ConcurrencyLimit(ToolJobType type)
{
    std::lock_guard<std::mutex> lock(g_concurrencyMutex);
    const auto found = g_concurrencyLimits.find(type);
    return found == g_concurrencyLimits.end() ? 1 : found->second;
}

bool JobService::WriteReport(const wxString& projectPath, const wxString& jobId,
                             const JobReport& report, wxString& errorMessage) const
{
    ToolJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (report.jobType != job.request.type) {
        errorMessage = "Job report type does not match the job manifest.";
        return false;
    }
    ToolJobPaths paths = GetPaths(projectPath, job.request.type, jobId);
    if (!EnsureDirectory(paths.reports)) {
        errorMessage = "Unable to create job report directory: " + paths.reports;
        return false;
    }
    return WriteJson(paths.jobReport, ToJson(report), errorMessage);
}

bool JobService::LoadReport(const wxString& projectPath, const wxString& jobId,
                            JobReport& report, wxString& errorMessage) const
{
    ToolJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    Json::Value root;
    if (!ReadJson(GetPaths(projectPath, job.request.type, jobId).jobReport, root, errorMessage)) return false;
    return FromJson(root, report, errorMessage);
}

bool JobService::RecordArtifact(const wxString& projectPath, const wxString& jobId,
                                const wxString& path, const wxString& kind,
                                wxString& errorMessage) const
{
    ToolJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    const wxString normalized = NormalizePath(path);
    if (!wxFileExists(normalized) || !IsWithinDirectory(normalized, projectPath)) {
        errorMessage = "Artifact must be an existing file inside the project: " + normalized;
        return false;
    }
    JobReport report;
    if (!wxFileExists(GetPaths(projectPath, job.request.type, jobId).jobReport)) {
        report.jobType = job.request.type;
        report.jobId = job.id;
        report.state = job.state;
    } else if (!LoadReport(projectPath, jobId, report, errorMessage)) {
        return false;
    }
    JobArtifact artifact;
    artifact.path = normalized;
    artifact.kind = kind;
    GetFileSize(normalized, artifact.sizeBytes);
    artifact.sha256 = Sha256File(normalized);
    if (artifact.sha256.IsEmpty()) {
        errorMessage = "Unable to calculate artifact SHA-256: " + normalized;
        return false;
    }
    report.artifacts.push_back(artifact);
    report.state = job.state;
    return WriteReport(projectPath, job.id, report, errorMessage);
}

bool JobServiceRegistry::Register(const JobToolDescriptor& descriptor, wxString& errorMessage)
{
    if (descriptor.name.IsEmpty() || descriptor.maxConcurrent == 0) {
        errorMessage = "A job tool name and positive concurrency limit are required.";
        return false;
    }
    if (!descriptor.handler) {
        errorMessage = "A job tool run handler is required: " + descriptor.name;
        return false;
    }
    for (const JobToolDescriptor& existing : descriptors_) {
        if (existing.type == descriptor.type || existing.name == descriptor.name) {
            errorMessage = "Job tool is already registered: " + descriptor.name;
            return false;
        }
    }
    descriptors_.push_back(descriptor);
    JobService::SetConcurrencyLimit(descriptor.type, descriptor.maxConcurrent);
    return true;
}

bool JobServiceRegistry::Find(ToolJobType type, JobToolDescriptor& descriptor) const
{
    for (const JobToolDescriptor& candidate : descriptors_) {
        if (candidate.type == type) {
            descriptor = candidate;
            return true;
        }
    }
    return false;
}

std::vector<JobToolDescriptor> JobServiceRegistry::List() const
{
    return descriptors_;
}

bool JobServiceRegistry::ValidateParameters(const JobToolDescriptor& descriptor,
                                            const Json::Value& parameters, wxString& errorMessage)
{
    const Json::Value& properties = descriptor.inputSchema["properties"];
    if (!properties.isObject()) return true;
    if (!parameters.isNull() && !parameters.isObject()) {
        errorMessage = descriptor.name + ": job parameters must be a JSON object.";
        return false;
    }
    const Json::Value& required = descriptor.inputSchema["required"];
    if (required.isArray()) {
        for (const Json::Value& name : required) {
            if (!name.isString()) continue;
            const Json::Value& value = parameters[name.asString()];
            if (value.isNull() || (value.isString() && value.asString().empty())) {
                errorMessage = descriptor.name + ": missing required parameter '" +
                               wxString::FromUTF8(name.asString()) + "'.";
                return false;
            }
        }
    }
    for (const std::string& name : properties.getMemberNames()) {
        const Json::Value& value = parameters[name];
        if (value.isNull()) continue;
        const Json::Value& spec = properties[name];
        const std::string type = spec.get("type", "").asString();
        const bool typeOk =
            (type == "string" && value.isString()) ||
            (type == "integer" && value.isInt64()) ||
            (type == "number" && (value.isNumeric())) ||
            (type == "boolean" && value.isBool()) ||
            (type == "array" && value.isArray()) ||
            (type == "object" && value.isObject()) ||
            type.empty();
        if (!typeOk) {
            errorMessage = descriptor.name + ": parameter '" + wxString::FromUTF8(name) +
                           "' must be of type " + wxString::FromUTF8(type) + ".";
            return false;
        }
        const Json::Value& allowed = spec["enum"];
        if (allowed.isArray()) {
            bool matched = false;
            for (const Json::Value& candidate : allowed) {
                if (candidate == value) { matched = true; break; }
            }
            if (!matched) {
                errorMessage = descriptor.name + ": parameter '" + wxString::FromUTF8(name) +
                               "' is not an allowed value.";
                return false;
            }
        }
    }
    return true;
}

bool JobServiceRegistry::Submit(const ToolJobRequest& request, ToolJob& job,
                                wxString& errorMessage) const
{
    JobToolDescriptor descriptor;
    if (!Find(request.type, descriptor)) {
        errorMessage = "No registered job tool for type: " + ToString(request.type);
        return false;
    }
    if (!ValidateParameters(descriptor, request.parameters, errorMessage)) return false;
    return JobService().Create(request, job, errorMessage);
}

bool JobServiceRegistry::Run(const ToolJobRequest& request, const JobExecutionOptions& options,
                             ToolJob& job, JobReport& report, wxString& errorMessage) const
{
    JobToolDescriptor descriptor;
    if (!Find(request.type, descriptor)) {
        errorMessage = "No registered job tool for type: " + ToString(request.type);
        return false;
    }
    if (!Submit(request, job, errorMessage)) return false;
    if (!descriptor.handler(job, options, report, errorMessage)) return false;
    return true;
}
