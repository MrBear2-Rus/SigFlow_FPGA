// MainFrame.h
#pragma once
#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/activityindicator.h>
#include <wx/file.h>
#include <wx/xml/xml.h>
#include <wx/mstream.h>

#include <memory>
#include <functional>

#include <json/json.h>
#include <wx/stc/stc.h>
#include "AsyncAnalysisCenter.h"
#include "SigTextEditor.h"
#include "ProjectTreePanel.h"
#include "SigTree.h"
#include "SigFlowTreePanel.h"
#include "SFNPropertyPanel.h"
#include "Simulation/SimulationEngine.h"
#include "fpga/FpgaPinBindingPanel.h"
#include "BuildProgressBar.h"

#include "wave/TraceViewPanel.h"

#include "TerminalCtrl.h"

#include "PluginManager.h"

class ToolBars;
class CanvasNoteBook;
class HandyToolKit;
class ToolboxPanel;
class Structuring;
class VerilogManager;
class YosysExecutor;
class NextpnrExecutor;
class FpgaToolWindow;
class TraceBridgeWindow;
class DebugContractConfigWindow;
enum class FpgaToolPage;
struct TraceBridgeCaptureRequest;
struct TraceBridgeDebugBuildRequest;

wxDECLARE_EVENT(EVT_SFTREE_NODE_ACTIVATED, wxCommandEvent);


class MainFrame : public wxFrame
{
private:
    int snap_version;
    wxString m_projectName;
    wxString m_currentFilePath;
    wxString m_currentProjectPath;
    wxString m_workspacePath;
    wxString GetWorkspaceCopyPath(const wxString& m_currentFilePath);
    bool m_isModified;
    SigTextEditor* m_verilogEditor;
    AsyncAnalysisCenter* m_analysisCenter; // 异步中心指针
    ProjectTreePanel* m_projectTreePanel;
    SigFlowTree* sigTree;
    ToolboxPanel* m_toolbox;
    SigFlowTreePanel* m_sigFlowTreePanel;
    SFNPropertyPanel* m_sfnPropertyPanel;
    FpgaPinBindingPanel* m_fpgaPinBindingPanel;
    FpgaToolWindow* m_fpgaToolWindow;
    TraceBridgeWindow* m_traceBridgeWindow;
    DebugContractConfigWindow* m_debugContractConfigWindow;
    TerminalCtrl* m_terminalCtrl;
    BuildProgressBar* m_buildProgressBar = nullptr;
    sigflow::wave::TraceViewPanel* m_wavePanel;
    std::unique_ptr<YosysExecutor> m_yosysExecutor;
    std::unique_ptr<NextpnrExecutor> m_nextpnrExecutor;
    wxString m_activeNextpnrJobId;
    wxString m_activeDebugSessionId;
    bool m_routeCancelRequested = false;
    wxString m_pendingNextpnrRetryOf;
    wxString m_activeYosysJobId;
    wxString m_pendingYosysRetryOf;

    PluginManager* m_pluginMgr;

    VerilogManager* m_verilogMgr;
    TSParser* m_parser;

    std::map<wxString, std::unordered_map<SigTreeNode*, std::tuple<int, int>>> maps;
    void RefreshTitle();
    void ShowFpgaToolWindow(FpgaToolPage page);
    void RunFpgaSynthesis();
    void RunFpgaRoute();
    void RunFpgaPack();
    void RunFpgaProgram(const wxString& bitstreamPath,
                        std::function<void(bool, const wxString&)> completion = {},
                        bool confirmProgramming = false);
    void RunTraceBridgeCapture(
        const TraceBridgeCaptureRequest& request,
        std::function<void(bool, const wxString&)> completion = {});
    void RunTraceBridgeDebugBuild(const TraceBridgeDebugBuildRequest& request);
    void KillAsyncToolProcess(long processId);
    // 声明事件处理函数
    void OnAnalysisComplete(wxThreadEvent& event);

private:
    bool SaveToFile(const wxString& filePath);
    wxString GenerateFileContent();

private:
    void AddLibraryNode(wxXmlNode* parent, const wxString& name, const wxString& desc);
    void AddWireNode(wxXmlNode* parent, const wxString& from, const wxString& to);
    bool SaveAsNodeFile(const wxString& filePath);
    bool SaveAsNetFile(const wxString& filePath);
    bool ConfirmCurrentWorkBeforeProjectSwitch();
    void ResetCurrentDocumentForProjectSwitch();
    void OnClose(wxCloseEvent& event);
    void OnToolSelected(wxCommandEvent& evt);

public:
    MainFrame();
    ~MainFrame();
    //新增顶层模块获取方法
    wxString GetTopModuleName();

