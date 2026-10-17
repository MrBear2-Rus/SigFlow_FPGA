#include "MainMenuBar.h"
#include "MainFrame.h"  // 为了转发调用 DoFileXXX
#include "ui/UiLayoutState.h"  // T-510：六栏显隐枚举（LAY-020）
#include "ui/UiTheme.h" // T-120：主题三态（SPEC THE-006/THE-007）
#include <wx/config.h>
#include "platform/Log.h"

// T-120：主题菜单 id（避开既有占用：+300/+301 内置工具、+301..304 TraceViewPanel、+400+ 动态文档、+500+ Help）
enum
{
    idThemeFollowSystem = wxID_HIGHEST + 310,
    idThemeLight        = wxID_HIGHEST + 311,
    idThemeDark         = wxID_HIGHEST + 312
};

// T-510：六栏显隐菜单 id（SPEC LAY-020 的六个字段；快捷键见 KEY-002）
enum
{
    idPanelMenuBar   = wxID_HIGHEST + 320,
    idPanelRail      = wxID_HIGHEST + 321,
    idPanelLeft      = wxID_HIGHEST + 322,
    idPanelRight     = wxID_HIGHEST + 323,
    idPanelBottom    = wxID_HIGHEST + 324,
    idPanelStatusBar = wxID_HIGHEST + 325
};

// T-510：布局预设菜单 id（LAY-025；与 UiTheme::Presets 的四个 id 对应）
enum
{
    idPresetClassic = wxID_HIGHEST + 330,
    idPresetCode    = wxID_HIGHEST + 331,
    idPresetCanvas  = wxID_HIGHEST + 332,
    idPresetChat    = wxID_HIGHEST + 333
};

// T-250：布局保存 / 重置（SPEC LAY-031/LAY-032、docs/gui P2-2）
enum
{
    idSaveLayout  = wxID_HIGHEST + 340,
    idResetLayout = wxID_HIGHEST + 341,
    idCustomizeLayout = wxID_HIGHEST + 342,
    // T-730：显式浮动/收回（不依赖 caption 拖拽）
    idFloatSidePanel = wxID_HIGHEST + 343,
    idFloatLeftPanel = wxID_HIGHEST + 344,
    idFloatBottomPanel = wxID_HIGHEST + 345
};

enum
{
    idExportImage = wxID_HIGHEST + 1,
    idSaveAsNode = wxID_HIGHEST + 2,  // 保存.node文件
    idSaveAsNet = wxID_HIGHEST + 3,    // 保存.net文件
    idOpenProj = wxID_HIGHEST + 4
};

wxBEGIN_EVENT_TABLE(MainMenuBar, wxMenuBar)

EVT_MENU(idOpenProj, MainMenuBar::OnFileOpenProject)
EVT_MENU(wxID_NEW, MainMenuBar::OnFileNew)
EVT_MENU(wxID_OPEN, MainMenuBar::OnFileOpen)
EVT_MENU(wxID_CLOSE, MainMenuBar::OnFileClose)
EVT_MENU(wxID_SAVE, MainMenuBar::OnFileSave)
EVT_MENU(wxID_SAVEAS, MainMenuBar::OnFileSaveAs)
EVT_MENU(idSaveAsNode, MainMenuBar::OnSaveAsNode)
EVT_MENU(idSaveAsNet, MainMenuBar::OnSaveAsNet)
EVT_MENU(idExportImage, MainMenuBar::OnExportImage)
EVT_MENU(wxID_PRINT, MainMenuBar::OnPrint)
EVT_MENU(wxID_PREFERENCES, MainMenuBar::OnPreferences)
EVT_MENU(wxID_EXIT, MainMenuBar::OnExit)
// 最近文件事件范围
EVT_MENU_RANGE(wxID_FILE1, wxID_FILE9, MainMenuBar::OnFileHistory)

EVT_MENU(wxID_UNDO, MainMenuBar::OnUndo)
EVT_MENU(wxID_CUT, MainMenuBar::OnCut)
EVT_MENU(wxID_COPY, MainMenuBar::OnCopy)
EVT_MENU(wxID_PASTE, MainMenuBar::OnPaste)
EVT_MENU(wxID_DELETE, MainMenuBar::OnDelete)
EVT_MENU(wxID_DUPLICATE, MainMenuBar::OnDuplicate)
EVT_MENU(wxID_SELECTALL, MainMenuBar::OnSelectAll)
EVT_MENU(wxID_HIGHEST + 10, MainMenuBar::OnRaiseSel)
EVT_MENU(wxID_HIGHEST + 11, MainMenuBar::OnLowerSel)
EVT_MENU(wxID_HIGHEST + 12, MainMenuBar::OnRaiseTop)
EVT_MENU(wxID_HIGHEST + 13, MainMenuBar::OnLowerBottom)
EVT_MENU(wxID_HIGHEST + 14, MainMenuBar::OnAddVertex)
EVT_MENU(wxID_HIGHEST + 15, MainMenuBar::OnRemoveVertex)

EVT_MENU(wxID_HIGHEST + 100, MainMenuBar::OnAddCircuit)
EVT_MENU(wxID_HIGHEST + 101, MainMenuBar::OnLoadLibrary)
EVT_MENU(wxID_HIGHEST + 102, MainMenuBar::OnLoadLibrary)
EVT_MENU(wxID_HIGHEST + 103, MainMenuBar::OnUnloadLibraries)
EVT_MENU(wxID_HIGHEST + 104, MainMenuBar::OnMoveCircuitUp)
EVT_MENU(wxID_HIGHEST + 105, MainMenuBar::OnMoveCircuitDown)
EVT_MENU(wxID_HIGHEST + 106, MainMenuBar::OnSetAsMainCircuit)
EVT_MENU(wxID_HIGHEST + 107, MainMenuBar::OnRemoveCircuit)
EVT_MENU(wxID_HIGHEST + 108, MainMenuBar::OnRevertAppearance)
EVT_MENU(wxID_HIGHEST + 109, MainMenuBar::OnViewToolbox)
EVT_MENU(wxID_HIGHEST + 110, MainMenuBar::OnViewSimTree)
EVT_MENU(wxID_HIGHEST + 111, MainMenuBar::OnEditLayout)
EVT_MENU(wxID_HIGHEST + 112, MainMenuBar::OnEditAppearance)
EVT_MENU(wxID_HIGHEST + 113, MainMenuBar::OnAnalyzeCircuit)
EVT_MENU(wxID_HIGHEST + 114, MainMenuBar::OnGetStats)
EVT_MENU(wxID_HIGHEST + 115, MainMenuBar::OnOptions)

