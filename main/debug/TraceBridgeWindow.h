#pragma once

#include "DebugAcquisition.h"
#include "CaptureRunService.h"
#include "DebugBehaviorSummary.h"
#include "DebugContract.h"
#include "WaveformAligner.h"
#include "WaveformComparator.h"
#include "../wave/TraceViewPanel.h"

#include "ui/UiTheme.h"   // T-140 / THE-030：子窗主题刷新

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wx/dnd.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/spinctrl.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxColour;
class wxRadioButton;
class wxSpinCtrl;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

struct TraceBridgeCaptureRequest {
    std::string projectPath;
    std::string contractPath;
    std::string serialPort;
    sigflow::debug::DebugContract contract;
    sigflow::debug::DebugAcquisitionOptions options;
    // Keep the cancellation flag alive through detached host capture workers.
    std::shared_ptr<std::atomic<bool>> abortOwner;
    // Shared control plane for the host-side repeated capture scheduler.
    // The GUI may request pause/resume without owning the worker thread.
    std::shared_ptr<sigflow::debug::CaptureRunService> captureRunService;

    // Host-side capture plan. Defaults preserve the existing single-shot UX;
    // the fields are intentionally carried in the request so a future plan
    // editor can opt into bounded repeated capture without changing the
    // transport or acquisition API.
    std::size_t captureTargetCount = 1;
    std::size_t captureMaxAttempts = 0;
    std::size_t captureStopAfterFailures = 3;
    std::uint32_t captureTotalTimeoutMs = 60000;
    std::uint32_t captureAttemptTimeoutMs = 5000;
    std::uint32_t captureInterAttemptDelayMs = 0;
};

struct TraceBridgeDebugBuildRequest {
    std::string projectPath;
    sigflow::debug::DebugContract contract;
    bool programAndCapture = false;
    TraceBridgeCaptureRequest captureRequest;
    std::function<void(bool, const wxString&)> completion;
};

// TraceBridge 自定义事件类型。
wxDECLARE_EVENT(wxEVT_TB_STEP,     wxCommandEvent);
wxDECLARE_EVENT(wxEVT_TB_PROGRESS, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_TB_LOG,      wxCommandEvent);
wxDECLARE_EVENT(wxEVT_TB_RESULT,   wxCommandEvent);

enum class TbLogTag {
    INFO = 0, CFG, ARM, POL, RD, OK, WR, ERR, CANCEL
};
const wxChar* TbLogTagText(TbLogTag tag);
wxColour    TbLogTagColour(TbLogTag tag);

class TraceBridgeStepStepper;
class TraceBridgeContractSummaryCard;
class TraceBridgeRuntimePanel;

// 拖拽文本目标：用于 hs_valid / hs_ready 路径输入框接受 SigFlowTree 拖来的信号路径。
class SignalPathDropTarget : public wxDropTarget {
public:
    explicit SignalPathDropTarget(wxTextCtrl* target);
    wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def) override;
    bool OnDrop(wxCoord x, wxCoord y) override;
private:
    wxTextCtrl* target_;
};

class TraceBridgeWindow : public wxFrame {
public:
    explicit TraceBridgeWindow(wxWindow* parent);
    ~TraceBridgeWindow() override;

    void SetProjectContext(const wxString& projectPath);
    void ReloadContract();
    void SetCaptureStartHandler(std::function<void(const TraceBridgeCaptureRequest&)> handler);
    void SetDebugBuildStartHandler(std::function<void(const TraceBridgeDebugBuildRequest&)> handler);
    void SetOpenVcdHandler(std::function<void(const wxString&)> handler);
    void SetNavigationCallback(sigflow::wave::WaveNavigationCallback callback);
    void SetCaptureStatus(const wxString& message);
    void SetCaptureResult(bool success, const wxString& message, const wxString& vcdPath = wxEmptyString);
    void SetCaptureRunResult(const sigflow::debug::CaptureRunResult& result);

private:
    void ApplyTheme();

    void BuildUi();
    void BuildLeftPanel(wxPanel* left);
    void BuildRightPanel(wxPanel* right);

    void LoadDefaultContract();
    void LoadContractFromPath(const wxString& path);
    void RefreshContractSummaryUi();

    bool BuildCaptureRequest(TraceBridgeCaptureRequest& request, wxString& error) const;
    bool BuildDebugBuildRequest(TraceBridgeDebugBuildRequest& request, bool programAndCapture,
                                wxString& error) const;
    void RefreshSerialPorts();

    void StartCaptureAsync();
    void CancelCapture();
    void JoinWorkerIfAny();

