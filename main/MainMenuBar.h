#pragma once
#include <wx/wx.h>
#include <wx/filehistory.h>

class MainFrame;  // 前向声明

/*
 * 暂时只实现 File 菜单，其余菜单留空
 * 事件全部转发到 MainFrame 的 DoFileXXX() 接口
 */
class MainMenuBar : public wxMenuBar
{
public:
    explicit MainMenuBar(MainFrame* owner);
    ~MainMenuBar();

    /* 最近文件列表封装 */
    void    AddFileToHistory(const wxString& fullPath);
    wxString GetHistoryFile(size_t i) const;
    size_t  GetHistoryFileCount() const;
    void    LoadHistory();
    void    SaveHistory();

    /* 让 MainFrame 调用的接口 */
    void RebuildWindowMenu();
    void AddDocToWindowList(const wxString& title);
    void SetCurrentDocInWindowList(const wxString& title);

    // T-120：把 Window › Theme 的单选态同步到当前主题（SPEC THE-006/THE-007）
    void UpdateThemeMenuChecks();

    // T-510：把 Window › Panels 的勾选态同步到当前六栏显隐（SPEC LAY-023/LAY-025、INT-020）
    void UpdatePanelMenuChecks();

    void SetSimulationBusy(bool busy);

    // P1-8：Toolchain Backends 子菜单（懒构建：Help 菜单打开时按当前插件列表填充）。
    void RebuildToolchainBackendsMenu();

private:
    MainFrame* m_owner;
    wxFileHistory m_fileHistory;

    wxMenu* m_windowMenu;        // 保存指针，方便重建
    wxMenu* m_helpMenu;          // P1-8：Help 菜单
    wxMenu* m_helpBackendsMenu;  // P1-8：Toolchain Backends 子菜单
    wxString m_curDocTitle;      // 当前文档标题（不含路径）

    /* 六大菜单创建函数 */
    wxMenu* CreateFileMenu();
    wxMenu* CreateEditMenu();
    wxMenu* CreateProjectMenu();
    wxMenu* CreateSimulateMenu();
    wxMenu* CreateFpgaMenu();
    wxMenu* CreateWindowMenu();
    wxMenu* CreateHelpMenu();

    /* File 菜单事件回调 */
    void OnFileOpenProject(wxCommandEvent&);
    void OnFileNew(wxCommandEvent&);
    void OnFileOpen(wxCommandEvent&);
    void OnFileClose(wxCommandEvent&);
    void OnFileSave(wxCommandEvent&);
    void OnFileSaveAs(wxCommandEvent&);
    void OnSaveAsNode(wxCommandEvent& event);  // 处理.node保存事件
    void OnSaveAsNet(wxCommandEvent& event);   // 处理.net保存事件
    void OnFileHistory(wxCommandEvent&);
    void OnExportImage(wxCommandEvent&);
    void OnPrint(wxCommandEvent&);
    void OnPreferences(wxCommandEvent&);
    void OnExit(wxCommandEvent&);

