#include "FpgaSynthesisJobsPanel.h"

#include <algorithm>

#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stc/stc.h>
#include <wx/tokenzr.h>

#include "FpgaTheme.h"

namespace {

constexpr int kStyleDefault = 0;
constexpr int kStyleMuted   = 1;
constexpr int kStyleInfo    = 2;
constexpr int kStyleWarn    = 3;
constexpr int kStyleError   = 4;
constexpr int kStyleHeader  = 5;

int ReportStyleForLine(const wxString& line)
{
    wxString trimmed = line;
    trimmed.Trim(true).Trim(false);
    if (trimmed.StartsWith("[ERROR]") || trimmed.StartsWith("ERROR") ||
        trimmed.Contains("Error:")) {
        return kStyleError;
    }
    if (trimmed.StartsWith("[WARNING]") || trimmed.StartsWith("WARNING") ||
        trimmed.Contains("Warning:")) {
        return kStyleWarn;
    }
    if (trimmed.StartsWith("[INFO]") || trimmed.StartsWith("INFO")) {
        return kStyleInfo;
    }
    if (trimmed.StartsWith("Job:") || trimmed.StartsWith("State:") ||
        trimmed.StartsWith("===") || trimmed.StartsWith("---") ||
        (trimmed.StartsWith("[") && trimmed.EndsWith("]"))) {
        return kStyleHeader;
    }
    return kStyleDefault;
}

} // namespace

FpgaSynthesisJobsPanel::FpgaSynthesisJobsPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(FpgaTheme::kBackground);
    BuildUi();
    m_refreshTimer.SetOwner(this);
    Bind(wxEVT_TIMER, &FpgaSynthesisJobsPanel::OnRefreshTimer, this);
    m_refreshTimer.Start(2000);
}

void FpgaSynthesisJobsPanel::SetProjectContext(const wxString& projectPath,
                                               const wxString& activeJobId)
{
    if (m_projectPath != projectPath) m_selectedJobId.clear();
    m_projectPath = projectPath;
    m_activeJobId = activeJobId;
}

void FpgaSynthesisJobsPanel::SetOpenFileHandler(
    std::function<void(const wxString&, long)> handler)
{
    m_openFileHandler = std::move(handler);
}

void FpgaSynthesisJobsPanel::SetRetryHandler(std::function<void(const wxString&)> handler)
{
    m_retryHandler = std::move(handler);
}