    // 事件槽
    void OnBrowseContract(wxCommandEvent&);
    void OnReloadContract(wxCommandEvent&);
    void OnRefreshPorts(wxCommandEvent&);
    void OnSelfTest(wxCommandEvent&);
    void OnPlanProbes(wxCommandEvent&);
    void OnOpenCaptureWizard(wxCommandEvent&);
    void OnShowRecoveryPlan(wxCommandEvent&);
    void OnShowTemplateHelp(wxCommandEvent&);
    void OnOpenMonitorSummary(wxCommandEvent&);
    void OnOpenImpactReport(wxCommandEvent&);
    void OnOpenEvidenceReport(wxCommandEvent&);
    void OnStartCapture(wxCommandEvent&);
    void OnBuildDebugBitstream(wxCommandEvent&);
    void OnBuildProgramCapture(wxCommandEvent&);
    void OnCancelCapture(wxCommandEvent&);
    void OnPauseCapture(wxCommandEvent&);
    void OnOpenCaptureWindows(wxCommandEvent&);
    void OnOpenCaptureVcd(wxCommandEvent&);
    void OnCompareSimVcd(wxCommandEvent&);
    void OnGenerateReplay(wxCommandEvent&);
    void OnExportSession(wxCommandEvent&);
    void OnImportSession(wxCommandEvent&);
    void OnRefreshSessions(wxCommandEvent&);
    void OnSessionSelected(wxCommandEvent&);

    // 自定义事件
    void OnTBStep(wxCommandEvent&);
    void OnTBProgress(wxCommandEvent&);
    void OnTBLog(wxCommandEvent&);
    void OnTBResult(wxCommandEvent&);
    void OnClose(wxCloseEvent&);

    void AddLog(TbLogTag tag, const wxString& message);
    void UpdateStepperFromStage(int stageOrder, const wxString& message);
    void SetDebugWorkflowResult(bool success, const wxString& message);

    // 波形比对
    void RunComparison();
    void ShowComparisonResult(const sigflow::debug::ComparisonResult& r);

    // 会话管理
    void RefreshSessionList();
    void LoadSessionVcd(const wxString& sessionId);

    // 导出
    bool ExportSessionToZip(const wxString& sessionId, wxString& outPath, wxString& error);
    bool ImportSessionFromZip(const wxString& archivePath, wxString& sessionId, wxString& error);

    // 数据
    wxString projectPath_;
    wxString contractFilePath_;
    wxString lastCaptureHwVcd_;
    wxString lastCaptureSimVcd_;
    sigflow::debug::DebugContract contract_;
    sigflow::debug::ComparisonResult lastComparison_;
    sigflow::debug::DebugBehaviorSummary lastBehaviorSummary_;
    std::function<void(const TraceBridgeCaptureRequest&)> captureStartHandler_;
    std::function<void(const TraceBridgeDebugBuildRequest&)> debugBuildStartHandler_;
    std::function<void(const wxString&)> openVcdHandler_;
    sigflow::wave::WaveNavigationCallback navigationCallback_;

    // 线程相关
    std::unique_ptr<std::thread> worker_;
    std::shared_ptr<std::atomic<bool>> m_aborted = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<sigflow::debug::CaptureRunService> captureRunService_;
    sigflow::debug::CaptureRunResult lastCaptureRunResult_;
    bool hasCaptureRunResult_ = false;

    // T-140 / THE-030：主题刷新（THE-028：订阅句柄析构即解绑）
    UiTheme::Subscription themeSubscription_;
    wxPanel* leftPanel_ = nullptr;
    wxPanel* rightPanel_ = nullptr;

    // UI 控件
    wxSplitterWindow* splitter_ = nullptr;
    TraceBridgeStepStepper* stepper_ = nullptr;
    TraceBridgeContractSummaryCard* summaryCard_ = nullptr;
    TraceBridgeRuntimePanel* runtimePanel_ = nullptr;
    wxGauge* progressGauge_ = nullptr;
    wxButton* startCaptureButton_ = nullptr;
    wxButton* buildDebugButton_ = nullptr;
    wxButton* buildProgramCaptureButton_ = nullptr;
    wxButton* captureWizardButton_ = nullptr;
    wxButton* recoveryPlanButton_ = nullptr;
    wxButton* templateHelpButton_ = nullptr;
    wxButton* monitorSummaryButton_ = nullptr;
    wxButton* impactReportButton_ = nullptr;
    wxButton* evidenceReportButton_ = nullptr;
    wxButton* pauseCaptureButton_ = nullptr;
    wxButton* cancelCaptureButton_ = nullptr;
    wxButton* captureWindowsButton_ = nullptr;
    wxButton* openHwVcdButton_ = nullptr;
    wxButton* compareSimButton_ = nullptr;
    wxButton* generateReplayButton_ = nullptr;
    wxButton* exportButton_ = nullptr;
    wxButton* importButton_ = nullptr;
    wxButton* refreshSessionsButton_ = nullptr;
    wxChoice* sessionChoice_ = nullptr;
    wxListCtrl* logList_ = nullptr;
    wxStaticText* stepperStatus_ = nullptr;
    wxStaticText* waveHint_ = nullptr;
    wxTextCtrl* probeEditor_ = nullptr;
    bool loadingContract_ = false;
    bool pendingProbeBuild_ = false;

    // 右侧波形面板
    wxNotebook* rightNotebook_ = nullptr;
    sigflow::wave::TraceViewPanel* hwWavePanel_ = nullptr;
    sigflow::wave::TraceViewPanel* simWavePanel_ = nullptr;
    wxTextCtrl* diffReportText_ = nullptr;

    // 会话/比对状态
    wxString lastSessionId_;
    static bool SaveCompareJson(const wxString& sessionDir, const wxString& sessionId,
                                const sigflow::debug::ComparisonResult& r);
};