    /* Edit 菜单事件回调 */
    void OnUndo(wxCommandEvent&);
    void OnCut(wxCommandEvent&);
    void OnCopy(wxCommandEvent&);
    void OnPaste(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnDuplicate(wxCommandEvent&);
    void OnSelectAll(wxCommandEvent&);
    void OnRaiseSel(wxCommandEvent&);
    void OnLowerSel(wxCommandEvent&);
    void OnRaiseTop(wxCommandEvent&);
    void OnLowerBottom(wxCommandEvent&);
    void OnAddVertex(wxCommandEvent&);
    void OnRemoveVertex(wxCommandEvent&);

    /* Project 菜单事件回调 */
    void OnAddCircuit(wxCommandEvent&);
    void OnLoadLibrary(wxCommandEvent&);
    void OnUnloadLibraries(wxCommandEvent&);
    void OnMoveCircuitUp(wxCommandEvent&);
    void OnMoveCircuitDown(wxCommandEvent&);
    void OnSetAsMainCircuit(wxCommandEvent&);
    void OnRemoveCircuit(wxCommandEvent&);
    void OnRevertAppearance(wxCommandEvent&);
    void OnViewToolbox(wxCommandEvent&);
    void OnViewSimTree(wxCommandEvent&);
    void OnEditLayout(wxCommandEvent&);
    void OnEditAppearance(wxCommandEvent&);
    void OnAnalyzeCircuit(wxCommandEvent&);
    void OnGetStats(wxCommandEvent&);
    void OnOptions(wxCommandEvent&);

    /* Simulate 菜单事件回调 */
    void OnSimEnable(wxCommandEvent&);
    void OnSimReset(wxCommandEvent&);
    void OnSimStep(wxCommandEvent&);
    void OnGoOutToState(wxCommandEvent&);
    void OnGoInToState(wxCommandEvent&);
    void OnTickOnce(wxCommandEvent&);
    void OnTicksEnabled(wxCommandEvent&);
    void OnSetTickFreq(wxCommandEvent&);
    void OnLogging(wxCommandEvent&);
    void OnSimCompile(wxCommandEvent&);      // 编译仿真模型
    void OnSimRun(wxCommandEvent&);          // 运行仿真
    void OnSimClean(wxCommandEvent&);        // 清理仿真缓存
    void OnSimCancel(wxCommandEvent&);       // 取消仿真作业

    /* FPGA 菜单事件回调 */
    void OnFpgaSynthesis(wxCommandEvent&);
    void OnFpgaCancelSynthesis(wxCommandEvent&);
    void OnFpgaShowSynthesisJobs(wxCommandEvent&);
    void OnFpgaOpenSynthesisReport(wxCommandEvent&);
    void OnFpgaRetrySynthesis(wxCommandEvent&);
    void OnFpgaRoute(wxCommandEvent&);
    void OnFpgaCancelRoute(wxCommandEvent&);
    void OnFpgaProgram(wxCommandEvent&);
    void OnFpgaPinBinding(wxCommandEvent&);
    void OnTraceBridge(wxCommandEvent&);
    void OnDebugContract(wxCommandEvent&);

    /* Window 菜单事件回调 */
    void OnMinimize(wxCommandEvent&);
    void OnMaximize(wxCommandEvent&);
    void OnCloseWnd(wxCommandEvent&);
    void OnCombinationalAnalysis(wxCommandEvent&);
    void OnWindowPreferences(wxCommandEvent&);
    void OnWindowItem(wxCommandEvent&);          // 动态文档列表统一入口

    /* 主题菜单事件回调（T-120；id 段 wxID_HIGHEST+310..312） */
    void OnThemeFollowSystem(wxCommandEvent&);
    void OnThemeLight(wxCommandEvent&);
    void OnThemeDark(wxCommandEvent&);

    /* 六栏显隐事件回调（T-510；id 段 wxID_HIGHEST+320..325） */
    void OnToggleMenuBar(wxCommandEvent&);
    void OnToggleRail(wxCommandEvent&);
    void OnToggleLeftBar(wxCommandEvent&);
    void OnToggleRightBar(wxCommandEvent&);
    void OnToggleBottomPanel(wxCommandEvent&);
    void OnToggleStatusBar(wxCommandEvent&);

    /* 布局预设事件回调（T-510；id 段 wxID_HIGHEST+330..333） */
    void OnApplyLayoutPreset(wxCommandEvent&);

    /* 布局保存 / 重置（T-250；id 段 wxID_HIGHEST+340..345；SPEC LAY-031/LAY-032） */
    void OnSaveLayout(wxCommandEvent&);
    void OnResetLayout(wxCommandEvent&);
    void OnCustomizeLayout(wxCommandEvent&);
    /* T-730：显式浮动/收回面板（不依赖 caption 拖拽；Debug 构建实测拖不动） */
    void OnFloatSidePanel(wxCommandEvent&);
    void OnFloatLeftPanel(wxCommandEvent&);
    void OnFloatBottomPanel(wxCommandEvent&);

    /* Window 菜单业务接口 */
    void DoWindowCombinationalAnalysis();
    void DoWindowPreferences();
    void DoWindowSwitchToDoc(const wxString& title);

    /* Help 菜单事件回调 */
    void OnTutorial(wxCommandEvent&);
    void OnUserGuide(wxCommandEvent&);
    void OnLibraryRef(wxCommandEvent&);
    void OnAbout(wxCommandEvent&);
#if defined(SIGFLOW_BUILD_EDU_AGENT)
    void OnTeachingQuestion(wxCommandEvent&);
    void OnTeachingHint(wxCommandEvent&);
    void OnTeachingCancel(wxCommandEvent&);
#endif
    void OnMenuOpen(wxMenuEvent&);          // P1-8：Help 打开时懒重建后端子菜单
    DECLARE_EVENT_TABLE()
};