EVT_MENU(wxID_HIGHEST + 200, MainMenuBar::OnSimEnable)
EVT_MENU(wxID_HIGHEST + 201, MainMenuBar::OnSimReset)
EVT_MENU(wxID_HIGHEST + 202, MainMenuBar::OnSimStep)
EVT_MENU(wxID_HIGHEST + 203, MainMenuBar::OnGoOutToState)
EVT_MENU(wxID_HIGHEST + 204, MainMenuBar::OnGoInToState)
EVT_MENU(wxID_HIGHEST + 205, MainMenuBar::OnTickOnce)
EVT_MENU(wxID_HIGHEST + 206, MainMenuBar::OnTicksEnabled)
EVT_MENU(wxID_HIGHEST + 207, MainMenuBar::OnSetTickFreq)
EVT_MENU(wxID_HIGHEST + 208, MainMenuBar::OnSetTickFreq)
EVT_MENU(wxID_HIGHEST + 209, MainMenuBar::OnSetTickFreq)
EVT_MENU(wxID_HIGHEST + 210, MainMenuBar::OnSetTickFreq)
EVT_MENU(wxID_HIGHEST + 211, MainMenuBar::OnSetTickFreq)
EVT_MENU(wxID_HIGHEST + 212, MainMenuBar::OnSetTickFreq)
EVT_MENU(wxID_HIGHEST + 213, MainMenuBar::OnLogging)

EVT_MENU(wxID_HIGHEST + 220, MainMenuBar::OnSimCompile)
EVT_MENU(wxID_HIGHEST + 221, MainMenuBar::OnSimRun)
EVT_MENU(wxID_HIGHEST + 222, MainMenuBar::OnSimClean)
EVT_MENU(wxID_HIGHEST + 223, MainMenuBar::OnSimCancel)

EVT_MENU(wxID_HIGHEST + 230, MainMenuBar::OnFpgaSynthesis)
EVT_MENU(wxID_HIGHEST + 231, MainMenuBar::OnFpgaRoute)
EVT_MENU(wxID_HIGHEST + 232, MainMenuBar::OnFpgaProgram)
EVT_MENU(wxID_HIGHEST + 233, MainMenuBar::OnFpgaCancelSynthesis)
EVT_MENU(wxID_HIGHEST + 234, MainMenuBar::OnFpgaPinBinding)
EVT_MENU(wxID_HIGHEST + 235, MainMenuBar::OnFpgaShowSynthesisJobs)
EVT_MENU(wxID_HIGHEST + 236, MainMenuBar::OnFpgaOpenSynthesisReport)
EVT_MENU(wxID_HIGHEST + 237, MainMenuBar::OnFpgaRetrySynthesis)
EVT_MENU(wxID_HIGHEST + 238, MainMenuBar::OnFpgaCancelRoute)
EVT_MENU(wxID_HIGHEST + 239, MainMenuBar::OnTraceBridge)
EVT_MENU(wxID_HIGHEST + 240, MainMenuBar::OnDebugContract)

EVT_MENU(wxID_ICONIZE_FRAME, MainMenuBar::OnMinimize)
EVT_MENU(wxID_MAXIMIZE_FRAME, MainMenuBar::OnMaximize)
EVT_MENU(wxID_CLOSE_FRAME, MainMenuBar::OnCloseWnd)
EVT_MENU(wxID_HIGHEST + 300, MainMenuBar::OnCombinationalAnalysis)
EVT_MENU(wxID_HIGHEST + 301, MainMenuBar::OnWindowPreferences)
EVT_MENU(idThemeFollowSystem, MainMenuBar::OnThemeFollowSystem)
EVT_MENU(idThemeLight, MainMenuBar::OnThemeLight)
EVT_MENU(idThemeDark, MainMenuBar::OnThemeDark)
EVT_MENU(idPanelMenuBar, MainMenuBar::OnToggleMenuBar)
EVT_MENU(idPanelRail, MainMenuBar::OnToggleRail)
EVT_MENU(idPanelLeft, MainMenuBar::OnToggleLeftBar)
EVT_MENU(idPanelRight, MainMenuBar::OnToggleRightBar)
EVT_MENU(idPanelBottom, MainMenuBar::OnToggleBottomPanel)
EVT_MENU(idPanelStatusBar, MainMenuBar::OnToggleStatusBar)
EVT_MENU(idPresetClassic, MainMenuBar::OnApplyLayoutPreset)
EVT_MENU(idPresetCode, MainMenuBar::OnApplyLayoutPreset)
EVT_MENU(idPresetCanvas, MainMenuBar::OnApplyLayoutPreset)
EVT_MENU(idPresetChat, MainMenuBar::OnApplyLayoutPreset)
EVT_MENU(idSaveLayout, MainMenuBar::OnSaveLayout)
EVT_MENU(idResetLayout, MainMenuBar::OnResetLayout)
EVT_MENU(idCustomizeLayout, MainMenuBar::OnCustomizeLayout)
EVT_MENU(idFloatSidePanel, MainMenuBar::OnFloatSidePanel)
EVT_MENU(idFloatLeftPanel, MainMenuBar::OnFloatLeftPanel)
EVT_MENU(idFloatBottomPanel, MainMenuBar::OnFloatBottomPanel)

EVT_MENU(wxID_HIGHEST + 500, MainMenuBar::OnTutorial)
EVT_MENU(wxID_HIGHEST + 501, MainMenuBar::OnUserGuide)
EVT_MENU(wxID_HIGHEST + 502, MainMenuBar::OnLibraryRef)
EVT_MENU(wxID_ABOUT, MainMenuBar::OnAbout)
EVT_MENU_OPEN(MainMenuBar::OnMenuOpen)
wxEND_EVENT_TABLE()

MainMenuBar::MainMenuBar(MainFrame* owner)
    : wxMenuBar(),
    m_owner(owner),
    m_fileHistory(9),
    m_windowMenu(nullptr),
    m_helpMenu(nullptr),
    m_helpBackendsMenu(nullptr)
{
    /* 1. 创建并追加六大菜单 */
    wxMenu* fileMenu = CreateFileMenu();   // 先拿到指针
    Append(fileMenu, wxT("&File"));
    Append(CreateEditMenu(), wxT("&Edit"));
    Append(CreateProjectMenu(), wxT("&Project"));
    Append(CreateSimulateMenu(), wxT("&Simulate"));
    Append(CreateFpgaMenu(), wxT("&FPGA"));
    RebuildWindowMenu();  
    Append(CreateHelpMenu(), wxT("&Help"));

    /* 2. 最近文件列表挂在 File 菜单上 */
    m_fileHistory.UseMenu(fileMenu);          // 直接用指针，不再 FindItem
    m_fileHistory.Load(*wxConfig::Get());

    /* 3. 首次重建 Window 菜单 */
    RebuildWindowMenu();
}

