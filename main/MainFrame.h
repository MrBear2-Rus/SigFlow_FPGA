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
#include <chrono>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <json/json.h>
#include <wx/stc/stc.h>
#include "ui/UiDockArt.h"
#include "ui/UiLayoutState.h"
#include "ui/UiTabArt.h"
#include "ui/UiTheme.h"
#include "ui/UiToolBarArt.h"
#include "ui/UiRegionHost.h"
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

#include "Composer.h"

#if defined(SIGFLOW_BUILD_EDU_AGENT)
#include "eda-agent-gateway/GatewayServer.h"
#include "agent/AgentServiceController.h"
#include <memory>
#endif

class ToolBars;
class CanvasNoteBook;
class CanvasPanel;
class HandyToolKit;
class ToolboxPanel;
class Structuring;
class VerilogManager;
class YosysExecutor;
class NextpnrExecutor;
class FpgaToolWindow;
class TraceBridgeWindow;
class DebugContractConfigWindow;
class JobRunHandle;
class EditorChrome;
class UiAlignedPanel;
class UiLayoutPicker;
class UiLayoutChoice;
class UiToolBarKeyboard;
class UiToolBarArt;
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
    bool m_isModified;
    SigTextEditor* m_verilogEditor;
    EditorChrome* m_editorChrome = nullptr;
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
    std::vector<std::shared_ptr<JobRunHandle>> m_jobRuns;
    std::vector<wxString> m_activeToolJobIds;
    wxTimer m_coreJobTimer;
    using CoreJobCompletion = std::function<void(const eda::JobRecord&, const eda::JobReport&)>;
    std::map<std::string, CoreJobCompletion> m_pendingCoreJobs;
    void TrackCoreJob(const std::string& jobId, CoreJobCompletion completion);
    void OnCoreJobTimer(wxTimerEvent& event);

    sigflow::Composer* m_composer;

#if defined(SIGFLOW_BUILD_EDU_AGENT)
    // Gateway 与 Python sidecar 都由 IDE 生命周期持有；sidecar 不可用不影响既有 EDA 流程。
    std::unique_ptr<eda::agent::GatewayServer> m_agentGateway;
    std::unique_ptr<AgentServiceController> m_agentSidecar;
    std::string m_agentProjectId;
    wxTimer m_agentEventTimer;
    void ConfigureAgentProjectStorage();
    void RefreshAgentProjectSnapshotState(bool dirty, bool synchronized);
    void OnAgentEventTimer(wxTimerEvent& event);
#endif

    VerilogManager* m_verilogMgr;
    TSParser* m_parser;

    std::map<wxString, std::unordered_map<std::uint64_t, std::tuple<int, int>>> maps;
    void RefreshTitle();
    void ShowFpgaToolWindow(FpgaToolPage page);
    void RunFpgaSynthesis();
    void RunFpgaRoute();
    void RunFpgaPack();
    void RunFpgaProgram(const wxString& bitstreamPath,
                        std::function<void(bool, const wxString&)> completion = {},
                        bool confirmProgramming = true);

    // P1-8：工具链后端能力查询（公开给 MainMenuBar 的能力选择子菜单）。
    void RunTraceBridgeCapture(
        const TraceBridgeCaptureRequest& request,
        std::function<void(bool, const wxString&)> completion = {});
    void RunTraceBridgeDebugBuild(const TraceBridgeDebugBuildRequest& request);
    void KillAsyncToolProcess(long processId);
    // 声明事件处理函数
    void OnAnalysisComplete(wxThreadEvent& event);

public:
    // P1-8：Help 菜单的 "Toolchain Backends" 子菜单构建（动态；MainMenuBar 调用）。
    void BuildToolchainBackendsMenu(wxMenu* menu);
    // P1-8：工具链后端能力查询（供能力选择子菜单/插件管理中心使用）。
    // 数据源为 Composer 的 eda::PluginHost；执行路径仍为 legacy（迁移中）。
    std::vector<std::string> AvailableToolCapabilities() const;
    std::vector<eda::PluginInfo> ToolProviders(const std::string& capability) const;
    eda::PluginInfo DefaultToolProvider(const std::string& capability) const;
    // P1-8：切换某能力的默认后端（持久化到应用配置）。
    void SetDefaultToolProvider(const std::string& capability, const std::string& pluginId);
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
    void ReapJobRuns();