void FpgaSynthesisJobsPanel::BuildUi()
{
    wxBoxSizer* layout = new wxBoxSizer(wxVERTICAL);

    // ── 工具条 ──
    wxBoxSizer* toolbar = new wxBoxSizer(wxHORIZONTAL);
    wxButton* refresh = new wxButton(this, wxID_REFRESH, "Refresh");
    FpgaTheme::StyleSecondaryButton(refresh);
    toolbar->Add(refresh, 0, wxALL, FromDIP(4));
    toolbar->AddStretchSpacer();
    toolbar->Add(FpgaTheme::MakeLabel(this, "Display:", FpgaTheme::kTextSecondary),
                 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
    m_filter = new wxChoice(this, wxID_ANY);
    m_filter->Append("Summary");
    m_filter->Append("Errors");
    m_filter->Append("Warnings");
    m_filter->Append("All Diagnostics");
    m_filter->SetSelection(0);
    m_filter->SetBackgroundColour(FpgaTheme::kPanelAlt);
    m_filter->SetForegroundColour(FpgaTheme::kText);
    toolbar->Add(m_filter, 0, wxALL, FromDIP(4));
    layout->Add(toolbar, 0, wxEXPAND);

    // ── 任务列表 + 报告区 ──
    wxSplitterWindow* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxSP_LIVE_UPDATE | wxBORDER_NONE);
    splitter->SetBackgroundColour(FpgaTheme::kBackground);

    m_jobList = new wxListCtrl(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    m_jobList->SetBackgroundColour(FpgaTheme::kBackground);
    m_jobList->SetForegroundColour(FpgaTheme::kText);
    m_jobList->AppendColumn("Job", wxLIST_FORMAT_LEFT, FromDIP(210));
    m_jobList->AppendColumn("State", wxLIST_FORMAT_LEFT, FromDIP(95));
    m_jobList->AppendColumn("Strategy", wxLIST_FORMAT_LEFT, FromDIP(115));
    m_jobList->AppendColumn("Updated", wxLIST_FORMAT_LEFT, FromDIP(140));

    m_reportText = new wxStyledTextCtrl(splitter, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxBORDER_SIMPLE);
    m_reportText->SetWrapMode(wxSTC_WRAP_WORD);
    m_reportText->SetUseHorizontalScrollBar(true);
    m_reportText->SetMarginWidth(0, 0);
    m_reportText->SetReadOnly(true);
    m_reportText->SetBackgroundColour(FpgaTheme::kBackground);

    const wxFont reportFont(FromDIP(9), wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL,
                            wxFONTWEIGHT_NORMAL);
    m_reportText->StyleClearAll();
    m_reportText->StyleSetFont(kStyleDefault, reportFont);
    for (int style = kStyleDefault; style <= kStyleHeader; ++style) {
        m_reportText->StyleSetBackground(style, FpgaTheme::kBackground);
        m_reportText->StyleSetFont(style, reportFont);
    }
    m_reportText->StyleSetForeground(kStyleDefault, FpgaTheme::kText);
    m_reportText->StyleSetForeground(kStyleMuted, FpgaTheme::kTextMuted);
    m_reportText->StyleSetForeground(kStyleInfo, FpgaTheme::kBlue);
    m_reportText->StyleSetForeground(kStyleWarn, FpgaTheme::kAmber);
    m_reportText->StyleSetForeground(kStyleError, FpgaTheme::kRed);
    m_reportText->StyleSetForeground(kStyleHeader, FpgaTheme::kTextSecondary);
    m_reportText->SetCaretForeground(FpgaTheme::kText);

    splitter->SplitHorizontally(m_jobList, m_reportText, FromDIP(175));
    splitter->SetMinimumPaneSize(FromDIP(80));
    layout->Add(splitter, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));

    // ── 底部操作 ──
    wxBoxSizer* actions = new wxBoxSizer(wxHORIZONTAL);
    m_openReportButton = new wxButton(this, wxID_ANY, "Open Report");
    m_openLogButton = new wxButton(this, wxID_ANY, "Open Log");
    m_openSourceButton = new wxButton(this, wxID_ANY, "Open Error Source");
    m_retryButton = new wxButton(this, wxID_ANY, "Retry");
    FpgaTheme::StyleSecondaryButton(m_openReportButton);
    FpgaTheme::StyleSecondaryButton(m_openLogButton);
    FpgaTheme::StyleSecondaryButton(m_openSourceButton);
    FpgaTheme::StyleButton(m_retryButton, FpgaTheme::kBlue, *wxWHITE);
    actions->Add(m_openReportButton, 0, wxALL, FromDIP(4));
    actions->Add(m_openLogButton, 0, wxALL, FromDIP(4));
    actions->Add(m_openSourceButton, 0, wxALL, FromDIP(4));
    actions->AddStretchSpacer();
    actions->Add(m_retryButton, 0, wxALL, FromDIP(4));
    layout->Add(actions, 0, wxEXPAND);
    SetSizer(layout);

    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshJobs(); });
    m_jobList->Bind(wxEVT_LIST_ITEM_SELECTED, &FpgaSynthesisJobsPanel::OnJobSelected, this);
    m_filter->Bind(wxEVT_CHOICE, &FpgaSynthesisJobsPanel::OnFilterChanged, this);
    m_openReportButton->Bind(wxEVT_BUTTON, &FpgaSynthesisJobsPanel::OnOpenReport, this);
    m_openLogButton->Bind(wxEVT_BUTTON, &FpgaSynthesisJobsPanel::OnOpenLog, this);
    m_openSourceButton->Bind(wxEVT_BUTTON, &FpgaSynthesisJobsPanel::OnOpenSource, this);
    m_retryButton->Bind(wxEVT_BUTTON, &FpgaSynthesisJobsPanel::OnRetry, this);
    UpdateActions();
}