MainMenuBar::~MainMenuBar()
{
    if (m_fileHistory.GetCount() != 0) {
        SaveHistory();
    }
}

/* ---------- 最近文件列表封装 ---------- */
void MainMenuBar::AddFileToHistory(const wxString& path)
{
    m_fileHistory.AddFileToHistory(path);
}
wxString MainMenuBar::GetHistoryFile(size_t i) const
{
    return m_fileHistory.GetHistoryFile(i);
}
size_t MainMenuBar::GetHistoryFileCount() const
{
    return m_fileHistory.GetCount();
}
void MainMenuBar::LoadHistory()
{
    // 必须与启动窗口、以及菜单构造里的 m_fileHistory.Load(*wxConfig::Get())
    // 使用**同一个** config 身份。
    // 旧实现这里硬编码 "MyLogisim"，而启动窗口用 "Sigflow"、菜单构造用默认 app
    // config —— 三套存储互不相通：在启动窗口删掉的最近项目会在 File 菜单里复活，
    // 菜单列表也永远保存不下来。应用名由 cMain 的 SetAppName("Sigflow") 统一设定。
    m_fileHistory.Load(*wxConfig::Get());
}
void MainMenuBar::SaveHistory()
{
    m_fileHistory.Save(*wxConfig::Get());
}

/* ---------- 六大菜单创建 ---------- */
wxMenu* MainMenuBar::CreateFileMenu()
{
    wxMenu* menu = new wxMenu;
    menu->Append(idOpenProj, "&Open Project");

    menu->AppendSeparator();
    menu->Append(wxID_NEW, "&New\tCtrl+N", "Create a new circuit");
    menu->Append(wxID_OPEN, "&Open\tCtrl+O", "Open an existing circuit");

    wxMenu* recentMenu = new wxMenu;
    menu->AppendSubMenu(recentMenu, "Open Recent");
    m_fileHistory.UseMenu(recentMenu);

    menu->AppendSeparator();
    menu->Append(wxID_CLOSE, "&Close\tCtrl+Shift+W", "Close current window");
    menu->Append(wxID_SAVE, "&Save\tCtrl+S", "Save current circuit");
    menu->Append(wxID_SAVEAS, "Save &As...\tCtrl+Shift+S", "Save with new name");

    menu->Append(idSaveAsNode, "Save as .node", "Save nodes to .node file");
    menu->Append(idSaveAsNet, "Save as .net", "Save network to .net file");

    menu->AppendSeparator();
    menu->Append(idExportImage, "Export Image...", "Export canvas to PNG/JPEG");
    menu->Append(wxID_PRINT, "&Print...\tCtrl+P", "Print circuit");

    menu->AppendSeparator();
    menu->Append(wxID_PREFERENCES, "Preferences...", "Open settings");

    menu->AppendSeparator();
    menu->Append(wxID_EXIT, "E&xit\tCtrl+Q", "Quit application");
    return menu;
}

wxMenu* MainMenuBar::CreateEditMenu()
{
    wxMenu* m = new wxMenu;

    wxMenuItem* undoItem = new wxMenuItem(m, wxID_UNDO, "&Can't Undo");
    m->Append(undoItem);
    undoItem->Enable(false);  // 必须在 Append 之后才能 Enable（否则 wxGTK 断言 invalid menu item）
    m->AppendSeparator();

    m->Append(wxID_CUT, "Cu&t\t");
    m->Append(wxID_COPY, "&Copy\t");
    m->Append(wxID_PASTE, "&Paste\t");
    m->AppendSeparator();

    m->Append(wxID_DELETE, "&Delete\tDel");
    m->Append(wxID_DUPLICATE, "D&uplicate\tCtrl+D");
    m->Append(wxID_SELECTALL, "Select &All\tCtrl+A");
    m->AppendSeparator();

    /* 用 wxID_HIGHEST+10 起步，避免与系统 ID 冲突 */
    m->Append(wxID_HIGHEST + 10, "Raise Selection\tCtrl+UP");
    m->Append(wxID_HIGHEST + 11, "Lower Selection\tCtrl+DOWN");
    m->Append(wxID_HIGHEST + 12, "Raise to Top\tCtrl+Shift+UP");
    m->Append(wxID_HIGHEST + 13, "Lower to Bottom\tCtrl+Shift+DOWN");
    m->AppendSeparator();

    m->Append(wxID_HIGHEST + 14, "Add Vertex");
    m->Append(wxID_HIGHEST + 15, "Remove Vertex");

    return m;
}

wxMenu* MainMenuBar::CreateProjectMenu()
{
    wxMenu* m = new wxMenu;

    /* 1. 库管理 */
    m->Append(wxID_HIGHEST + 100, "Add Circuit...");
    wxMenu* loadSub = new wxMenu;
    loadSub->Append(wxID_HIGHEST + 101, "From Built-in Libraries");
    loadSub->Append(wxID_HIGHEST + 102, "From Jar/Directory");
    m->AppendSubMenu(loadSub, "Load Library");
    m->Append(wxID_HIGHEST + 103, "Unload Libraries...");
    m->AppendSeparator();

    /* 2. 电路排序与属性 */
    m->Append(wxID_HIGHEST + 104, "Move Circuit Up");
    m->Append(wxID_HIGHEST + 105, "Move Circuit Down");
    m->Append(wxID_HIGHEST + 106, "Set As Main Circuit");
    m->Append(wxID_HIGHEST + 107, "Remove Circuit");
    m->Append(wxID_HIGHEST + 108, "Revert to Default Appearance");
    m->AppendSeparator();

    /* 3. 视图与编辑 */
    m->Append(wxID_HIGHEST + 109, "View Toolbox");
    m->Append(wxID_HIGHEST + 110, "View Simulation Tree");
    m->Append(wxID_HIGHEST + 111, "Edit Circuit Layout");
    m->Append(wxID_HIGHEST + 112, "Edit Circuit Appearance");
    m->AppendSeparator();

    /* 4. 分析与统计 */
    m->Append(wxID_HIGHEST + 113, "Analyze Circuit");
    m->Append(wxID_HIGHEST + 114, "Get Circuit Statistics");
    m->AppendSeparator();

    /* 5. 项目级选项 */
    m->Append(wxID_HIGHEST + 115, "Options...");

    return m;
}