    void OnUndoStackChanged();
    void OnOpenFileFromTree(wxCommandEvent& evt);
    void SetProjectDir(const wxString& projectDir);
    /* File 菜单业务接口 */
    void DoFileOpenProject();
    bool DoFileNew();
    void DoFileOpen(const wxString& path = {});
    bool DoFileSave();
    bool DoFileSaveAs();
    void DoFileSaveAsNode();  // 另存为.node文件
    void DoFileSaveAsNet();   // 另存为.net文件
    void OnQuit(wxCommandEvent&) { Close(); }
    void OnAbout(wxCommandEvent&);

    /* Edit 菜单业务接口 */
    void DoEditUndo();
    void DoEditCut();
    void DoEditCopy();
    void DoEditPaste();
    void DoEditDelete();
    void DoEditDuplicate();
    void DoEditSelectAll();
    void DoEditRaiseSel();
    void DoEditLowerSel();
    void DoEditRaiseTop();
    void DoEditLowerBottom();
    void DoEditAddVertex();
    void DoEditRemoveVertex();

    /* Project 菜单业务接口 */
    void DoProjectAddCircuit();
    void DoProjectLoadLibrary();
    void DoProjectUnloadLibraries();
    void DoProjectMoveCircuitUp();
    void DoProjectMoveCircuitDown();
    void DoProjectSetAsMain();
    void DoProjectRemoveCircuit();
    void DoProjectRevertAppearance();
    void DoProjectViewToolbox();
    void DoProjectViewSimTree();
    void DoProjectEditLayout();
    void DoProjectEditAppearance();
    void DoProjectAnalyzeCircuit();
    void DoProjectGetStats();
    void DoProjectOptions();

    /* Simulate 菜单业务接口 */
    void DoSimSetEnabled(bool on);
    void DoSimReset();
    void DoSimStep();
    void DoSimGoOut();
    void DoSimGoIn();
    void DoSimTickOnce();
    void DoSimTicksEnabled(bool on);
    void DoSimSetTickFreq(int hz);
    void DoSimLogging();
    
    /* Verilator 仿真接口 */
    bool LoadProjectConfig(const wxString& projectPath, 
                           wxString& outTopModule,
                           std::vector<wxString>& outSourceFiles);  // 读取项目配置
    void DoSimCompile();      // 编译仿真模型
    void DoSimRun();          // 运行仿真
    void DoSimClean();        // 清理仿真缓存

    /* FPGA 工具链接口 */
    void DoFpgaSynthesis();
    void DoFpgaCancelSynthesis();
    void DoFpgaShowSynthesisJobs();
    void DoFpgaOpenSynthesisReport();
    void DoFpgaRetrySynthesis();
    void DoFpgaRoute();
    void DoFpgaCancelRoute();
    void DoFpgaProgram();
    void DoFpgaPinBinding();
    void DoTraceBridge();
    void DoDebugContract();

    /* Window 菜单业务接口 */
    void DoWindowCombinationalAnalysis();
    void DoWindowPreferences();
    void DoWindowSwitchToDoc(const wxString& title);

    /* Help 菜单业务接口 */
    void DoHelpTutorial();
    void DoHelpUserGuide();
    void DoHelpLibraryRef();
    void DoHelpAbout();

