#pragma once

#include <functional>

#include <wx/frame.h>

#include "FpgaTheme.h"   // T-140：适配层 + 主题重放登记表（含 ui/UiTheme.h）

class FpgaSynthesisJobsPanel;
class NextpnrJobsPanel;
class wxNotebook;
class wxPanel;
class wxStaticText;
class wxButton;
class wxTextCtrl;
namespace eda { class IJobService; }

enum class FpgaToolPage {
    Yosys,
    Nextpnr,
    Programmer,
};

class FpgaToolWindow : public wxFrame {
public:
    explicit FpgaToolWindow(wxWindow* parent);

    void ShowPage(FpgaToolPage page);
    void SetProjectContext(const wxString& projectPath, const wxString& activeYosysJobId);
    void SetCoreJobService(eda::IJobService* service, const wxString& jobsRoot);
    void RefreshSynthesisJobs();

    void SetOpenFileHandler(std::function<void(const wxString&, long)> handler);
    void SetSynthesisStartHandler(std::function<void()> handler);
    void SetSynthesisCancelHandler(std::function<void()> handler);
    void SetSynthesisRetryHandler(std::function<void(const wxString&)> handler);
    void SetRouteStartHandler(std::function<void()> handler);
    void SetRouteCancelHandler(std::function<void()> handler);
    void SetRouteRetryHandler(std::function<void(const wxString&)> handler);
    void SetPackStartHandler(std::function<void()> handler);
    void SetProgramStartHandler(std::function<void(const wxString&)> handler);
    void SetRouteActiveJob(const wxString& jobId);
    void RefreshRouteJobs();
    void SetPackResult(const wxString& bitstreamPath, bool success, const wxString& message);

private:
    // T-140 / THE-030：主题刷新 —— 重放登记表 + 订阅句柄，均随本窗析构释放
    FpgaTheme::RepaintActions m_repaint;
    UiTheme::Subscription m_themeSub;

    wxString m_projectPath;
    wxString m_activeYosysJobId;
    wxString m_activeNextpnrJobId;
    wxNotebook* m_notebook = nullptr;
    FpgaSynthesisJobsPanel* m_synthesisJobsPanel = nullptr;
    NextpnrJobsPanel* m_routeJobsPanel = nullptr;

    wxStaticText* m_headerProjectLabel = nullptr;
    wxStaticText* m_nextpnrDeviceLabel = nullptr;
    wxStaticText* m_nextpnrFamilyLabel = nullptr;
    wxStaticText* m_nextpnrJsonLabel = nullptr;
    wxStaticText* m_nextpnrCstLabel = nullptr;
    wxButton* m_routeStartButton = nullptr;
    wxButton* m_packButton = nullptr;
    wxButton* m_programButton = nullptr;
    wxTextCtrl* m_bitstreamPathText = nullptr;
    wxStaticText* m_programBoardLabel = nullptr;
    wxStaticText* m_programStatusLabel = nullptr;
    wxString m_bitstreamPath;

    std::function<void()> m_synthesisStartHandler;
    std::function<void()> m_synthesisCancelHandler;
    std::function<void(const wxString&)> m_synthesisRetryHandler;
    std::function<void()> m_routeStartHandler;
    std::function<void()> m_routeCancelHandler;
    std::function<void(const wxString&)> m_routeRetryHandler;
    std::function<void()> m_packStartHandler;
    std::function<void(const wxString&)> m_programStartHandler;

    void ApplyTheme();
    void BuildUi();
    wxPanel* BuildHeader();
    wxPanel* BuildYosysPage();
    wxPanel* BuildNextpnrPage();
    wxPanel* BuildProgrammerPage();
    void UpdateProjectLabels();
    void UpdateNextpnrInfo();
    void UpdateProgrammerInfo();
    void OnClose(wxCloseEvent& event);
};