wxMenu* MainMenuBar::CreateSimulateMenu()
{
    wxMenu* m = new wxMenu;

    /* Verilator 仿真 */
    m->Append(wxID_HIGHEST + 220, wxT("Compile Simulation Model\tF5"), wxT("使用Verilator编译当前Verilog文件"));
    m->Append(wxID_HIGHEST + 221, wxT("Run Simulation\tF6"), wxT("运行仿真并生成波形"));
    m->Append(wxID_HIGHEST + 222, wxT("Clean Simulation Cache"), wxT("清理仿真编译缓存"));
    m->Append(wxID_HIGHEST + 223, wxT("Cancel Simulation\tShift+F5"), wxT("终止当前仿真作业（杀进程树）"));
    m->AppendSeparator();

    m->AppendCheckItem(wxID_HIGHEST + 200, "Simulation Enabled\tCtrl+E");
    m->Append(wxID_HIGHEST + 201, "Reset Simulation\tCtrl+R");
    m->Append(wxID_HIGHEST + 202, "Step Simulation\tCtrl+I");
    m->AppendSeparator();

    /* 状态跳转子菜单 */
    wxMenu* gotoMenu = new wxMenu;
    gotoMenu->Append(wxID_HIGHEST + 203, "Go Out To State");
    gotoMenu->Append(wxID_HIGHEST + 204, "Go In To State");
    m->AppendSubMenu(gotoMenu, "Go To State");
    m->AppendSeparator();

    /* 时钟控制 */
    m->Append(wxID_HIGHEST + 205, "Tick Once\tCtrl+T");
    m->AppendCheckItem(wxID_HIGHEST + 206, "Ticks Enabled\tCtrl+K");
    wxMenu* freqMenu = new wxMenu;
    freqMenu->Append(wxID_HIGHEST + 207, "1 Hz");
    freqMenu->Append(wxID_HIGHEST + 208, "2 Hz");
    freqMenu->Append(wxID_HIGHEST + 209, "4 Hz");
    freqMenu->Append(wxID_HIGHEST + 210, "8 Hz");
    freqMenu->Append(wxID_HIGHEST + 211, "16 Hz");
    freqMenu->Append(wxID_HIGHEST + 212, "32 Hz");
    m->AppendSubMenu(freqMenu, "Tick Frequency");
    m->AppendSeparator();

    /* 日志 */
    m->Append(wxID_HIGHEST + 213, "Logging...");

    return m;
}

wxMenu* MainMenuBar::CreateFpgaMenu()
{
    wxMenu* menu = new wxMenu;
    menu->Append(wxID_HIGHEST + 230, wxT("FPGA Tools..."),
                 wxT("Open Yosys, nextpnr, and openFPGALoader controls"));
    menu->Append(wxID_HIGHEST + 234, wxT("Pin Binding..."),
                 wxT("Configure FPGA pin constraints for the target device"));
    menu->Append(wxID_HIGHEST + 240, wxT("debug_contract..."),
                 wxT("Create and edit the project debug-contract.json"));
    menu->AppendSeparator();
    menu->Append(wxID_HIGHEST + 239, wxT("TraceBridge Hardware Capture..."),
                 wxT("Capture a programmed debug bitstream through UART"));
    return menu;
}

void MainMenuBar::RebuildWindowMenu() //这个版本会把window放在simulate前面
{
    /* 如果已存在则先销毁旧项（重建用） */
    if (m_windowMenu)
        Remove(3);   // Window 菜单在索引 3 位置（File/Edit/Project之后）

    m_windowMenu = new wxMenu;

    /* 1. 常规窗口命令 */
    m_windowMenu->Append(wxID_ICONIZE_FRAME, "Minimize\tCtrl+M");
    m_windowMenu->Append(wxID_MAXIMIZE_FRAME, "Maximize");
    m_windowMenu->Append(wxID_CLOSE_FRAME, "Close\tCtrl+W");
    m_windowMenu->AppendSeparator();

    /* 2. 内置工具 */
    m_windowMenu->Append(wxID_HIGHEST + 300, "Combinational Analysis");
    m_windowMenu->Append(wxID_HIGHEST + 301, "Preferences");
    m_windowMenu->AppendSeparator();

    /* 2b. 主题（T-120；SPEC THE-006 三态、THE-007 入口、THE-008 持久化键）
     *     设计稿的工具栏循环按钮受 DEC-03（工具栏内容不变）与 DEC-05（不自绘标题栏）约束未实现，
     *     此处以 Window 菜单三态单选作为等价入口；循环语义保留在代码中（MainFrame::SetThemeMode）。
     *     DEC-11（2026-10-10 用户实测）：深色档暂缓 —— 面板/画布/子窗口尚未 token 化，
     *     切深色会"部分变黑、部分仍白"。因此深色未启用时**不显示本子菜单**，避免用户踩到半成品；
     *     待 T-310/T-330/T-340/T-410/T-140 完成后 UiTheme::IsDarkModeEnabled() 转为 true，入口自动恢复。 */
    if (UiTheme::IsDarkModeEnabled()) {
        wxMenu* themeMenu = new wxMenu;
        themeMenu->AppendRadioItem(idThemeFollowSystem, "Follow System",
                                   "跟随系统主题（系统切换时自动跟随）");
        themeMenu->AppendRadioItem(idThemeLight, "Light", "浅色档");
        themeMenu->AppendRadioItem(idThemeDark, "Dark", "深色档");
        m_windowMenu->AppendSubMenu(themeMenu, "Theme");
        m_windowMenu->AppendSeparator();
    }

    /* 2c. 六栏显隐（T-510；SPEC LAY-020…LAY-024、KEY-002）
     *     快捷键沿用本仓既有机制（菜单标签 `\t` 后），不新引入 wxAcceleratorTable，
     *     从而与既有 24 条快捷键共用同一套语义与冲突检查（KEY-003/KEY-005）。
     *     状态栏无快捷键（设计稿 app.html:815 亦无 kbd）。 */
    {
        wxMenu* panelsMenu = new wxMenu;
        panelsMenu->AppendCheckItem(idPanelMenuBar, "Menu Bar\tCtrl+Shift+M", "显示 / 隐藏菜单栏");
        panelsMenu->AppendCheckItem(idPanelRail, "Activity Bar\tCtrl+Shift+A", "显示 / 隐藏活动栏");
        panelsMenu->AppendCheckItem(idPanelLeft, "Primary Side Bar\tCtrl+B", "显示 / 隐藏主侧栏");
        panelsMenu->AppendCheckItem(idPanelRight, "Secondary Side Bar\tCtrl+Alt+B", "显示 / 隐藏辅助侧边栏");
        panelsMenu->AppendCheckItem(idPanelBottom, "Panel\tCtrl+J", "显示 / 隐藏底部面板");
        panelsMenu->AppendCheckItem(idPanelStatusBar, "Status Bar", "显示 / 隐藏状态栏");
        m_windowMenu->AppendSubMenu(panelsMenu, "Panels");
        m_windowMenu->AppendSeparator();
    }

    /* 2d. 布局预设（T-510；SPEC LAY-025）
     *     设计稿把这四个预设放在 Quick Pick 浮层的顶部；浮层本体尚未实现（LAY-026/027 登记为待做），
     *     因此先用菜单提供**同等功能**：勾选态按"当前状态与预设全字段相等"判定（LAY-025 的判据）。 */
    {
        wxMenu* presetsMenu = new wxMenu;
        presetsMenu->AppendCheckItem(idPresetClassic, "Classic",
                                     "经典：六栏全显（设计稿「经典」）");
        presetsMenu->AppendCheckItem(idPresetCode, "Code First",
                                     "编码优先：隐藏活动栏（设计稿「编码优先」）");
        presetsMenu->AppendCheckItem(idPresetCanvas, "Canvas First",
                                     "画布优先：隐藏活动栏与主侧栏（设计稿「画布优先」）");
        presetsMenu->AppendCheckItem(idPresetChat, "Chat Detached",
                                     "Chat 分离：隐藏辅助侧边栏（设计稿「Chat 分离」；"
                                     "本轮不含跨显示器拖出，见 DEC-06）");
        m_windowMenu->AppendSubMenu(presetsMenu, "Layout Presets");
        m_windowMenu->AppendSeparator();
    }

    /* 2e. 布局保存 / 重置（T-250；SPEC LAY-031/LAY-032）
     *     Reset 只重置布局（pane / 显隐 / perspective），**不重建任何业务对象、不清空 Job、
     *     不重启 Agent**（SPEC LAY-032 与 docs/gui P2-2 的明确要求）。 */
    {
        m_windowMenu->Append(idSaveLayout, "Save Layout", "把当前布局与窗口几何写入应用配置");
        m_windowMenu->Append(idCustomizeLayout, "Customize Layout...", "Open the keyboard-accessible layout picker");
        m_windowMenu->Append(idResetLayout, "Reset Layout", "恢复默认布局（不触碰业务状态）");
        m_windowMenu->AppendSeparator();
        // T-730：显式浮动/收回。wxAUI 只能由 caption 起拖，而 caption 在部分构建下不可见/不可命中
        //（实测 build-local(Debug) 拖不动、build-release-size(Release) 正常）→ 给出稳定的菜单入口。
        m_windowMenu->Append(idFloatSidePanel, "Float / Dock Side Panel",
                             "把右侧栏弹出为浮动窗（或收回停靠）—— 不依赖标题条拖拽");
        m_windowMenu->Append(idFloatLeftPanel, "Float / Dock Project Manager",
                             "把左侧栏弹出为浮动窗（或收回停靠）");
        m_windowMenu->Append(idFloatBottomPanel, "Float / Dock Console",
                             "把底部面板弹出为浮动窗（或收回停靠）");
        m_windowMenu->AppendSeparator();
    }

    /* 3. 动态文档列表（初始 Untitled） */
    AddDocToWindowList(m_curDocTitle.IsEmpty() ? wxString("Untitled") : m_curDocTitle);
    SetCurrentDocInWindowList(m_curDocTitle.IsEmpty() ? wxString("Untitled") : m_curDocTitle);

    /* 4. 重新插入菜单栏 */
    Insert(3, m_windowMenu, wxT("&Window"));

    /* 5. 同步主题单选态与六栏勾选态（新建菜单项后必须回填） */
    UpdateThemeMenuChecks();
    UpdatePanelMenuChecks();
}

