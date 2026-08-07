#include "FpgaSynthesisJob.h"

#include <json/json.h>

#include <windows.h>
#include <bcrypt.h>

#include <wx/dir.h>
#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

std::atomic<unsigned long> g_jobSequence{0};

wxString NowUtc()
{
    return wxDateTime::UNow().FormatISOCombined('T') + "Z";
}

bool IsSafeJobId(const wxString& value)
{
    if (value.IsEmpty()) {
        return false;
    }
    for (const wxChar character : value) {
        if (!(wxIsalnum(character) || character == '-' || character == '_')) {
            return false;
        }
    }
    return true;
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
    normalizedPath.MakeLower();
    normalizedDirectory.MakeLower();
    if (!normalizedDirectory.EndsWith("\\")) normalizedDirectory += "\\";
    return normalizedPath.StartsWith(normalizedDirectory);
}

bool WriteJsonAtomically(const wxString& path, const Json::Value& value, wxString& errorMessage);

wxString Sha256File(const wxString& filePath)
{
    wxFile file(filePath, wxFile::read);
    if (!file.IsOpened()) return wxString();

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD bytesReturned = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                                   &bytesReturned, 0);
    }
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                   reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                                   &bytesReturned, 0);
    }
    std::vector<unsigned char> object(objectLength);
    std::vector<unsigned char> value(hashLength);
    if (status >= 0) {
        status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);
    }
    std::vector<unsigned char> buffer(64 * 1024);
    while (status >= 0) {
        const wxFileOffset bytesRead = file.Read(buffer.data(), buffer.size());
        if (bytesRead == wxInvalidOffset) {
            status = -1;
        } else if (bytesRead == 0) {
            break;
        } else {
            status = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(bytesRead), 0);
        }
    }
    if (status >= 0) status = BCryptFinishHash(hash, value.data(), hashLength, 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return wxString();

    wxString result;
    for (unsigned char byte : value) result += wxString::Format("%02x", byte);
    return result;
}

bool WriteInputFileList(const SynthesisJob& job, const SynthesisJobPaths& paths, wxString& errorMessage)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["project_path"] = NormalizePath(job.request.projectPath).ToStdString();
    root["top_module"] = job.request.topModule.ToStdString();
    Json::Value files(Json::arrayValue);
    for (const wxString& source : job.request.sourceFiles) {
        Json::Value item(Json::objectValue);
        item["path"] = NormalizePath(source).ToStdString();
        item["name"] = wxFileName(source).GetFullName().ToStdString();
        item["sha256"] = Sha256File(source).ToStdString();
        files.append(item);
    }
    root["files"] = files;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return WriteJsonAtomically(paths.inputs + "\\file-list.json", root, errorMessage);
}

bool WriteJsonAtomically(const wxString& path, const Json::Value& value, wxString& errorMessage)
{
    wxFileName fileName(path);
    const wxString temporaryPath = path + ".tmp";
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const wxString content = wxString::FromUTF8(Json::writeString(writer, value)) + "\n";
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    wxFile file(temporaryPath, wxFile::write);
    if (!file.IsOpened() || !utf8.data() ||
        file.Write(utf8.data(), utf8.length()) != static_cast<wxFileOffset>(utf8.length())) {
        errorMessage = wxString("Unable to write job manifest: ") + path;
        file.Close();
        wxRemoveFile(temporaryPath);
        return false;
    }
    file.Close();
    if (!wxRenameFile(temporaryPath, path, true)) {
        errorMessage = wxString("Unable to replace job manifest: ") + path;
        wxRemoveFile(temporaryPath);
        return false;
    }
    return true;
}