public:
    MainFrame();
    ~MainFrame();
    //新增顶层模块获取方法
    wxString GetTopModuleName();

    void OnUndoStackChanged();

    /* 主题（T-120；SPEC THE-006…THE-009 / THE-028…THE-030） */
    void SetThemeMode(UiTheme::Mode mode);  ///< 设置并持久化主题，随后刷新全部窗口
    void ApplyTheme();                      ///< 把当前主题应用到主窗、AUI 外壳、pane 与状态栏

    /* 布局引擎：六栏显隐（T-510；SPEC LAY-020…LAY-024、KEY-002、INT-020） */
    void SetPanelVisibility(UiTheme::Visibility which, bool visible, bool announce = true);
    void TogglePanelVisibility(UiTheme::Visibility which);
    bool IsPanelVisible(UiTheme::Visibility which);
    void ApplyLayoutVisibility();  ///< 按 m_layoutState 一次性施加六栏显隐（启动恢复用，不写状态栏）
    void ApplyLayoutPreset(const wxString& presetId);  ///< 应用布局预设（LAY-025；菜单入口）
    void SaveLayoutNow();                              ///< 立即保存布局（LAY-031；菜单入口）
    void ResetLayout();                                ///< 恢复默认布局（LAY-032；不触碰业务状态）
    void SetPrimarySide(UiTheme::Side side);
    void SetPanelAlignment(UiTheme::Balance balance);
    void SetPanelPosition(UiTheme::PanelPos position);
    void ShowLayoutPicker();

    /* 窄屏断点与最小窗口（T-520；SPEC LAY-019 / PLT-006 / TEST-020 / A11Y-010） */
    /// 按客户区宽度（DIP）施加断点：<1024 两 dock 用 clamp、<760 辅助侧栏自动隐藏。
    /// 只在宽度真正变化时重算，避免 SendSizeEvent 引起的重入与抖动。
    void ApplyResponsiveLayout();

    /* DPI / 显示器 / 系统配色（T-660；SPEC PLT-009 / PLT-010 / THE-033）---- */
    /// DPI 或显示器变化后的全量重算：Art 度量 → 配色 → DIP 骨架尺寸 → 控件字体 → 工作区校验。
    void OnDpiOrDisplayChanged(const char* reason);
    /// 重设以 FromDIP 计算字体的控件（编辑器 / 终端），避免字号滞留旧 DPI 的像素值。
    void ReapplyDpiDependentFonts();
    /// PLT-010：几何恢复后校验窗口/浮动 pane 仍在某个显示器的可见工作区内。
    void EnsureWindowsOnScreen();
    /// 只读访问布局状态（供菜单同步预设勾选态，LAY-025）
    const UiTheme::LayoutState& GetLayoutState() const { return m_layoutState; }
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
    void DoSimCancel();       // 取消仿真作业
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
    void ShowComponentLibrary();

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

    // ---- 自定义布局状态与持久化（T-250；SPEC LAY-020…LAY-032）----
    UiTheme::LayoutState m_layoutState;
    wxString m_defaultPerspective;  ///< 本版本默认 AUI perspective（恢复失败时回退用）
    void RestoreLayoutState();
    void SaveLayoutState();

    // ---- 主题订阅（T-120；SPEC THE-028：必须可解绑）----
    UiTheme::Subscription m_themeSubscription;

    /* 布局引擎内部（T-510）---- */
    void SetPaneVisible(const char* paneName, bool visible);
    void SyncPanelMenuChecks();
    /// T-720 / F-720-01：菜单栏显隐。MSW 上 `wxMenuBar::Show()` 是**空操作**（菜单栏无窗口句柄），
    /// 必须走 Win32 `SetMenu`/`DrawMenuBar`；其它平台走原生 `Show()`。实现见 .cpp。
    void ApplyMenuBarVisibility(bool visible);
    void ApplyLayoutGeometry();
    void OnLayoutKey(wxKeyEvent& event);

    /* 键盘可达与无障碍内部（T-620；SPEC KEY-007…KEY-011 / A11Y-001…A11Y-006 / INT-019）---- */
    /// CHAR_HOOK：记录输入来源（键盘/鼠标），并把工具栏内方向键/Space 路由给 UiToolBarKeyboard。
    /// 永不消费 Tab / Ctrl+Tab / Esc / 编辑器按键（KEY-007、KEY-009、INT-019）。
    void OnKeyboardCharHook(wxKeyEvent& event);
    /// Tab 顺序 = 视觉顺序（KEY-007/A11Y-003）：菜单→工具栏→活动栏→主侧栏→画布→编辑器→辅助→底部→状态栏。
    void SetupTabOrder();
    /// Tab/Shift+Tab 显式走位（等效手段：wxFrame 在 MSW 上不是 Tab 域）。返回 true 表示已消费。
    bool AdvanceKeyboardFocus(bool backward);
    /// KEY-008：主侧栏宽度键盘步进（每次 12 DIP，遵守 pane 自身的最小/最大保护）。
    void StepPrimarySideBarWidth(int step);
    std::unique_ptr<UiToolBarKeyboard> m_topBarKeyboard;
    std::unique_ptr<UiToolBarKeyboard> m_railKeyboard;
    UiToolBarArt* m_topBarArt = nullptr;  ///< 非拥有；由 topBar 的 art provider 持有（THE-032）
    UiToolBarArt* m_railArt = nullptr;    ///< 非拥有；由 sideNav 的 art provider 持有（THE-032）

    /* 窄屏断点内部（T-520）---- */
    int m_lastResponsiveWidthDip = -1;   ///< 上次施加断点时的客户区宽度（DIP）
    bool m_rightNarrowOverride = false;  ///< 窄屏下用户显式打开过辅助侧栏（覆盖 LAY-019 自动隐藏）
    wxControl* m_synthesizeCta = nullptr;  ///< 工具栏内嵌主 CTA（缺陷 B：窄屏放不下时隐藏而非压成 0 宽）
    /* T-520 性能闸门：本函数的下半段每次都要多次全量 AUI 重排，必须只在"布局结果会变"时执行。
     * 不加这道闸会把 T-510 的「预设应用 ≤150ms」预算顶爆（实测 4 个预设全部超时）。 */
    UiTheme::Breakpoint m_lastBreakpoint = UiTheme::Breakpoint::Wide;
    int m_lastClampedLeft = -1;          ///< 上次 clamp 后的左 dock 宽度（DIP）
    int m_lastClampedRight = -1;         ///< 上次 clamp 后的右 dock 宽度（DIP）
    bool m_lastAutoHidden = false;       ///< 上次是否处于"辅助侧栏自动隐藏"
    UiTheme::Breakpoint m_lastProbeBreakpoint = UiTheme::Breakpoint::Wide;
    int m_lastProbeWidthDip = -1000;     ///< 内嵌控件探测的宽度量化（8 DIP），避免逐像素重排
    /* PERF-005 性能闸门：ApplyLayoutGeometry() 每次都做一次全量 AUI 重排 + Tab 顺序重排，
     * 而**纯显隐变化不改几何**。缓存几何签名后，纯显隐的预设/切换只走一次重排。 */
    int m_lastGeometrySignature = -1;    ///< side/pos/bal 的组合签名；-1 表示尚未施加过
    UiRegionHost* m_bottomHost = nullptr;
    wxAuiNotebook* m_bottomNotebook = nullptr;
    wxControl* m_layoutControls = nullptr;
    UiLayoutChoice* m_layoutButtons[3] = {};
    UiLayoutPicker* m_layoutPicker = nullptr;
    /// T-620：Tab 顺序（KEY-007）用的非拥有窗指针：工具栏→活动栏→主侧栏→中区→辅助侧栏→底部面板。
    wxWindow* m_tabOrder[6] = {};

    /* 状态栏右区（T-340；SPEC CMP-070…CMP-073）---- */
    CanvasPanel* m_activeCanvas = nullptr;  ///< 当前画布页（非拥有）
    wxPoint m_lastCanvasCursor{ 0, 0 };     ///< 最近一次鼠标所在的画布坐标（已按网格吸附）
    void SyncActiveCanvas();                ///< 把回报回调接到当前画布页，并刷新状态栏
    void UpdateCanvasStatus();              ///< 刷新 坐标 / Grid / Zoom / 工具链 四个字段

    wxActivityIndicator* m_busyIndicator = nullptr;
    void LayoutBusyIndicator();