// T-510：六栏显隐勾选态同步（LAY-023/LAY-025、INT-020）
void MainMenuBar::UpdatePanelMenuChecks()
{
    if (m_windowMenu == nullptr || m_owner == nullptr) {
        return;
    }
    const struct
    {
        int id;
        UiTheme::Visibility which;
    } items[] = {
        { idPanelMenuBar, UiTheme::Visibility::Menubar },
        { idPanelRail, UiTheme::Visibility::Rail },
        { idPanelLeft, UiTheme::Visibility::Left },
        { idPanelRight, UiTheme::Visibility::Right },
        { idPanelBottom, UiTheme::Visibility::Bottom },
        { idPanelStatusBar, UiTheme::Visibility::Statusbar },
    };
    for (const auto& entry : items) {
        if (wxMenuItem* item = m_windowMenu->FindItem(entry.id)) {
            item->Check(m_owner->IsPanelVisible(entry.which));
        }
    }

    // T-510 / LAY-025：预设勾选态 = "当前状态与预设**全字段**相等"才点亮（与设计稿 app.html:1636 同语义）
    const struct
    {
        int id;
        const char* preset;
    } presets[] = {
        { idPresetClassic, UiTheme::Presets::Classic },
        { idPresetCode, UiTheme::Presets::Code },
        { idPresetCanvas, UiTheme::Presets::Canvas },
        { idPresetChat, UiTheme::Presets::Chat },
    };
    for (const auto& entry : presets) {
        if (wxMenuItem* item = m_windowMenu->FindItem(entry.id)) {
            item->Check(UiTheme::MatchesPreset(m_owner->GetLayoutState(), entry.preset));
        }
    }
}

// T-510：六栏显隐（菜单与快捷键共用一个入口）
void MainMenuBar::OnToggleMenuBar(wxCommandEvent&)
{
    if (m_owner != nullptr) m_owner->TogglePanelVisibility(UiTheme::Visibility::Menubar);
}

void MainMenuBar::OnToggleRail(wxCommandEvent&)
{
    if (m_owner != nullptr) m_owner->TogglePanelVisibility(UiTheme::Visibility::Rail);
}

void MainMenuBar::OnToggleLeftBar(wxCommandEvent&)
{
    if (m_owner != nullptr) m_owner->TogglePanelVisibility(UiTheme::Visibility::Left);
}

void MainMenuBar::OnToggleRightBar(wxCommandEvent&)
{
    if (m_owner != nullptr) m_owner->TogglePanelVisibility(UiTheme::Visibility::Right);
}

void MainMenuBar::OnToggleBottomPanel(wxCommandEvent&)
{
    if (m_owner != nullptr) m_owner->TogglePanelVisibility(UiTheme::Visibility::Bottom);
}

void MainMenuBar::OnToggleStatusBar(wxCommandEvent&)
{
    if (m_owner != nullptr) m_owner->TogglePanelVisibility(UiTheme::Visibility::Statusbar);
}

// T-510：布局预设（LAY-025）。四个预设共用同一处理器，按 id 映射到预设常量。
void MainMenuBar::OnApplyLayoutPreset(wxCommandEvent& evt)
{
    if (m_owner == nullptr) {
        return;
    }
    wxString presetId;
    switch (evt.GetId()) {
        case idPresetClassic: presetId = UiTheme::Presets::Classic; break;
        case idPresetCode:    presetId = UiTheme::Presets::Code; break;
        case idPresetCanvas:  presetId = UiTheme::Presets::Canvas; break;
        case idPresetChat:    presetId = UiTheme::Presets::Chat; break;
        default: return;
    }
    m_owner->ApplyLayoutPreset(presetId);
}

