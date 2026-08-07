#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include "../FpgaSynthesisJob.h"
#include "FpgaYosysLogParser.h"

class wxStyledTextCtrl;

class FpgaSynthesisJobsPanel : public wxPanel {
public:
    explicit FpgaSynthesisJobsPanel(wxWindow* parent);

    void SetProjectContext(const wxString& projectPath, const wxString& activeJobId);
    void RefreshJobs();
    void SetOpenFileHandler(std::function<void(const wxString&, long)> handler);
    void SetRetryHandler(std::function<void(const wxString&)> handler);

private:
    wxString m_projectPath;
    wxString m_activeJobId;
    wxString m_selectedJobId;
    std::vector<SynthesisJob> m_jobs;
    YosysLogRecord m_parsedLog;

    wxListCtrl* m_jobList = nullptr;
    wxChoice* m_filter = nullptr;
    wxStyledTextCtrl* m_reportText = nullptr;
    wxButton* m_openReportButton = nullptr;
    wxButton* m_openLogButton = nullptr;
    wxButton* m_openSourceButton = nullptr;
    wxButton* m_retryButton = nullptr;
    wxTimer m_refreshTimer;

    std::function<void(const wxString&, long)> m_openFileHandler;
    std::function<void(const wxString&)> m_retryHandler;

    void BuildUi();
    void SortJobsNewestFirst();
    void SelectJob(long itemIndex);
    void RenderSelectedJob();
    void RenderDiagnostics();
    void SetReportText(const wxString& text);
    void AppendReportLine(const wxString& line);
    const SynthesisJob* GetSelectedJob() const;
    void UpdateActions();
    void ShowInfo(const wxString& message) const;
    bool HasRunningJob() const;

    static wxString RelativeTime(const wxString& isoUtc);
    static wxColour StateColour(SynthesisJobState state);

    void OnJobSelected(wxListEvent& event);
    void OnFilterChanged(wxCommandEvent& event);
    void OnRefreshTimer(wxTimerEvent& event);
    void OnOpenReport(wxCommandEvent& event);
    void OnOpenLog(wxCommandEvent& event);
    void OnOpenSource(wxCommandEvent& event);
    void OnRetry(wxCommandEvent& event);
};