void FpgaSynthesisJobsPanel::SortJobsNewestFirst()
{
    std::sort(m_jobs.begin(), m_jobs.end(),
        [](const SynthesisJob& left, const SynthesisJob& right) {
            if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
            return left.id > right.id;
        });
}

void FpgaSynthesisJobsPanel::RefreshJobs()
{
    m_jobs.clear();
    m_jobList->DeleteAllItems();
    SetReportText(wxEmptyString);
    m_parsedLog = YosysLogRecord();

    if (m_projectPath.IsEmpty()) {
        SetReportText("Open a project to view synthesis jobs.");
        UpdateActions();
        return;
    }

    FpgaSynthesisJobService service;
    wxString errorMessage;
    service.RecoverStaleJobs(m_projectPath, m_activeJobId, errorMessage);
    if (!service.List(m_projectPath, m_jobs, errorMessage)) {
        SetReportText("Unable to load synthesis jobs.\n\n" + errorMessage);
        UpdateActions();
        return;
    }

    // 最新在前：按创建时间倒序排列
    SortJobsNewestFirst();

    long selectedIndex = wxNOT_FOUND;
    for (size_t index = 0; index < m_jobs.size(); ++index) {
        const SynthesisJob& job = m_jobs[index];
        const long item = m_jobList->InsertItem(static_cast<long>(index), job.id);
        m_jobList->SetItem(item, 1, ToString(job.state));
        m_jobList->SetItem(item, 2, job.request.strategyId);
        m_jobList->SetItem(item, 3, RelativeTime(job.updatedAt));
        wxListItem stateItem;
        stateItem.SetId(item);
        stateItem.SetColumn(1);
        stateItem.SetTextColour(StateColour(job.state));
        m_jobList->SetItem(stateItem);
        if (job.state == SynthesisJobState::Running) {
            m_jobList->SetItemBackgroundColour(item, FpgaTheme::kBlueDeep);
        }
        if (job.id == m_selectedJobId) selectedIndex = item;
    }

    if (m_jobs.empty()) {
        SetReportText("No synthesis jobs have been created for this project.");
        UpdateActions();
        return;
    }
    if (selectedIndex == wxNOT_FOUND) selectedIndex = 0;
    m_jobList->SetItemState(selectedIndex, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                            wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    SelectJob(selectedIndex);
}

void FpgaSynthesisJobsPanel::SelectJob(long itemIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= m_jobs.size()) return;
    m_selectedJobId = m_jobs[static_cast<size_t>(itemIndex)].id;
    RenderSelectedJob();
}

const SynthesisJob* FpgaSynthesisJobsPanel::GetSelectedJob() const
{
    const auto selected = std::find_if(m_jobs.begin(), m_jobs.end(),
        [this](const SynthesisJob& job) { return job.id == m_selectedJobId; });
    return selected == m_jobs.end() ? nullptr : &*selected;
}