void MainMenuBar::OnSaveLayout(wxCommandEvent&)
{
    if (m_owner != nullptr) {
        m_owner->SaveLayoutNow();
    }
}

void MainMenuBar::OnResetLayout(wxCommandEvent&)
{
    if (m_owner != nullptr) {
        m_owner->ResetLayout();
    }
}

void MainMenuBar::OnCustomizeLayout(wxCommandEvent&) { if(m_owner) m_owner->ShowLayoutPicker(); }

// T-730：显式浮动/收回面板（不依赖 caption 拖拽）。
// 背景：wxAUI 只能由 caption part 起拖，而 caption 在部分构建下不可见/不可命中
// （实测 build-local(Debug) 拖不动、build-release-size(Release) 正常），故给出菜单入口。
void MainMenuBar::OnFloatSidePanel(wxCommandEvent&) { if (m_owner) m_owner->TogglePaneFloating("right_sidebar"); }
void MainMenuBar::OnFloatLeftPanel(wxCommandEvent&) { if (m_owner) m_owner->TogglePaneFloating("left_sidebar"); }
void MainMenuBar::OnFloatBottomPanel(wxCommandEvent&) { if (m_owner) m_owner->TogglePaneFloating("bottom_tabs"); }

// T-120：主题菜单单选态同步
void MainMenuBar::UpdateThemeMenuChecks()
{
    if (m_windowMenu == nullptr) {
        return;
    }
    int target = idThemeFollowSystem;
    switch (UiTheme::GetMode()) {
        case UiTheme::Mode::Light: target = idThemeLight; break;
        case UiTheme::Mode::Dark: target = idThemeDark; break;
        case UiTheme::Mode::System: break;
    }
    if (wxMenuItem* item = m_windowMenu->FindItem(target)) {
        item->Check(true);
    }
}

// T-120：三个主题入口（显式选择后系统变化不再覆盖，SPEC THE-006）
void MainMenuBar::OnThemeFollowSystem(wxCommandEvent&)
{
    if (m_owner != nullptr) {
        m_owner->SetThemeMode(UiTheme::Mode::System);
    }
}

void MainMenuBar::OnThemeLight(wxCommandEvent&)
{
    if (m_owner != nullptr) {
        m_owner->SetThemeMode(UiTheme::Mode::Light);
    }
}

void MainMenuBar::OnThemeDark(wxCommandEvent&)
{
    if (m_owner != nullptr) {
        m_owner->SetThemeMode(UiTheme::Mode::Dark);
    }
}

//void MainMenuBar::RebuildWindowMenu()
//{
//    /* 1. 如果已存在，按名字找到并删除，避免索引错位 */
//    int oldPos = FindMenu(wxT("&Window"));
//    if (oldPos != wxNOT_FOUND)
//        Remove(oldPos);
//
//    /* 2. 重建菜单内容 */
//    m_windowMenu = new wxMenu;
//    m_windowMenu->Append(wxID_ICONIZE_FRAME, "Minimize\tCtrl+M");
//    m_windowMenu->Append(wxID_MAXIMIZE_FRAME, "Maximize");
//    m_windowMenu->Append(wxID_CLOSE_FRAME, "Close\tCtrl+W");
//    m_windowMenu->AppendSeparator();
//
//    m_windowMenu->Append(wxID_HIGHEST + 300, "Combinational Analysis");
//    m_windowMenu->Append(wxID_HIGHEST + 301, "Preferences");
//    m_windowMenu->AppendSeparator();
//
//    AddDocToWindowList(m_curDocTitle.IsEmpty() ? wxT("Untitled") : m_curDocTitle);
//    SetCurrentDocInWindowList(m_curDocTitle.IsEmpty() ? wxT("Untitled") : m_curDocTitle);
//
//    /* 3. 插在 Help 之前（Simulate 之后自然就是 Window） */
//    int helpPos = FindMenu(wxT("&Help"));
//    Insert(helpPos, m_windowMenu, wxT("&Window"));
//}

wxMenu* MainMenuBar::CreateHelpMenu()
{
    wxMenu* m = new wxMenu;
    m->Append(wxID_HIGHEST + 500, wxT("Tutorial"));
    m->Append(wxID_HIGHEST + 501, wxT("User's Guide"));
    m->Append(wxID_HIGHEST + 502, wxT("Library Reference"));
    m->AppendSeparator();
    // P1-8：工具链后端选择（能力 → 插件）放在 Help 下拉栏；先建空子菜单，
    // 待 Help 菜单打开时（插件已加载）再由 OnMenuOpen → RebuildToolchainBackendsMenu 填充。
    m_helpBackendsMenu = new wxMenu;
    m->AppendSubMenu(m_helpBackendsMenu, wxT("Toolchain Backends"));
    m->AppendSeparator();
    m->Append(wxID_ABOUT, wxT("About..."));
    m_helpMenu = m;
    return m;
}

// P1-8：按当前已加载插件重建 Toolchain Backends 子菜单。
void MainMenuBar::RebuildToolchainBackendsMenu()
{
    if (m_helpBackendsMenu == nullptr) return;
    // 清空旧项（含子菜单对象）。
    while (m_helpBackendsMenu->GetMenuItemCount() > 0) {
        m_helpBackendsMenu->Delete(m_helpBackendsMenu->FindItemByPosition(0));
    }
    if (m_owner == nullptr) return;
    const std::vector<std::string> capabilities = m_owner->AvailableToolCapabilities();
    if (capabilities.empty()) {
        m_helpBackendsMenu->Append(wxID_ANY, wxT("(no toolchain backends found)"))->Enable(false);
        return;
    }
    // 复用 MainFrame 的构建逻辑（它负责按能力生成单选项并绑定切换）。
    m_owner->BuildToolchainBackendsMenu(m_helpBackendsMenu);
}

void MainMenuBar::OnMenuOpen(wxMenuEvent& event)
{
    // Help 菜单打开时懒重建后端子菜单（此时插件已加载）。
    if (m_helpMenu != nullptr && event.GetMenu() == m_helpMenu) {
        RebuildToolchainBackendsMenu();
    }
    event.Skip();
}

/* ---------- File 菜单事件转发 ---------- */
void MainMenuBar::OnFileOpenProject(wxCommandEvent&) {
    if (m_owner) {
        m_owner->DoFileOpenProject();
    }
}

