#pragma once

#include "DebugAcquisition.h"
#include "DebugBehaviorSummary.h"
#include "DebugContract.h"
#include "WaveformAligner.h"
#include "WaveformComparator.h"
#include "../wave/TraceViewPanel.h"

#include <atomic>
#include <condition_variable>
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

private:
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
    void OnStartCapture(wxCommandEvent&);
    void OnBuildDebugBitstream(wxCommandEvent&);
    void OnBuildProgramCapture(wxCommandEvent&);
    void OnCancelCapture(wxCommandEvent&);
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
    std::atomic<bool> m_aborted{false};

    // UI 控件
    wxSplitterWindow* splitter_ = nullptr;
    TraceBridgeStepStepper* stepper_ = nullptr;
    TraceBridgeContractSummaryCard* summaryCard_ = nullptr;
    TraceBridgeRuntimePanel* runtimePanel_ = nullptr;
    wxGauge* progressGauge_ = nullptr;
    wxButton* startCaptureButton_ = nullptr;
    wxButton* buildDebugButton_ = nullptr;
    wxButton* buildProgramCaptureButton_ = nullptr;
    wxButton* cancelCaptureButton_ = nullptr;
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
