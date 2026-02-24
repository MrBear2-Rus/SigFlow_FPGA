// MainFrame.h
#pragma once
#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/file.h>
#include <wx/xml/xml.h>
#include <wx/mstream.h>

#include <json/json.h>
#include <wx/stc/stc.h>
#include "AsyncAnalysisCenter.h"
#include "SigTextEditor.h"
#include "ProjectTreePanel.h"
#include "SigTree.h"
#include "SigFlowTreePanel.h"
#include "SFNPropertyPanel.h"
#include "Simulation/SimulationEngine.h"

#include "TerminalCtrl.h"

#include "PluginManager.h"

class ToolBars;
class CanvasPanel;
class HandyToolKit;

wxDECLARE_EVENT(EVT_SFTREE_NODE_ACTIVATED, wxCommandEvent);
wxDECLARE_EVENT(EVT_SFTREE_CHANGED, wxCommandEvent);

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
    SigFlowTreePanel* m_sigFlowTreePanel;
    SFNPropertyPanel* m_sfnPropertyPanel;
    TerminalCtrl* m_terminalCtrl;

    PluginManager* m_pluginMgr;
    void RefreshTitle();
    // 声明事件处理函数
    void OnAnalysisComplete(wxThreadEvent& event);

    wxTimer* m_refreshTimer;
    void OnRefreshTimer(wxTimerEvent& event);
    std::string m_lastProcessedCode; // 用于对比，避免没改动也分析

private:
    bool SaveToFile(const wxString& filePath);
    wxString GenerateFileContent();

private:
    void AddLibraryNode(wxXmlNode* parent, const wxString& name, const wxString& desc);
    void AddWireNode(wxXmlNode* parent, const wxString& from, const wxString& to);
    bool SaveAsNodeFile(const wxString& filePath);
    bool SaveAsNetFile(const wxString& filePath);
    void OnClose(wxCloseEvent& event);
    void OnToolSelected(wxCommandEvent& evt);

public:
    MainFrame();
    ~MainFrame();

    void OnUndoStackChanged();
    void OnOpenFileFromTree(wxCommandEvent& evt);

    /* File 菜单业务接口 */
    void DoFileOpenProject();
    void DoFileNew();
    void DoFileOpen(const wxString& path = {});
    void DoFileSave();
    void DoFileSaveAs();
    void DoFileSaveAsNode();  // 另存为.node文件
    void DoFileSaveAsNet();   // 另存为.net文件
    void OnQuit(wxCommandEvent&) { Close(true); }
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
    void DoSimCompile();      // 编译仿真模型
    void DoSimRun();          // 运行仿真
    void DoSimClean();        // 清理仿真缓存

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
    void PropertyLoadNode(SigTreeNode* node);

private:
    wxAuiManager m_auiMgr;
public:
    CanvasPanel* m_canvas;

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