Json::Value ToJson(const SynthesisJob& job)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["job_id"] = job.id.ToStdString();
    root["retry_of"] = job.retryOf.ToStdString();
    root["state"] = ToString(job.state).ToStdString();
    root["created_at"] = job.createdAt.ToStdString();
    root["updated_at"] = job.updatedAt.ToStdString();
    root["exit_code"] = job.exitCode;
    Json::Value request(Json::objectValue);
    request["project_path"] = job.request.projectPath.ToStdString();
    request["top_module"] = job.request.topModule.ToStdString();
    request["target_profile"] = job.request.targetProfileId.ToStdString();
    request["target_profile_version"] = job.request.targetProfileVersion.ToStdString();
    request["strategy"] = job.request.strategyId.ToStdString();
    request["strategy_version"] = job.request.strategyVersion.ToStdString();
    request["operator"] = job.request.operatorName.ToStdString();
    request["retry_of"] = job.request.retryOf.ToStdString();
    Json::Value sourceFiles(Json::arrayValue);
    for (const wxString& sourceFile : job.request.sourceFiles) {
        sourceFiles.append(sourceFile.ToStdString());
    }
    request["source_files"] = sourceFiles;
    root["request"] = request;
    Json::Value transitions(Json::arrayValue);
    for (const SynthesisJobTransition& transition : job.transitions) {
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

bool FromJson(const Json::Value& root, SynthesisJob& job, wxString& errorMessage)
{
    if (!root.isObject() || !root["job_id"].isString() || !root["request"].isObject() ||
        !root["state"].isString()) {
        errorMessage = "Invalid synthesis job manifest.";
        return false;
    }
    job = SynthesisJob();
    job.id = wxString::FromUTF8(root["job_id"].asString());
    if (!IsSafeJobId(job.id) || !ParseSynthesisJobState(wxString::FromUTF8(root["state"].asString()), job.state)) {
        errorMessage = "Invalid synthesis job ID or state.";
        return false;
    }
    job.retryOf = wxString::FromUTF8(root["retry_of"].asString());
    job.createdAt = wxString::FromUTF8(root["created_at"].asString());
    job.updatedAt = wxString::FromUTF8(root["updated_at"].asString());
    job.exitCode = root["exit_code"].asInt();
    const Json::Value& request = root["request"];
    job.request.projectPath = wxString::FromUTF8(request["project_path"].asString());
    job.request.topModule = wxString::FromUTF8(request["top_module"].asString());
    job.request.targetProfileId = wxString::FromUTF8(request["target_profile"].asString());
    job.request.targetProfileVersion = wxString::FromUTF8(request["target_profile_version"].asString());
    job.request.strategyId = wxString::FromUTF8(request["strategy"].asString());
    if (request["strategy_version"].isString()) {
        job.request.strategyVersion = wxString::FromUTF8(request["strategy_version"].asString());
    }
    job.request.operatorName = wxString::FromUTF8(request["operator"].asString());
    job.request.retryOf = wxString::FromUTF8(request["retry_of"].asString());
    for (const Json::Value& sourceFile : request["source_files"]) {
        if (sourceFile.isString()) job.request.sourceFiles.push_back(wxString::FromUTF8(sourceFile.asString()));
    }
    for (const Json::Value& item : root["transitions"]) {
        SynthesisJobTransition transition;
        if (!item.isObject() || !ParseSynthesisJobState(wxString::FromUTF8(item["state"].asString()), transition.state)) {
            errorMessage = "Invalid synthesis job transition.";
            return false;
        }
        transition.timestamp = wxString::FromUTF8(item["timestamp"].asString());
        transition.operatorName = wxString::FromUTF8(item["operator"].asString());
        transition.reason = wxString::FromUTF8(item["reason"].asString());
        transition.exitCode = item["exit_code"].asInt();
        job.transitions.push_back(transition);
    }
    return true;
}

bool ReadManifest(const wxString& path, SynthesisJob& job, wxString& errorMessage)
{
    wxFile file(path, wxFile::read);
    wxString content;
    if (!file.IsOpened() || !file.ReadAll(&content)) {
        errorMessage = wxString("Unable to read job manifest: ") + path;
        return false;
    }
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!utf8.data() || !reader->parse(utf8.data(), utf8.data() + utf8.length(), &root, &errors)) {
        errorMessage = wxString("Unable to parse job manifest: ") + wxString::FromUTF8(errors);
        return false;
    }
    return FromJson(root, job, errorMessage);
}

bool WriteManifest(const SynthesisJob& job, wxString& errorMessage)
{
    return WriteJsonAtomically(FpgaSynthesisJobService::GetPaths(job.request.projectPath, job.id).manifest,
                               ToJson(job), errorMessage);
}

} // namespace

