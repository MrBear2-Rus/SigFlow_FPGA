#pragma once

#include <functional>

#include <wx/frame.h>

class FpgaSynthesisJobsPanel;
class NextpnrJobsPanel;
class wxNotebook;
class wxPanel;
class wxStaticText;
class wxButton;
class wxTextCtrl;

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
    void RefreshSynthesisJobs();

    void SetOpenFileHandler(std::function<void(const wxString&, long)> handler);
    void SetSynthesisStartHandler(std::function<void()> handler);
    void SetSynthesisCancelHandler(std::function<void()> handler);
    void SetSynthesisRetryHandler(std::function<void(const wxString&)> handler);
    void SetRouteStartHandler(std::function<void()> handler);
    void SetRouteCancelHandler(std::function<void()> handler);
    void SetRouteRetryHandler(std::function<void(const wxString&)> handler);
    void SetProgramStartHandler(std::function<void(const wxString&)> handler);
    void SetRouteActiveJob(const wxString& jobId);
    void RefreshRouteJobs();

private:
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
    std::function<void(const wxString&)> m_programStartHandler;

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