void MainMenuBar::OnFileNew(wxCommandEvent&)
{
    m_owner->DoFileNew();
}
void MainMenuBar::OnFileOpen(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoFileOpen();
    }
}
void MainMenuBar::OnFileClose(wxCommandEvent&)
{
    m_owner->Close();  // 直接关窗口，后续可改
}
void MainMenuBar::OnFileSave(wxCommandEvent&)
{
    m_owner->DoFileSave();
}
void MainMenuBar::OnFileSaveAs(wxCommandEvent&)
{
    m_owner->DoFileSaveAs();
}
void MainMenuBar::OnSaveAsNode(wxCommandEvent&)
{
    if (m_owner)
        m_owner->DoFileSaveAsNode();
}

void MainMenuBar::OnSaveAsNet(wxCommandEvent&)
{
    if (m_owner)
        m_owner->DoFileSaveAsNet();
}
void MainMenuBar::OnExportImage(wxCommandEvent&)
{
    wxMessageBox("Export Image placeholder", "Info");
}
void MainMenuBar::OnPrint(wxCommandEvent&)
{
    wxMessageBox("Print placeholder", "Info");
}
void MainMenuBar::OnPreferences(wxCommandEvent&)
{
    wxMessageBox("Preferences placeholder", "Info");
}
void MainMenuBar::OnExit(wxCommandEvent&)
{
    if (m_fileHistory.GetCount() != 0) {
        SaveHistory();
    }
    if (m_owner) {
        m_owner->Close();
    }
}
void MainMenuBar::OnFileHistory(wxCommandEvent& evt)
{
    const int idx = evt.GetId() - wxID_FILE1;

    // 必须做边界检查。
    // EVT_MENU_RANGE 只保证 id 落在 [wxID_FILE1, wxID_FILE9]，并不保证
    // 该下标在内部历史数组里存在：菜单项与 wxFileHistory 的内部数组一旦不同步
    //（例如历史被 Load() 重建而菜单没有跟着重建），wxFileHistory::GetHistoryFile(idx)
    // 就会用越界下标去访问内部数组，直接在 wx 内部触发断言。
    if (idx < 0 || static_cast<size_t>(idx) >= m_fileHistory.GetCount()) {
        return;
    }

    const wxString path = m_fileHistory.GetHistoryFile(idx);
    if (path.IsEmpty()) {
        return;
    }

    m_owner->DoFileOpen(path);  // 带路径打开

    // "提到最前" 会 insert/delete 菜单项（wxFileHistory::AddFileToHistory 内部会
    // 调用 m_fileMenus 里各菜单的 Append/SetLabel/Delete 来重排 MRU 项）。
    // 而此刻我们正处在**这个菜单自己的事件处理过程中** —— 在 wxWidgets 里
    // 于菜单事件里修改该菜单是明确禁忌：菜单项数量与 wxFileHistory 内部数组
    // 一旦错位，后续的 SetLabel/Delete（以及内部的 RemoveAt）就会用错误下标。
    // 因此把这次历史更新推迟到本次事件处理结束之后再做。
    if (m_owner) {
        m_owner->CallAfter([this, path]() { AddFileToHistory(path); });
    }
}

void MainMenuBar::OnFpgaSynthesis(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoFpgaSynthesis();
    }
}

void MainMenuBar::OnFpgaCancelSynthesis(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoFpgaCancelSynthesis();
    }
}

void MainMenuBar::OnFpgaShowSynthesisJobs(wxCommandEvent&)
{
    if (m_owner) m_owner->DoFpgaShowSynthesisJobs();
}

void MainMenuBar::OnFpgaOpenSynthesisReport(wxCommandEvent&)
{
    if (m_owner) m_owner->DoFpgaOpenSynthesisReport();
}

void MainMenuBar::OnFpgaRetrySynthesis(wxCommandEvent&)
{
    if (m_owner) m_owner->DoFpgaRetrySynthesis();
}

void MainMenuBar::OnFpgaRoute(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoFpgaRoute();
    }
}

void MainMenuBar::OnFpgaCancelRoute(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoFpgaCancelRoute();
    }
}

void MainMenuBar::OnFpgaProgram(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoFpgaProgram();
    }
}

void MainMenuBar::OnFpgaPinBinding(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoFpgaPinBinding();
    }
}

void MainMenuBar::OnTraceBridge(wxCommandEvent&)
{
    if (m_owner) {
        m_owner->DoTraceBridge();
    }
}

void MainMenuBar::OnDebugContract(wxCommandEvent&)
{
    if (m_owner) m_owner->DoDebugContract();
}

void MainMenuBar::OnUndo(wxCommandEvent&) { m_owner->DoEditUndo(); }
void MainMenuBar::OnCut(wxCommandEvent&) { m_owner->DoEditCut(); }
void MainMenuBar::OnCopy(wxCommandEvent&) { m_owner->DoEditCopy(); }
void MainMenuBar::OnPaste(wxCommandEvent&) { m_owner->DoEditPaste(); }
void MainMenuBar::OnDelete(wxCommandEvent&) { m_owner->DoEditDelete(); }
void MainMenuBar::OnDuplicate(wxCommandEvent&) { m_owner->DoEditDuplicate(); }
void MainMenuBar::OnSelectAll(wxCommandEvent&) { m_owner->DoEditSelectAll(); }
void MainMenuBar::OnRaiseSel(wxCommandEvent&) { m_owner->DoEditRaiseSel(); }
void MainMenuBar::OnLowerSel(wxCommandEvent&) { m_owner->DoEditLowerSel(); }
void MainMenuBar::OnRaiseTop(wxCommandEvent&) { m_owner->DoEditRaiseTop(); }
void MainMenuBar::OnLowerBottom(wxCommandEvent&) { m_owner->DoEditLowerBottom(); }
void MainMenuBar::OnAddVertex(wxCommandEvent&) { m_owner->DoEditAddVertex(); }
void MainMenuBar::OnRemoveVertex(wxCommandEvent&) { m_owner->DoEditRemoveVertex(); }