wxString ToString(SynthesisJobState state)
{
    switch (state) {
    case SynthesisJobState::Created: return "Created";
    case SynthesisJobState::Validating: return "Validating";
    case SynthesisJobState::Queued: return "Queued";
    case SynthesisJobState::Running: return "Running";
    case SynthesisJobState::ValidatingArtifact: return "ValidatingArtifact";
    case SynthesisJobState::Succeeded: return "Succeeded";
    case SynthesisJobState::Failed: return "Failed";
    case SynthesisJobState::Cancelled: return "Cancelled";
    case SynthesisJobState::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

bool ParseSynthesisJobState(const wxString& value, SynthesisJobState& state)
{
    for (SynthesisJobState candidate : { SynthesisJobState::Created, SynthesisJobState::Validating,
            SynthesisJobState::Queued, SynthesisJobState::Running, SynthesisJobState::ValidatingArtifact,
            SynthesisJobState::Succeeded, SynthesisJobState::Failed, SynthesisJobState::Cancelled,
            SynthesisJobState::TimedOut }) {
        if (value == ToString(candidate)) { state = candidate; return true; }
    }
    return false;
}

bool IsTerminalSynthesisJobState(SynthesisJobState state)
{
    return state == SynthesisJobState::Succeeded || state == SynthesisJobState::Failed ||
           state == SynthesisJobState::Cancelled || state == SynthesisJobState::TimedOut;
}

SynthesisJobPaths FpgaSynthesisJobService::GetPaths(const wxString& projectPath, const wxString& jobId)
{
    SynthesisJobPaths paths;
    paths.root = projectPath + "\\.sigflow\\fpga\\runs\\" + jobId;
    paths.inputs = paths.root + "\\inputs";
    paths.scripts = paths.root + "\\scripts";
    paths.logs = paths.root + "\\logs";
    paths.artifacts = paths.root + "\\artifacts";
    paths.reports = paths.root + "\\reports";
    paths.manifest = paths.root + "\\manifest.json";
    return paths;
}

bool FpgaSynthesisJobService::Create(const SynthesisJobRequest& request, SynthesisJob& job,
                                     wxString& errorMessage) const
{
    errorMessage.clear();
    if (request.projectPath.IsEmpty() || request.topModule.IsEmpty() || request.sourceFiles.empty() ||
        !wxDirExists(request.projectPath)) {
        errorMessage = "A project directory, top module, and at least one RTL source are required.";
        return false;
    }
    for (const wxString& sourceFile : request.sourceFiles) {
        if (!wxFileExists(sourceFile)) {
            errorMessage = wxString("RTL source does not exist: ") + sourceFile;
            return false;
        }
        if (!IsWithinDirectory(sourceFile, request.projectPath)) {
            errorMessage = wxString("RTL source is outside the project directory: ") + sourceFile;
            return false;
        }
    }
    job = SynthesisJob();
    job.request = request;
    job.retryOf = request.retryOf;
    job.id = wxDateTime::UNow().Format("%Y%m%dT%H%M%S") + "-" +
        wxString::Format("%06lu", ++g_jobSequence);
    job.createdAt = NowUtc();
    job.updatedAt = job.createdAt;
    job.transitions.push_back({ SynthesisJobState::Created, job.createdAt, request.operatorName,
                                "Job created.", 0 });
    const SynthesisJobPaths paths = GetPaths(request.projectPath, job.id);
    for (const wxString& directory : { paths.inputs, paths.scripts, paths.logs, paths.artifacts, paths.reports }) {
        if (!EnsureDirectory(directory)) {
            errorMessage = wxString("Unable to create job directory: ") + directory;
            return false;
        }
    }
    return WriteInputFileList(job, paths, errorMessage) && WriteManifest(job, errorMessage);
}

bool FpgaSynthesisJobService::CreateSynthesisJob(const SynthesisJobRequest& request,
                                                 SynthesisJob& job, wxString& errorMessage) const
{
    return Create(request, job, errorMessage);
}

bool FpgaSynthesisJobService::Load(const wxString& projectPath, const wxString& jobId, SynthesisJob& job,
                                   wxString& errorMessage) const
{
    errorMessage.clear();
    if (!IsSafeJobId(jobId)) { errorMessage = "Invalid synthesis job ID."; return false; }
    return ReadManifest(GetPaths(projectPath, jobId).manifest, job, errorMessage);
}

bool FpgaSynthesisJobService::Transition(const wxString& projectPath, const wxString& jobId,
                                         SynthesisJobState targetState, const wxString& reason,
                                         int exitCode, wxString& errorMessage) const
{
    errorMessage.clear();
    SynthesisJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (!IsLegalTransition(job.state, targetState)) {
        errorMessage = wxString("Illegal synthesis job transition: ") + ToString(job.state) + " -> " + ToString(targetState);
        return false;
    }
    job.state = targetState;
    job.updatedAt = NowUtc();
    job.exitCode = exitCode;
    job.transitions.push_back({ targetState, job.updatedAt, job.request.operatorName, reason, exitCode });
    return WriteManifest(job, errorMessage);
}

bool FpgaSynthesisJobService::Cancel(const wxString& projectPath, const wxString& jobId,
                                     const wxString& reason, wxString& errorMessage) const
{
    errorMessage.clear();
    SynthesisJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (job.state != SynthesisJobState::Running) {
        errorMessage = "Only a running synthesis job can be cancelled.";
        return false;
    }
    return Transition(projectPath, jobId, SynthesisJobState::Cancelled, reason, job.exitCode, errorMessage);
}

bool FpgaSynthesisJobService::Retry(const wxString& projectPath, const wxString& jobId,
                                    SynthesisJob& retryJob, wxString& errorMessage) const
{
    errorMessage.clear();
    SynthesisJob original;
    if (!Load(projectPath, jobId, original, errorMessage)) return false;
    if (!IsTerminalSynthesisJobState(original.state)) {
        errorMessage = "Only terminal synthesis jobs can be retried.";
        return false;
    }
    if (!Create(original.request, retryJob, errorMessage)) return false;
    retryJob.retryOf = original.id;
    retryJob.request.retryOf = original.id;
    return WriteManifest(retryJob, errorMessage);
}

bool FpgaSynthesisJobService::List(const wxString& projectPath, std::vector<SynthesisJob>& jobs,
                                   wxString& errorMessage) const
{
    errorMessage.clear();
    jobs.clear();
    const wxString runsDirectory = projectPath + "\\.sigflow\\fpga\\runs";
    if (!wxDirExists(runsDirectory)) return true;
    wxDir directory(runsDirectory);
    wxString name;
    bool found = directory.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
    while (found) {
        SynthesisJob job;
        wxString loadError;
        if (Load(projectPath, name, job, loadError)) jobs.push_back(job);
        else if (errorMessage.IsEmpty()) errorMessage = loadError;
        found = directory.GetNext(&name);
    }
    return errorMessage.IsEmpty();
}

bool FpgaSynthesisJobService::GetSynthesisArtifacts(const wxString& projectPath, const wxString& jobId,
                                                    std::vector<wxString>& artifactPaths,
                                                    wxString& errorMessage) const
{
    errorMessage.clear();
    SynthesisJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    artifactPaths.clear();
    const SynthesisJobPaths paths = GetPaths(projectPath, jobId);
    wxDir directory(paths.root);
    wxString fileName;
    bool found = directory.GetFirst(&fileName, wxEmptyString, wxDIR_FILES);
    while (found) {
        artifactPaths.push_back(paths.root + "\\" + fileName);
        found = directory.GetNext(&fileName);
    }
    for (const wxString& subdirectory : { paths.scripts, paths.logs, paths.artifacts, paths.reports }) {
        wxDir child(subdirectory);
        if (!child.IsOpened()) continue;
        found = child.GetFirst(&fileName, wxEmptyString, wxDIR_FILES);
        while (found) {
            artifactPaths.push_back(subdirectory + "\\" + fileName);
            found = child.GetNext(&fileName);
        }
    }
    return true;
}

bool FpgaSynthesisJobService::GetSynthesisReport(const wxString& projectPath, const wxString& jobId,
                                                 wxString& jsonReport, wxString& summaryReport,
                                                 wxString& errorMessage) const
{
    errorMessage.clear();
    SynthesisJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    const SynthesisJobPaths paths = GetPaths(projectPath, jobId);
    wxFile jsonFile(paths.reports + "\\synthesis.analysis.json", wxFile::read);
    wxFile summaryFile(paths.reports + "\\synthesis.summary.md", wxFile::read);
    if (!jsonFile.IsOpened() || !jsonFile.ReadAll(&jsonReport)) {
        errorMessage = "Synthesis analysis report is not available for job " + jobId + ".";
        return false;
    }
    if (!summaryFile.IsOpened() || !summaryFile.ReadAll(&summaryReport)) {
        errorMessage = "Synthesis summary report is not available for job " + jobId + ".";
        return false;
    }
    return true;
}

bool FpgaSynthesisJobService::RecoverStaleJobs(const wxString& projectPath, const wxString& activeJobId,
                                               wxString& errorMessage) const
{
    errorMessage.clear();
    std::vector<SynthesisJob> jobs;
    if (!List(projectPath, jobs, errorMessage)) return false;
    for (const SynthesisJob& job : jobs) {
        if (job.id == activeJobId || job.state != SynthesisJobState::Running) continue;
        wxString updateError;
        if (!Transition(projectPath, job.id, SynthesisJobState::Failed,
                        "Recovered stale running job after application restart.", -1, updateError) &&
            errorMessage.IsEmpty()) {
            errorMessage = updateError;
        }
    }
    return errorMessage.IsEmpty();
}

bool FpgaSynthesisJobService::IsLegalTransition(SynthesisJobState source, SynthesisJobState target)
{
    switch (source) {
    case SynthesisJobState::Created:
        return target == SynthesisJobState::Validating || target == SynthesisJobState::Cancelled || target == SynthesisJobState::Failed;
    case SynthesisJobState::Validating:
        return target == SynthesisJobState::Queued || target == SynthesisJobState::Cancelled || target == SynthesisJobState::Failed;
    case SynthesisJobState::Queued:
        return target == SynthesisJobState::Running || target == SynthesisJobState::Cancelled || target == SynthesisJobState::Failed;
    case SynthesisJobState::Running:
        return target == SynthesisJobState::ValidatingArtifact || target == SynthesisJobState::Cancelled ||
               target == SynthesisJobState::Failed || target == SynthesisJobState::TimedOut;
    case SynthesisJobState::ValidatingArtifact:
        return target == SynthesisJobState::Succeeded || target == SynthesisJobState::Failed;
    default:
        return false;
    }
}