    void OnSFNodeActivated(wxCommandEvent& event);
    void OnSFTreeChanged(wxCommandEvent& event);
    void OnSFNodeAdded(wxCommandEvent& event);
    void OnSFNodeDeleted(wxCommandEvent& event);
    void OnSFNodeChanged(wxCommandEvent& event);
    void PropertyLoadNode(SigTreeNode* node);

private:
    wxAuiManager m_auiMgr;
    wxActivityIndicator* m_busyIndicator = nullptr;
    void LayoutBusyIndicator();

public:
    void ShowBusyIndicator(const wxString& text = wxEmptyString);
    void HideBusyIndicator(const wxString& text = wxT("就绪"));
    //std::vector<CanvasPanel*> m_canvas;
    CanvasNoteBook* m_canvas;

    void UpdateCursor();        // 根据 m_pendingTool 更新十字/箭头

    // 仿真引擎
    std::unique_ptr<SimulationEngine> m_simEngine;

    void OnToolboxElement(wxCommandEvent& evt);
    
public:

    // 事件表声明
    wxDECLARE_EVENT_TABLE();
};

// 事件表定义放在类外部
// 注意：不要在类的声明内部定义事件表


class ModernDockArt : public wxAuiDefaultDockArt {
public:
    ModernDockArt() {
        // --- 静态属性：这些不随 DPI 改变 ---
        SetMetric(wxAUI_DOCKART_GRADIENT_TYPE, wxAUI_GRADIENT_NONE);
        SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, 1);
        SetMetric(wxAUI_DOCKART_GRIPPER_SIZE, 0); // 彻底干掉神人拖拽柄
        SetMetric(wxAUI_DOCKART_SASH_SIZE, wxWindow::FromDIP(16, nullptr));


        // --- 颜色属性：通常颜色不随 DPI 缩放 ---
        SetColour(wxAUI_DOCKART_BACKGROUND_COLOUR, wxColour(255, 255, 255));
        SetColour(wxAUI_DOCKART_SASH_COLOUR, wxColour(230, 230, 230));
        SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, wxColour(225, 230, 235));
        SetColor(wxAUI_DOCKART_ACTIVE_CAPTION_COLOUR, wxColour(225, 230, 235));
        SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, wxColour(120, 130, 140));
        SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR, wxColour(120, 130, 140));
    }

    // --- 动态属性：在 MainFrame 中调用这个函数 ---
    void UpdateMetrics(wxWindow* parent) {
        // 关键：根据传入的 parent 动态计算像素
        SetMetric(wxAUI_DOCKART_SASH_SIZE, parent->FromDIP(2));

        // 标题栏文字适配
        wxFont captionFont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        captionFont.SetFractionalPointSize(captionFont.GetFractionalPointSize() * 1.05);
        SetFont(wxAUI_DOCKART_CAPTION_FONT, captionFont);
    }
};


class MyCustomToolBarArt : public wxAuiDefaultToolBarArt {
public:
    void DrawButton(wxDC& dc, wxWindow* wnd, const wxAuiToolBarItem& item,
        const wxRect& rect) override {
        // 如果这个按钮被选中了 (Toggle 状态)
        if (item.GetState() & wxAUI_BUTTON_STATE_CHECKED) {
            // 1. 画你想要的背景，比如一个圆角矩形，或者干脆纯色
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(255, 255, 255))); // 选中变白
            dc.DrawRectangle(rect);

            // 2. 甚至可以画一根侧边的“指示条”（像 VS Code 那样）
            dc.SetBrush(wxBrush(wxColour(0, 120, 215))); // 蓝色指示条
            dc.DrawRectangle(rect.x, rect.y + 2, 3, rect.height - 4);
        }

        // 最后调用基类画图标，或者你自己画图标
        wxAuiDefaultToolBarArt::DrawButton(dc, wnd, item, rect);
    }
};