void MainMenuBar::OnAddCircuit(wxCommandEvent&) { m_owner->DoProjectAddCircuit(); }
void MainMenuBar::OnLoadLibrary(wxCommandEvent&) { m_owner->DoProjectLoadLibrary(); }
void MainMenuBar::OnUnloadLibraries(wxCommandEvent&) { m_owner->DoProjectUnloadLibraries(); }
void MainMenuBar::OnMoveCircuitUp(wxCommandEvent&) { m_owner->DoProjectMoveCircuitUp(); }
void MainMenuBar::OnMoveCircuitDown(wxCommandEvent&) { m_owner->DoProjectMoveCircuitDown(); }
void MainMenuBar::OnSetAsMainCircuit(wxCommandEvent&) { m_owner->DoProjectSetAsMain(); }
void MainMenuBar::OnRemoveCircuit(wxCommandEvent&) { m_owner->DoProjectRemoveCircuit(); }
void MainMenuBar::OnRevertAppearance(wxCommandEvent&) { m_owner->DoProjectRevertAppearance(); }
void MainMenuBar::OnViewToolbox(wxCommandEvent&) { m_owner->DoProjectViewToolbox(); }
void MainMenuBar::OnViewSimTree(wxCommandEvent&) { m_owner->DoProjectViewSimTree(); }
void MainMenuBar::OnEditLayout(wxCommandEvent&) { m_owner->DoProjectEditLayout(); }
void MainMenuBar::OnEditAppearance(wxCommandEvent&) { m_owner->DoProjectEditAppearance(); }
void MainMenuBar::OnAnalyzeCircuit(wxCommandEvent&) { m_owner->DoProjectAnalyzeCircuit(); }
void MainMenuBar::OnGetStats(wxCommandEvent&) { m_owner->DoProjectGetStats(); }
void MainMenuBar::OnOptions(wxCommandEvent&) { m_owner->DoProjectOptions(); }


void MainMenuBar::OnSimEnable(wxCommandEvent& evt)
{
    m_owner->DoSimSetEnabled(evt.IsChecked());
}
void MainMenuBar::OnSimReset(wxCommandEvent&) { m_owner->DoSimReset(); }
void MainMenuBar::OnSimStep(wxCommandEvent&) { m_owner->DoSimStep(); }
void MainMenuBar::OnGoOutToState(wxCommandEvent&) { m_owner->DoSimGoOut(); }
void MainMenuBar::OnGoInToState(wxCommandEvent&) { m_owner->DoSimGoIn(); }
void MainMenuBar::OnTickOnce(wxCommandEvent&) { m_owner->DoSimTickOnce(); }
void MainMenuBar::OnTicksEnabled(wxCommandEvent& evt)
{
    m_owner->DoSimTicksEnabled(evt.IsChecked());
}
void MainMenuBar::OnSetTickFreq(wxCommandEvent& evt)
{
    int hz = 1 << (evt.GetId() - wxID_HIGHEST - 207);   // 1,2,4,8,16,32
    m_owner->DoSimSetTickFreq(hz);
}
void MainMenuBar::OnLogging(wxCommandEvent&) { m_owner->DoSimLogging(); }



void MainMenuBar::AddDocToWindowList(const wxString& title)
{
    /* 相同标题不再重复添加 */
    for (size_t i = 0; i < m_windowMenu->GetMenuItemCount(); ++i)
    {
        wxMenuItem* item = m_windowMenu->FindItemByPosition(i);
        if (item->GetItemLabelText() == title) return;
    }
    int id = wxID_HIGHEST + 400 + (int)m_windowMenu->GetMenuItemCount();
    m_windowMenu->Append(id, title, wxT("Switch to this document"));
    Bind(wxEVT_MENU, &MainMenuBar::OnWindowItem, this, id);
}
void MainMenuBar::SetCurrentDocInWindowList(const wxString& title)
{
    m_curDocTitle = title;
    for (size_t i = 0; i < m_windowMenu->GetMenuItemCount(); ++i)
    {
        wxMenuItem* item = m_windowMenu->FindItemByPosition(i);
        if (item->GetId() < wxID_HIGHEST + 400) continue; // 非文档项
        wxString label = item->GetItemLabelText();
        /* 去掉旧圆点 */
        if (label.StartsWith(wxT("• "))) label = label.Mid(2);
        /* 给当前项加圆点 */
        if (label == title) label = wxT("• ") + label;
        item->SetItemLabel(label);
    }
}
void MainMenuBar::OnMinimize(wxCommandEvent&) { m_owner->Iconize(true); }
void MainMenuBar::OnMaximize(wxCommandEvent&) { m_owner->Maximize(); }
void MainMenuBar::OnCloseWnd(wxCommandEvent&) { m_owner->Close(); }
void MainMenuBar::OnCombinationalAnalysis(wxCommandEvent&)
{
    m_owner->DoWindowCombinationalAnalysis();
}
void MainMenuBar::OnWindowPreferences(wxCommandEvent&)
{
    m_owner->DoWindowPreferences();
}
void MainMenuBar::OnWindowItem(wxCommandEvent& evt)
{
    wxString title = m_windowMenu->FindItem(evt.GetId())->GetItemLabelText();
    if (title.StartsWith(wxT("• "))) title = title.Mid(2); // 去掉圆点
    m_owner->DoWindowSwitchToDoc(title);
}

void MainMenuBar::OnTutorial(wxCommandEvent&)
{
    m_owner->DoHelpTutorial();
}
void MainMenuBar::OnUserGuide(wxCommandEvent&)
{
    m_owner->DoHelpUserGuide();
}
void MainMenuBar::OnLibraryRef(wxCommandEvent&)
{
    m_owner->DoHelpLibraryRef();
}
void MainMenuBar::OnAbout(wxCommandEvent&)
{
    m_owner->DoHelpAbout();
}

void MainMenuBar::SetSimulationBusy(bool busy)
{
    // File=0, Edit=1, Project=2, Window=3, Simulate=4, Help=5
    EnableTop(0, !busy);  // File
    EnableTop(1, !busy);  // Edit
    EnableTop(2, !busy);  // Project
    // Simulate menu stays enabled while busy; only Cancel Simulation keeps working.
    const int simulateIndex = FindMenu(wxT("&Simulate"));
    if (simulateIndex < 0) return;
    wxMenu* simulateMenu = GetMenu(simulateIndex);
    for (size_t i = 0; i < simulateMenu->GetMenuItemCount(); ++i) {
        wxMenuItem* item = simulateMenu->FindItemByPosition(i);
        if (item == nullptr) continue;
        if (item->IsSeparator()) continue;
        item->Enable(!busy || item->GetId() == wxID_HIGHEST + 223);
    }
}

// 仿真相关事件处理
void MainMenuBar::OnSimCompile(wxCommandEvent&) 
{ 
    SIGFLOW_LOG("=== OnSimCompile called ===\n");
    if (m_owner) {
        SIGFLOW_LOG("m_owner is valid, calling DoSimCompile...\n");
        m_owner->DoSimCompile(); 
    } else {
        SIGFLOW_LOG("ERROR: m_owner is NULL!\n");
        wxMessageBox(wxString("m_owner is NULL!"), wxString("Error"), wxOK | wxICON_ERROR);
    }
}

void MainMenuBar::OnSimRun(wxCommandEvent&) 
{ 
    m_owner->DoSimRun(); 
}

void MainMenuBar::OnSimCancel(wxCommandEvent&)
{
    if (m_owner) m_owner->DoSimCancel();
}

void MainMenuBar::OnSimClean(wxCommandEvent&) 
{ 
    m_owner->DoSimClean(); 
}
