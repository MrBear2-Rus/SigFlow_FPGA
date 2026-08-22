#include "NextpnrJobsPanel.h"

#include <algorithm>

#include <wx/datetime.h>
#include <wx/file.h>
#include <wx/log.h>
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

NextpnrJobsPanel::NextpnrJobsPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(FpgaTheme::kBackground);
    BuildUi();
    m_refreshTimer.SetOwner(this);
    Bind(wxEVT_TIMER, &NextpnrJobsPanel::OnRefreshTimer, this);
    m_refreshTimer.Start(2000);
}

void NextpnrJobsPanel::SetProjectContext(const wxString& projectPath,
                                          const wxString& activeJobId)
{
    if (m_projectPath != projectPath) m_selectedJobId.clear();
    m_projectPath = projectPath;
    m_activeJobId = activeJobId;
    RefreshJobs();
}

void NextpnrJobsPanel::SetOpenFileHandler(
    std::function<void(const wxString&, long)> handler)
{
    m_openFileHandler = std::move(handler);
}

void NextpnrJobsPanel::SetRetryHandler(std::function<void(const wxString&)> handler)
{
    m_retryHandler = std::move(handler);
}

void NextpnrJobsPanel::BuildUi()
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
    m_filter->SetSelection(0);
    m_filter->SetBackgroundColour(FpgaTheme::kPanelAlt);
    m_filter->SetForegroundColour(FpgaTheme::kText);
    toolbar->Add(m_filter, 0, wxALL, FromDIP(4));
    layout->Add(toolbar, 0, wxEXPAND);

    // ── 任务列表 + 报告区 ──
    m_jobList = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    m_jobList->SetBackgroundColour(FpgaTheme::kBackground);
    m_jobList->SetForegroundColour(FpgaTheme::kText);
    m_jobList->AppendColumn("Job", wxLIST_FORMAT_LEFT, FromDIP(240));
    m_jobList->AppendColumn("State", wxLIST_FORMAT_LEFT, FromDIP(110));
    m_jobList->AppendColumn("Updated", wxLIST_FORMAT_LEFT, FromDIP(140));
    layout->Add(m_jobList, 2, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));

    m_reportText = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxBORDER_SIMPLE);
    m_reportText->SetWrapMode(wxSTC_WRAP_WORD);
    m_reportText->SetReadOnly(true);
    m_reportText->SetBackgroundColour(FpgaTheme::kBackground);
    m_reportText->SetForegroundColour(FpgaTheme::kText);

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
    layout->Add(m_reportText, 3, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));

    // ── 底部操作 ──
    wxBoxSizer* actions = new wxBoxSizer(wxHORIZONTAL);
    m_openReportButton = new wxButton(this, wxID_ANY, "Open Report");
    m_openLogButton = new wxButton(this, wxID_ANY, "Open Log");
    m_retryButton = new wxButton(this, wxID_ANY, "Retry");
    FpgaTheme::StyleSecondaryButton(m_openReportButton);
    FpgaTheme::StyleSecondaryButton(m_openLogButton);
    FpgaTheme::StyleButton(m_retryButton, FpgaTheme::kBlue, *wxWHITE);
    actions->Add(m_openReportButton, 0, wxALL, FromDIP(4));
    actions->Add(m_openLogButton, 0, wxALL, FromDIP(4));
    actions->AddStretchSpacer();
    actions->Add(m_retryButton, 0, wxALL, FromDIP(4));
    layout->Add(actions, 0, wxEXPAND);
    SetSizer(layout);

    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshJobs(); });
    m_jobList->Bind(wxEVT_LIST_ITEM_SELECTED, &NextpnrJobsPanel::OnJobSelected, this);
    m_filter->Bind(wxEVT_CHOICE, &NextpnrJobsPanel::OnFilterChanged, this);
    m_openReportButton->Bind(wxEVT_BUTTON, &NextpnrJobsPanel::OnOpenReport, this);
    m_openLogButton->Bind(wxEVT_BUTTON, &NextpnrJobsPanel::OnOpenLog, this);
    m_retryButton->Bind(wxEVT_BUTTON, &NextpnrJobsPanel::OnRetry, this);
    UpdateActions();
}

void NextpnrJobsPanel::SortJobsNewestFirst()
{
    std::sort(m_jobs.begin(), m_jobs.end(),
        [](const NextpnrJob& left, const NextpnrJob& right) {
            if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
            return left.id > right.id;
        });
}