void FpgaSynthesisJobsPanel::RenderSelectedJob()
{
    const SynthesisJob* job = GetSelectedJob();
    if (!job) {
        SetReportText(wxEmptyString);
        UpdateActions();
        return;
    }

    FpgaSynthesisJobService service;
    wxString errorMessage;
    wxString jsonReport;
    wxString summaryReport;
    if (!service.GetSynthesisReport(m_projectPath, job->id, jsonReport, summaryReport, errorMessage)) {
        summaryReport = "Job: " + job->id + "\nState: " + ToString(job->state) +
            "\n\nStructured report is not available yet.\n" + errorMessage;
    }

    const SynthesisJobPaths paths = FpgaSynthesisJobService::GetPaths(m_projectPath, job->id);
    wxString combinedLog;
    wxFile logFile(paths.logs + "\\yosys.combined.log", wxFile::read);
    if (logFile.IsOpened()) logFile.ReadAll(&combinedLog);
    m_parsedLog = FpgaYosysLogParser().Parse(combinedLog);
    if (m_filter->GetSelection() == 0) {
        SetReportText(summaryReport);
    } else {
        RenderDiagnostics();
    }
    UpdateActions();
}

void FpgaSynthesisJobsPanel::RenderDiagnostics()
{
    wxString diagnostics;
    const int selection = m_filter->GetSelection();
    const YosysLogSeverity requestedSeverity = selection == 1
        ? YosysLogSeverity::Error : YosysLogSeverity::Warning;
    for (const YosysLogEvent& event : m_parsedLog.events) {
        if (selection != 3 && event.severity != requestedSeverity) continue;
        diagnostics << "[" << ToString(event.severity) << "] " << event.ruleId
                    << " (" << event.stage << ", log " << event.lineNumber << ")\n"
                    << event.rawLine << "\n" << event.suggestion << "\n\n";
    }
    SetReportText(diagnostics.IsEmpty() ? wxString("No matching diagnostics.") : diagnostics);
}

void FpgaSynthesisJobsPanel::SetReportText(const wxString& text)
{
    m_reportText->SetReadOnly(false);
    m_reportText->ClearAll();
    wxStringTokenizer tokenizer(text, "\n", wxTOKEN_RET_EMPTY_ALL);
    while (tokenizer.HasMoreTokens()) {
        AppendReportLine(tokenizer.GetNextToken());
    }
    m_reportText->SetReadOnly(true);
    m_reportText->GotoPos(0);
}

void FpgaSynthesisJobsPanel::AppendReportLine(const wxString& line)
{
    const int start = m_reportText->GetLength();
    const int length = static_cast<int>(line.length() + 1); // +1 for '\n'
    m_reportText->AppendText(line + "\n");
    m_reportText->StartStyling(start);
    m_reportText->SetStyling(length, ReportStyleForLine(line));
}

void FpgaSynthesisJobsPanel::UpdateActions()
{
    const SynthesisJob* job = GetSelectedJob();
    const bool hasJob = job != nullptr;
    const SynthesisJobPaths paths = hasJob
        ? FpgaSynthesisJobService::GetPaths(m_projectPath, job->id) : SynthesisJobPaths();
    m_openReportButton->Enable(hasJob && wxFileExists(paths.reports + "\\synthesis.summary.md"));
    m_openLogButton->Enable(hasJob && wxFileExists(paths.logs + "\\yosys.combined.log"));
    const auto source = std::find_if(m_parsedLog.events.begin(), m_parsedLog.events.end(),
        [](const YosysLogEvent& event) {
            return event.severity == YosysLogSeverity::Error && !event.sourceFile.IsEmpty();
        });
    m_openSourceButton->Enable(source != m_parsedLog.events.end() && static_cast<bool>(m_openFileHandler));
    m_retryButton->Enable(hasJob && IsTerminalSynthesisJobState(job->state) &&
                          static_cast<bool>(m_retryHandler));
}

void FpgaSynthesisJobsPanel::ShowInfo(const wxString& message) const
{
    wxMessageBox(message, "FPGA Synthesis", wxOK | wxICON_INFORMATION,
                 const_cast<FpgaSynthesisJobsPanel*>(this));
}

bool FpgaSynthesisJobsPanel::HasRunningJob() const
{
    return std::any_of(m_jobs.begin(), m_jobs.end(),
        [](const SynthesisJob& job) { return job.state == SynthesisJobState::Running; });
}

