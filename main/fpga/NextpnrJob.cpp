#include "NextpnrJob.h"

#include <json/json.h>

#include <wx/dir.h>
#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <atomic>
#include <memory>

namespace {

std::atomic<unsigned long> g_nextpnrJobSequence{0};

wxString NowUtc()
{
    return wxDateTime::UNow().FormatISOCombined('T') + "Z";
}

bool IsSafeJobId(const wxString& value)
{
    if (value.IsEmpty()) return false;
    for (const wxChar c : value) {
        if (!(wxIsalnum(c) || c == '-' || c == '_')) return false;
    }
    return true;
}

bool EnsureDirectory(const wxString& path)
{
    return wxDirExists(path) || wxFileName::Mkdir(path, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

bool WriteJsonAtomically(const wxString& path, const Json::Value& value, wxString& errorMessage)
{
    const wxString tmp = path + ".tmp";
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const wxString content = wxString::FromUTF8(Json::writeString(writer, value)) + "\n";
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    wxFile file(tmp, wxFile::write);
    if (!file.IsOpened() || !utf8.data() ||
        file.Write(utf8.data(), utf8.length()) != static_cast<wxFileOffset>(utf8.length())) {
        errorMessage = "Unable to write manifest: " + path;
        file.Close();
        wxRemoveFile(tmp);
        return false;
    }
    file.Close();
    if (!wxRenameFile(tmp, path, true)) {
        errorMessage = "Unable to replace manifest: " + path;
        wxRemoveFile(tmp);
        return false;
    }
    return true;
}

Json::Value ToJson(const NextpnrJob& job)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = "1.0";
    root["job_id"] = job.id.ToStdString();
    root["retry_of"] = job.retryOf.ToStdString();
    root["state"] = ToString(job.state).ToStdString();
    root["created_at"] = job.createdAt.ToStdString();
    root["updated_at"] = job.updatedAt.ToStdString();
    root["exit_code"] = job.exitCode;

    Json::Value req(Json::objectValue);
    req["project_path"] = job.request.projectPath.ToStdString();
    req["top_module"] = job.request.topModule.ToStdString();
    req["target_profile"] = job.request.targetProfileId.ToStdString();
    req["target_profile_version"] = job.request.targetProfileVersion.ToStdString();
    req["operator"] = job.request.operatorName.ToStdString();
    req["retry_of"] = job.request.retryOf.ToStdString();
    req["json_path"] = job.request.jsonPath.ToStdString();
    req["cst_path"] = job.request.cstPath.ToStdString();
    req["device_name"] = job.request.deviceName.ToStdString();
    req["family_name"] = job.request.familyName.ToStdString();
    root["request"] = req;

    Json::Value transitions(Json::arrayValue);
    for (const auto& t : job.transitions) {
        Json::Value item(Json::objectValue);
        item["state"] = ToString(t.state).ToStdString();
        item["timestamp"] = t.timestamp.ToStdString();
        item["operator"] = t.operatorName.ToStdString();
        item["reason"] = t.reason.ToStdString();
        item["exit_code"] = t.exitCode;
        transitions.append(item);
    }
    root["transitions"] = transitions;
    return root;
}

bool FromJson(const Json::Value& root, NextpnrJob& job, wxString& errorMessage)
{
    if (!root.isObject() || !root["job_id"].isString() || !root["request"].isObject() ||
        !root["state"].isString()) {
        errorMessage = "Invalid nextpnr job manifest.";
        return false;
    }
    job = NextpnrJob();
    job.id = wxString::FromUTF8(root["job_id"].asString());
    if (!IsSafeJobId(job.id) ||
        !ParseNextpnrJobState(wxString::FromUTF8(root["state"].asString()), job.state)) {
        errorMessage = "Invalid job ID or state.";
        return false;
    }
    job.retryOf = wxString::FromUTF8(root["retry_of"].asString());
    job.createdAt = wxString::FromUTF8(root["created_at"].asString());
    job.updatedAt = wxString::FromUTF8(root["updated_at"].asString());
    job.exitCode = root["exit_code"].asInt();

    const Json::Value& req = root["request"];
    job.request.projectPath = wxString::FromUTF8(req["project_path"].asString());
    job.request.topModule = wxString::FromUTF8(req["top_module"].asString());
    job.request.targetProfileId = wxString::FromUTF8(req["target_profile"].asString());
    job.request.targetProfileVersion = wxString::FromUTF8(req["target_profile_version"].asString());
    job.request.operatorName = wxString::FromUTF8(req["operator"].asString());
    job.request.retryOf = wxString::FromUTF8(req["retry_of"].asString());
    if (req["json_path"].isString())
        job.request.jsonPath = wxString::FromUTF8(req["json_path"].asString());
    if (req["cst_path"].isString())
        job.request.cstPath = wxString::FromUTF8(req["cst_path"].asString());
    if (req["device_name"].isString())
        job.request.deviceName = wxString::FromUTF8(req["device_name"].asString());
    if (req["family_name"].isString())
        job.request.familyName = wxString::FromUTF8(req["family_name"].asString());

    for (const Json::Value& item : root["transitions"]) {
        NextpnrJobTransition t;
        if (!item.isObject() ||
            !ParseNextpnrJobState(wxString::FromUTF8(item["state"].asString()), t.state)) {
            errorMessage = "Invalid transition.";
            return false;
        }
        t.timestamp = wxString::FromUTF8(item["timestamp"].asString());
        t.operatorName = wxString::FromUTF8(item["operator"].asString());
        t.reason = wxString::FromUTF8(item["reason"].asString());
        t.exitCode = item["exit_code"].asInt();
        job.transitions.push_back(t);
    }
    return true;
}

bool ReadManifest(const wxString& path, NextpnrJob& job, wxString& errorMessage)
{
    wxFile file(path, wxFile::read);
    wxString content;
    if (!file.IsOpened() || !file.ReadAll(&content)) {
        errorMessage = "Unable to read manifest: " + path;
        return false;
    }
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!utf8.data() || !reader->parse(utf8.data(), utf8.data() + utf8.length(), &root, &errors)) {
        errorMessage = "Unable to parse manifest: " + wxString::FromUTF8(errors);
        return false;
    }
    return FromJson(root, job, errorMessage);
}

bool WriteManifest(const NextpnrJob& job, wxString& errorMessage)
{
    return WriteJsonAtomically(
        NextpnrJobService::GetPaths(job.request.projectPath, job.id).manifest,
        ToJson(job), errorMessage);
}

} // namespace