void NextpnrJobsPanel::RefreshJobs()
{
    // 保存当前滚动位置对应的 jobId，刷新后恢复滚动
    wxString topVisibleJobId;
    {
        const long topItem = m_jobList->GetTopItem();
        if (topItem >= 0 && static_cast<size_t>(topItem) < m_jobs.size()) {
            topVisibleJobId = m_jobs[topItem].id;
        }
    }

    m_jobs.clear();
    m_jobList->DeleteAllItems();
    SetReportText(wxEmptyString);

    if (m_projectPath.IsEmpty()) {
        SetReportText("Open a project to view place and route jobs.");
        UpdateActions();
        return;
    }

    NextpnrJobService service;
    wxString errorMessage;
    service.RecoverStaleJobs(m_projectPath, m_activeJobId, errorMessage);
    if (!service.List(m_projectPath, m_jobs, errorMessage)) {
        SetReportText("Unable to load nextpnr jobs.\n\n" + errorMessage);
        UpdateActions();
        return;
    }

    SortJobsNewestFirst();

    long selectedIndex = wxNOT_FOUND;
    for (size_t index = 0; index < m_jobs.size(); ++index) {
        const NextpnrJob& job = m_jobs[index];
        const long item = m_jobList->InsertItem(static_cast<long>(index), job.id);
        m_jobList->SetItem(item, 1, ToString(job.state));
        m_jobList->SetItem(item, 2, RelativeTime(job.updatedAt));
        wxListItem stateItem;
        stateItem.SetId(item);
        stateItem.SetColumn(1);
        stateItem.SetTextColour(StateColour(job.state));
        m_jobList->SetItem(stateItem);
        if (job.state == NextpnrJobState::Running) {
            m_jobList->SetItemBackgroundColour(item, FpgaTheme::kBlueDeep);
        }
        if (job.id == m_selectedJobId) selectedIndex = item;
    }

    if (m_jobs.empty()) {
        SetReportText("No nextpnr jobs have been created for this project.");
        UpdateActions();
        return;
    }
    if (selectedIndex == wxNOT_FOUND) selectedIndex = 0;
    m_jobList->SetItemState(selectedIndex, wxLIST_STATE_SELECTED,
                            wxLIST_STATE_SELECTED);
    // 恢复之前的滚动位置（找到刷新前顶部可见的 job）
    if (!topVisibleJobId.IsEmpty()) {
        for (long i = 0; i < static_cast<long>(m_jobs.size()); ++i) {
            if (m_jobs[i].id == topVisibleJobId) {
                m_jobList->EnsureVisible(i);
                break;
            }
        }
    }
    m_jobList->EnsureVisible(selectedIndex);
    SelectJob(selectedIndex);
}

void NextpnrJobsPanel::SelectJob(long itemIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= m_jobs.size()) return;
    m_selectedJobId = m_jobs[static_cast<size_t>(itemIndex)].id;
    RenderSelectedJob();
}

const NextpnrJob* NextpnrJobsPanel::GetSelectedJob() const
{
    const auto selected = std::find_if(m_jobs.begin(), m_jobs.end(),
        [this](const NextpnrJob& job) { return job.id == m_selectedJobId; });
    return selected == m_jobs.end() ? nullptr : &*selected;
}

void NextpnrJobsPanel::RenderSelectedJob()
{
    const NextpnrJob* job = GetSelectedJob();
    if (!job) {
        SetReportText(wxEmptyString);
        UpdateActions();
        return;
    }

    const NextpnrJobPaths paths = NextpnrJobService::GetPaths(m_projectPath, job->id);

    wxString reportContent;
    {
        wxLogNull suppressLog;
        wxFile reportFile(paths.reports + "\\route.analysis.json", wxFile::read);
        if (reportFile.IsOpened()) {
            reportFile.ReadAll(&reportContent);
        }
    }
    if (!reportContent.IsEmpty()) {
        SetReportText(reportContent);
    } else {
        SetReportText("Job: " + job->id + "\nState: " + ToString(job->state) +
            "\nDevice: " + job->request.deviceName +
            "\nFamily: " + job->request.familyName +
            "\n\nReport is not available yet.");
    }
    UpdateActions();
}

void NextpnrJobsPanel::SetReportText(const wxString& text)
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

