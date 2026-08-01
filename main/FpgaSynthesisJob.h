#pragma once

#include <wx/string.h>

#include <vector>

enum class SynthesisJobState {
    Created,
    Validating,
    Queued,
    Running,
    ValidatingArtifact,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
};

struct SynthesisJobRequest {
    wxString projectPath;
    std::vector<wxString> sourceFiles;
    wxString topModule;
    wxString targetProfileId;
    wxString targetProfileVersion;
    wxString strategyId = "baseline";
    wxString strategyVersion = "1.0";
    wxString operatorName = "local";
};

struct SynthesisJobTransition {
    SynthesisJobState state = SynthesisJobState::Created;
    wxString timestamp;
    wxString operatorName;
    wxString reason;
    int exitCode = 0;
};

struct SynthesisJob {
    wxString id;
    wxString retryOf;
    SynthesisJobRequest request;
    SynthesisJobState state = SynthesisJobState::Created;
    wxString createdAt;
    wxString updatedAt;
    int exitCode = 0;
    std::vector<SynthesisJobTransition> transitions;
};

struct SynthesisJobPaths {
    wxString root;
    wxString inputs;
    wxString scripts;
    wxString logs;
    wxString artifacts;
    wxString reports;
    wxString manifest;
};

wxString ToString(SynthesisJobState state);
bool ParseSynthesisJobState(const wxString& value, SynthesisJobState& state);
bool IsTerminalSynthesisJobState(SynthesisJobState state);

class FpgaSynthesisJobService {
public:
    bool Create(const SynthesisJobRequest& request, SynthesisJob& job, wxString& errorMessage) const;
    bool Load(const wxString& projectPath, const wxString& jobId, SynthesisJob& job,
              wxString& errorMessage) const;
    bool List(const wxString& projectPath, std::vector<SynthesisJob>& jobs,
              wxString& errorMessage) const;
    bool Transition(const wxString& projectPath, const wxString& jobId, SynthesisJobState targetState,
                    const wxString& reason, int exitCode, wxString& errorMessage) const;
    bool Cancel(const wxString& projectPath, const wxString& jobId, const wxString& reason,
                wxString& errorMessage) const;
    bool Retry(const wxString& projectPath, const wxString& jobId, SynthesisJob& retryJob,
               wxString& errorMessage) const;

    static SynthesisJobPaths GetPaths(const wxString& projectPath, const wxString& jobId);

private:
    static bool IsLegalTransition(SynthesisJobState source, SynthesisJobState target);
};
