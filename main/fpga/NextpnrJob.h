#pragma once

#include <wx/string.h>

#include <vector>

enum class NextpnrJobState {
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

struct NextpnrJobRequest {
    wxString projectPath;
    wxString topModule;
    wxString targetProfileId;
    wxString targetProfileVersion;
    wxString operatorName = "local";
    wxString retryOf;
    // Nextpnr-specific
    wxString jsonPath;
    wxString cstPath;
    wxString deviceName;
    wxString familyName;
};

struct NextpnrJobTransition {
    NextpnrJobState state = NextpnrJobState::Created;
    wxString timestamp;
    wxString operatorName;
    wxString reason;
    int exitCode = 0;
};

struct NextpnrJob {
    wxString id;
    wxString retryOf;
    NextpnrJobRequest request;
    NextpnrJobState state = NextpnrJobState::Created;
    wxString createdAt;
    wxString updatedAt;
    int exitCode = 0;
    std::vector<NextpnrJobTransition> transitions;
};

struct NextpnrJobPaths {
    wxString root;
    wxString inputs;
    wxString logs;
    wxString artifacts;
    wxString reports;
    wxString manifest;
};

wxString ToString(NextpnrJobState state);
bool ParseNextpnrJobState(const wxString& value, NextpnrJobState& state);
bool IsTerminalNextpnrJobState(NextpnrJobState state);

class NextpnrJobService {
public:
    bool Create(const NextpnrJobRequest& request, NextpnrJob& job,
                wxString& errorMessage) const;
    bool Load(const wxString& projectPath, const wxString& jobId, NextpnrJob& job,
              wxString& errorMessage) const;
    bool List(const wxString& projectPath, std::vector<NextpnrJob>& jobs,
              wxString& errorMessage) const;
    bool Transition(const wxString& projectPath, const wxString& jobId,
                    NextpnrJobState targetState, const wxString& reason,
                    int exitCode, wxString& errorMessage) const;
    bool Cancel(const wxString& projectPath, const wxString& jobId,
                const wxString& reason, wxString& errorMessage) const;
    bool Retry(const wxString& projectPath, const wxString& jobId,
               NextpnrJob& retryJob, wxString& errorMessage) const;
    bool RecoverStaleJobs(const wxString& projectPath, const wxString& activeJobId,
                           wxString& errorMessage) const;

    static NextpnrJobPaths GetPaths(const wxString& projectPath, const wxString& jobId);

private:
    static bool IsLegalTransition(NextpnrJobState source, NextpnrJobState target);
};