void NextpnrJobsPanel::AppendReportLine(const wxString& line)
{
    const int start = m_reportText->GetLength();
    const int length = static_cast<int>(line.length() + 1);
    m_reportText->AppendText(line + "\n");
    m_reportText->StartStyling(start);
    m_reportText->SetStyling(length, ReportStyleForLine(line));
}

void NextpnrJobsPanel::UpdateActions()
{
    const NextpnrJob* job = GetSelectedJob();
    const bool hasJob = job != nullptr;
    const NextpnrJobPaths paths = hasJob
        ? NextpnrJobService::GetPaths(m_projectPath, job->id) : NextpnrJobPaths();
    m_openReportButton->Enable(hasJob && wxFileExists(paths.reports + "\\route.analysis.json"));
    m_openLogButton->Enable(hasJob && wxFileExists(paths.logs + "\\nextpnr.combined.log"));
    m_retryButton->Enable(hasJob && IsTerminalNextpnrJobState(job->state) &&
                          static_cast<bool>(m_retryHandler));
}

void NextpnrJobsPanel::ShowInfo(const wxString& message) const
{
    wxMessageBox(message, "FPGA Place and Route", wxOK | wxICON_INFORMATION,
                 const_cast<NextpnrJobsPanel*>(this));
}

bool NextpnrJobsPanel::HasRunningJob() const
{
    return std::any_of(m_jobs.begin(), m_jobs.end(),
        [](const NextpnrJob& job) { return job.state == NextpnrJobState::Running; });
}

wxString NextpnrJobsPanel::RelativeTime(const wxString& isoUtc)
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

wxColour NextpnrJobsPanel::StateColour(NextpnrJobState state)
{
    switch (state) {
    case NextpnrJobState::Succeeded: return FpgaTheme::kGreen;
    case NextpnrJobState::Failed: return FpgaTheme::kRed;
    case NextpnrJobState::Running:
    case NextpnrJobState::Validating:
    case NextpnrJobState::Queued: return FpgaTheme::kBlue;
    case NextpnrJobState::TimedOut: return FpgaTheme::kAmber;
    case NextpnrJobState::Cancelled: return FpgaTheme::kTextMuted;
    default: return FpgaTheme::kTextSecondary;
    }
}

void NextpnrJobsPanel::OnJobSelected(wxListEvent& event) { SelectJob(event.GetIndex()); }
void NextpnrJobsPanel::OnFilterChanged(wxCommandEvent&) { if (GetSelectedJob()) RenderSelectedJob(); }

void NextpnrJobsPanel::OnRefreshTimer(wxTimerEvent&)
{
    if (!IsShown()) return;
    if (m_activeJobId.IsEmpty() && !HasRunningJob()) return;
    RefreshJobs();
}

void NextpnrJobsPanel::OnOpenReport(wxCommandEvent&)
{
    const NextpnrJob* job = GetSelectedJob();
    if (!job) return;
    const NextpnrJobPaths paths = NextpnrJobService::GetPaths(m_projectPath, job->id);
    const wxString filePath = paths.reports + "\\route.analysis.json";
    if (!wxFileExists(filePath)) {
        ShowInfo("Report has not been generated yet.");
        return;
    }
    wxString content;
    {
        wxLogNull suppressLog;
        wxFile f(filePath, wxFile::read);
        if (f.IsOpened()) f.ReadAll(&content);
    }
    if (!content.IsEmpty()) {
        SetReportText(content);
    }
}

void NextpnrJobsPanel::OnOpenLog(wxCommandEvent&)
{
    const NextpnrJob* job = GetSelectedJob();
    if (!job) return;
    const NextpnrJobPaths paths = NextpnrJobService::GetPaths(m_projectPath, job->id);
    const wxString filePath = paths.logs + "\\nextpnr.combined.log";
    if (!wxFileExists(filePath)) {
        ShowInfo("Log has not been generated yet.");
        return;
    }
    wxString content;
    {
        wxLogNull suppressLog;
        wxFile f(filePath, wxFile::read);
        if (f.IsOpened()) f.ReadAll(&content);
    }
    if (!content.IsEmpty()) {
        SetReportText(content);
    }
}

void NextpnrJobsPanel::OnRetry(wxCommandEvent&)
{
    const NextpnrJob* job = GetSelectedJob();
    if (!job || !IsTerminalNextpnrJobState(job->state) || !m_retryHandler) {
        ShowInfo("Only a completed nextpnr job can be retried.");
        return;
    }
    m_retryHandler(job->id);
}