public:
    void ShowBusyIndicator(const wxString& text = wxEmptyString);
    void HideBusyIndicator(const wxString& text = wxT("就绪"));
    //std::vector<CanvasPanel*> m_canvas;
    CanvasNoteBook* m_canvas;

    void UpdateCursor();        // 根据 m_pendingTool 更新十字/箭头

    // 仿真引擎
    std::shared_ptr<SimulationEngine> m_simEngine;
    bool m_guiSimProvidersRegistered = false;
    std::string m_activeCoreSimJobId;
    void EnsureGuiSimProviders();

    void OnToolboxElement(wxCommandEvent& evt);
    
public:

    // 事件表声明
    wxDECLARE_EVENT_TABLE();
};

// 事件表定义放在类外部
// 注意：不要在类的声明内部定义事件表


// T-130：原 `ModernDockArt`（AUI 停靠外观，含 6 处硬编码颜色）已移出本头文件，
//        改为 token 驱动的 `UiDockArt`（main/ui/UiDockArt.h/.cpp）。
//        Art provider 仍由 wxAUI 接管所有权（THE-032）；获取时必须用 dynamic_cast（THE-033 / GAP-E2）。


// T-230：原 `MyCustomToolBarArt`（仅实现 checked 态，且基类随平台别名化）已移出本头文件，
//        改为 token 驱动的 chip 绘制 `UiToolBarArt`（main/ui/UiToolBarArt.h/.cpp），
//        并同时安装到 `topBar` 与 `sideNav`（GAP-C3：原实现只装了 sideBar）。
