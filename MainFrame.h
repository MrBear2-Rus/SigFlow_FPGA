// MainFrame.h
#pragma once
#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/file.h>
#include <wx/xml/xml.h>
#include <wx/mstream.h>

#include "3rd/json/json.h"
#include "PropertyPanel.h"
#include <wx/stc/stc.h>
#include "AsyncAnalysisCenter.h"
#include "SigTextEditor.h"
#include "ProjectTreePanel.h"
#include "SigTree.h"


class ToolBars;
class CanvasPanel;
class HandyToolKit;

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

    /* Window 菜单业务接口 */
    void DoWindowCombinationalAnalysis();
    void DoWindowPreferences();
    void DoWindowSwitchToDoc(const wxString& title);

    /* Help 菜单业务接口 */
    void DoHelpTutorial();
    void DoHelpUserGuide();
    void DoHelpLibraryRef();
    void DoHelpAbout();

    // 工具栏
    ToolBars* m_toolBars;
    void AddToolBarsToAuiManager();

private:
    wxAuiManager m_auiMgr;
    PropertyPanel* m_propPanel = nullptr;
public:
    CanvasPanel* m_canvas;

    void UpdateCursor();        // 根据 m_pendingTool 更新十字/箭头

    void OnToolboxElement(wxCommandEvent& evt);
    
public:
    // 更新属性面板显示选中元件
    void UpdatePropertyPanel(int elementIndex);
    PropertyPanel* GetPropertyPanel() const { return m_propPanel; }

    // 事件表声明
    wxDECLARE_EVENT_TABLE();
};

// 事件表定义放在类外部
// 注意：不要在类的声明内部定义事件表