wxString ToString(NextpnrJobState state)
{
    switch (state) {
    case NextpnrJobState::Created:           return "Created";
    case NextpnrJobState::Validating:        return "Validating";
    case NextpnrJobState::Queued:            return "Queued";
    case NextpnrJobState::Running:           return "Running";
    case NextpnrJobState::ValidatingArtifact: return "ValidatingArtifact";
    case NextpnrJobState::Succeeded:         return "Succeeded";
    case NextpnrJobState::Failed:            return "Failed";
    case NextpnrJobState::Cancelled:         return "Cancelled";
    case NextpnrJobState::TimedOut:          return "TimedOut";
    }
    return "Unknown";
}

bool ParseNextpnrJobState(const wxString& value, NextpnrJobState& state)
{
    for (NextpnrJobState c : { NextpnrJobState::Created, NextpnrJobState::Validating,
            NextpnrJobState::Queued, NextpnrJobState::Running, NextpnrJobState::ValidatingArtifact,
            NextpnrJobState::Succeeded, NextpnrJobState::Failed, NextpnrJobState::Cancelled,
            NextpnrJobState::TimedOut }) {
        if (value == ToString(c)) { state = c; return true; }
    }
    return false;
}

bool IsTerminalNextpnrJobState(NextpnrJobState state)
{
    return state == NextpnrJobState::Succeeded || state == NextpnrJobState::Failed ||
           state == NextpnrJobState::Cancelled || state == NextpnrJobState::TimedOut;
}

NextpnrJobPaths NextpnrJobService::GetPaths(const wxString& projectPath, const wxString& jobId)
{
    NextpnrJobPaths paths;
    paths.root = projectPath + "\\.sigflow\\fpga\\runs-nextpnr\\" + jobId;
    paths.inputs = paths.root + "\\inputs";
    paths.logs = paths.root + "\\logs";
    paths.artifacts = paths.root + "\\artifacts";
    paths.reports = paths.root + "\\reports";
    paths.manifest = paths.root + "\\manifest.json";
    return paths;
}

bool NextpnrJobService::Create(const NextpnrJobRequest& request, NextpnrJob& job,
                                wxString& errorMessage) const
{
    errorMessage.clear();
    if (request.projectPath.IsEmpty() || request.topModule.IsEmpty() ||
        !wxDirExists(request.projectPath)) {
        errorMessage = "A project directory and top module are required.";
        return false;
    }
    if (request.jsonPath.IsEmpty() || !wxFileExists(request.jsonPath)) {
        errorMessage = "Yosys JSON netlist is required: " + request.jsonPath;
        return false;
    }

    job = NextpnrJob();
    job.request = request;
    job.retryOf = request.retryOf;
    job.id = wxDateTime::UNow().Format("%Y%m%dT%H%M%S") + "-" +
        wxString::Format("%06lu", ++g_nextpnrJobSequence);
    job.createdAt = NowUtc();
    job.updatedAt = job.createdAt;
    job.transitions.push_back({ NextpnrJobState::Created, job.createdAt, request.operatorName,
                                "Job created.", 0 });

    const NextpnrJobPaths paths = GetPaths(request.projectPath, job.id);
    for (const wxString& dir : { paths.inputs, paths.logs, paths.artifacts, paths.reports }) {
        if (!EnsureDirectory(dir)) {
            errorMessage = "Unable to create job directory: " + dir;
            return false;
        }
    }
    return WriteManifest(job, errorMessage);
}