wxString FpgaSynthesisJobsPanel::RelativeTime(const wxString& isoUtc)
{
    wxString normalized = isoUtc;
    if (normalized.EndsWith("Z")) normalized.RemoveLast();
    wxDateTime utc;
    if (!utc.ParseISOCombined(normalized)) return isoUtc;

    wxTimeSpan span = wxDateTime::UNow() - utc;
    if (span.IsNegative()) span = wxTimeSpan::Seconds(0);
    const long seconds = span.GetSeconds().ToLong();
    if (seconds < 60) return wxString::Format("%lds ago", seconds);
    const long minutes = seconds / 60;
    if (minutes < 60) return wxString::Format("%ldm ago", minutes);
    const long hours = minutes / 60;
    if (hours < 24) return wxString::Format("%ldh ago", hours);
    const long days = hours / 24;
    if (days < 7) return wxString::Format("%ldd ago", days);
    return utc.FormatISOCombined(' ');
}

wxColour FpgaSynthesisJobsPanel::StateColour(SynthesisJobState state)
{
    switch (state) {
    case SynthesisJobState::Succeeded: return FpgaTheme::kGreen;
    case SynthesisJobState::Failed: return FpgaTheme::kRed;
    case SynthesisJobState::Running:
    case SynthesisJobState::Validating:
    case SynthesisJobState::Queued: return FpgaTheme::kBlue;
    case SynthesisJobState::TimedOut: return FpgaTheme::kAmber;
    case SynthesisJobState::Cancelled: return FpgaTheme::kTextMuted;
    default: return FpgaTheme::kTextSecondary;
    }
}

void FpgaSynthesisJobsPanel::OnJobSelected(wxListEvent& event)
{
    SelectJob(event.GetIndex());
}

void FpgaSynthesisJobsPanel::OnFilterChanged(wxCommandEvent&)
{
    if (GetSelectedJob()) RenderSelectedJob();
}

void FpgaSynthesisJobsPanel::OnRefreshTimer(wxTimerEvent&)
{
    if (!IsShown()) return;
    if (m_activeJobId.IsEmpty() && !HasRunningJob()) return;
    RefreshJobs();
}

void FpgaSynthesisJobsPanel::OnOpenReport(wxCommandEvent&)
{
    const SynthesisJob* job = GetSelectedJob();
    if (!job || !m_openFileHandler) return;
    const SynthesisJobPaths paths = FpgaSynthesisJobService::GetPaths(m_projectPath, job->id);
    m_openFileHandler(paths.reports + "\\synthesis.summary.md", 0);
}

void FpgaSynthesisJobsPanel::OnOpenLog(wxCommandEvent&)
{
    const SynthesisJob* job = GetSelectedJob();
    if (!job || !m_openFileHandler) return;
    const SynthesisJobPaths paths = FpgaSynthesisJobService::GetPaths(m_projectPath, job->id);
    m_openFileHandler(paths.logs + "\\yosys.combined.log", 0);
}

void FpgaSynthesisJobsPanel::OnOpenSource(wxCommandEvent&)
{
    if (!m_openFileHandler) return;
    const auto source = std::find_if(m_parsedLog.events.begin(), m_parsedLog.events.end(),
        [](const YosysLogEvent& event) {
            return event.severity == YosysLogSeverity::Error && !event.sourceFile.IsEmpty();
        });
    if (source == m_parsedLog.events.end()) {
        ShowInfo("No source location is available in this report.");
        return;
    }
    m_openFileHandler(source->sourceFile, source->sourceLine);
}

void FpgaSynthesisJobsPanel::OnRetry(wxCommandEvent&)
{
    const SynthesisJob* job = GetSelectedJob();
    if (!job || !IsTerminalSynthesisJobState(job->state) || !m_retryHandler) {
        ShowInfo("Only a completed synthesis job can be retried.");
        return;
    }
    m_retryHandler(job->id);
}