bool NextpnrJobService::Load(const wxString& projectPath, const wxString& jobId,
                              NextpnrJob& job, wxString& errorMessage) const
{
    errorMessage.clear();
    if (!IsSafeJobId(jobId)) { errorMessage = "Invalid job ID."; return false; }
    return ReadManifest(GetPaths(projectPath, jobId).manifest, job, errorMessage);
}

bool NextpnrJobService::Transition(const wxString& projectPath, const wxString& jobId,
                                    NextpnrJobState targetState, const wxString& reason,
                                    int exitCode, wxString& errorMessage) const
{
    errorMessage.clear();
    NextpnrJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (!IsLegalTransition(job.state, targetState)) {
        errorMessage = "Illegal transition: " + ToString(job.state) + " -> " + ToString(targetState);
        return false;
    }
    job.state = targetState;
    job.updatedAt = NowUtc();
    job.exitCode = exitCode;
    job.transitions.push_back({ targetState, job.updatedAt, job.request.operatorName, reason, exitCode });
    return WriteManifest(job, errorMessage);
}

bool NextpnrJobService::Cancel(const wxString& projectPath, const wxString& jobId,
                                const wxString& reason, wxString& errorMessage) const
{
    errorMessage.clear();
    NextpnrJob job;
    if (!Load(projectPath, jobId, job, errorMessage)) return false;
    if (job.state != NextpnrJobState::Running) {
        errorMessage = "Only a running job can be cancelled.";
        return false;
    }
    return Transition(projectPath, jobId, NextpnrJobState::Cancelled, reason, job.exitCode, errorMessage);
}

bool NextpnrJobService::Retry(const wxString& projectPath, const wxString& jobId,
                               NextpnrJob& retryJob, wxString& errorMessage) const
{
    errorMessage.clear();
    NextpnrJob original;
    if (!Load(projectPath, jobId, original, errorMessage)) return false;
    if (!IsTerminalNextpnrJobState(original.state)) {
        errorMessage = "Only terminal jobs can be retried.";
        return false;
    }
    if (!Create(original.request, retryJob, errorMessage)) return false;
    retryJob.retryOf = original.id;
    retryJob.request.retryOf = original.id;
    return WriteManifest(retryJob, errorMessage);
}

bool NextpnrJobService::List(const wxString& projectPath, std::vector<NextpnrJob>& jobs,
                              wxString& errorMessage) const
{
    errorMessage.clear();
    jobs.clear();
    const wxString runsDir = projectPath + "\\.sigflow\\fpga\\runs-nextpnr";
    if (!wxDirExists(runsDir)) return true;
    wxDir dir(runsDir);
    wxString name;
    bool found = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
    while (found) {
        NextpnrJob job;
        wxString loadError;
        if (Load(projectPath, name, job, loadError)) jobs.push_back(job);
        else if (errorMessage.IsEmpty()) errorMessage = loadError;
        found = dir.GetNext(&name);
    }
    return errorMessage.IsEmpty();
}

bool NextpnrJobService::RecoverStaleJobs(const wxString& projectPath, const wxString& activeJobId,
                                          wxString& errorMessage) const
{
    errorMessage.clear();
    std::vector<NextpnrJob> jobs;
    if (!List(projectPath, jobs, errorMessage)) return false;
    for (const auto& job : jobs) {
        if (job.id == activeJobId || job.state != NextpnrJobState::Running) continue;
        wxString ignore;
        if (!Transition(projectPath, job.id, NextpnrJobState::Failed,
                        "Recovered stale running job.", -1, ignore) &&
            errorMessage.IsEmpty()) {
            errorMessage = ignore;
        }
    }
    return errorMessage.IsEmpty();
}

bool NextpnrJobService::IsLegalTransition(NextpnrJobState src, NextpnrJobState tgt)
{
    switch (src) {
    case NextpnrJobState::Created:
        return tgt == NextpnrJobState::Validating || tgt == NextpnrJobState::Cancelled || tgt == NextpnrJobState::Failed;
    case NextpnrJobState::Validating:
        return tgt == NextpnrJobState::Queued || tgt == NextpnrJobState::Cancelled || tgt == NextpnrJobState::Failed;
    case NextpnrJobState::Queued:
        return tgt == NextpnrJobState::Running || tgt == NextpnrJobState::Cancelled || tgt == NextpnrJobState::Failed;
    case NextpnrJobState::Running:
        return tgt == NextpnrJobState::ValidatingArtifact || tgt == NextpnrJobState::Cancelled ||
               tgt == NextpnrJobState::Failed || tgt == NextpnrJobState::TimedOut;
    case NextpnrJobState::ValidatingArtifact:
        return tgt == NextpnrJobState::Succeeded || tgt == NextpnrJobState::Failed;
    default:
        return false;
    }
}
