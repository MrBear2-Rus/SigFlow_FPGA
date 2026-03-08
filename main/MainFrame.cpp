#include <wx/msgdlg.h>
#include <wx/filename.h> 
#include <wx/sstream.h>
#include <wx/aui/aui.h>
#include <wx/progdlg.h>
#include <wx/stc/stc.h>
#include <wx/stdpaths.h>
#include <wx/aui/tabart.h>
#include <wx/simplebook.h>
#include <wx/splitter.h>

#include "MainFrame.h"
#include "MainMenuBar.h"
#include "ToolboxPanel.h"  
#include "CanvasModel.h"
#include "my_log.h"
#include "CanvasNoteBook.h"
#include "VerilogStructuring.h"

extern std::vector<SecondElement> g_elements;

wxDEFINE_EVENT(EVT_SFTREE_NODE_ACTIVATED, wxCommandEvent);
wxDEFINE_EVENT(EVT_SIGFLOWNODE_ADD, wxCommandEvent);
wxDEFINE_EVENT(EVT_SIGFLOWNODE_DEL, wxCommandEvent);
wxDEFINE_EVENT(EVT_SIGFLOWNODE_CHANGED, wxCommandEvent);

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
EVT_MENU(wxID_ABOUT, MainFrame::OnAbout)
EVT_MENU(wxID_EXIT, MainFrame::OnQuit)
EVT_MENU(wxID_HIGHEST + 900, MainFrame::OnToolboxElement)
EVT_MENU(wxID_HIGHEST + 901, MainFrame::OnToolSelected)
wxEND_EVENT_TABLE()

enum {
    ID_SIDEBAR_START = wxID_HIGHEST + 1000, // 从一个安全的数字开始


    ID_PROJ,
    ID_FLOW,
    ID_TBOX,
    ID_OTHER_TOOL,

    ID_TB_NEW,
    ID_TB_OPEN,
    ID_TB_SAVE,
    ID_TB_RECLAIM,
    ID_TB_START,
    ID_TB_STOP,
};

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "SigFlow"),
    snap_version(0),               // 初始化
    m_isModified(false),
    m_verilogEditor(nullptr),       // 先置空
    m_analysisCenter(nullptr),
    m_projectTreePanel(nullptr),
    sigTree(nullptr),
    m_toolbox(nullptr),
    m_sigFlowTreePanel(nullptr),
    m_sfnPropertyPanel(nullptr),
    m_terminalCtrl(nullptr),
    m_pluginMgr(nullptr)
{
    // 图标
    wxInitAllImageHandlers();
    wxBitmapBundle svgIcon = wxBitmapBundle::FromSVGFile("res\\svg_icons\\icon.svg", wxSize(24, 24));
    wxIcon icon = svgIcon.GetIconFor(this);
    SetIcon(icon);

    wxSize tbIconSize = FromDIP(wxSize(12, 12));
    // 标题
    SetTitle("SigFlow [no project]");

    // 元件模型库
    wxString jsonPath = wxFileName(wxGetCwd(), "canvas_elements.json").GetFullPath();
    MyLog("MainFrame: JSON full path = [%s]\n", jsonPath.ToUTF8().data());
    g_elements = LoadSecondElements(jsonPath);


    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
    

    // 构造SigTree
    sigTree = new SigFlowTree(this);
    this->Bind(EVT_SIGFLOWNODE_ADD, &MainFrame::OnSFNodeAdded, this);
    this->Bind(EVT_SIGFLOWNODE_DEL, &MainFrame::OnSFNodeDeleted, this);
    this->Bind(EVT_SIGFLOWNODE_CHANGED, &MainFrame::OnSFNodeChanged, this);

    /* 面板加载 */
    m_auiMgr.SetManagedWindow(this);

    /* 菜单栏*/
    SetMenuBar(new MainMenuBar(this));
    CreateStatusBar(1);
    int widths[] = { -4, -2, FromDIP(100), FromDIP(100) };
    int style[] = { wxSB_NORMAL, wxSB_NORMAL, wxSB_NORMAL, wxSB_NORMAL };
    GetStatusBar()->SetFieldsCount(4, widths);
    GetStatusBar()->SetStatusStyles(4, style);

    m_structuring = new Structuring();
    m_textTimer = new wxTimer();
    m_textTimer->Bind(wxEVT_TIMER, &MainFrame::OnAnalysisTimer, this);

    // 画布
    m_canvas = new CanvasNoteBook(this, sigTree, wxID_ANY, FromDIP(1123), FromDIP(794));

    // 元件库
    m_toolbox = new ToolboxPanel(this);
    // 构造树面板
    m_sigFlowTreePanel = new SigFlowTreePanel(this, sigTree);

    // SigTreeNode属性栏
    m_sfnPropertyPanel = new SFNPropertyPanel(this, sigTree);
    this->Bind(EVT_SFTREE_NODE_ACTIVATED, &MainFrame::OnSFNodeActivated, this);

    // 文本编辑面板
    m_verilogEditor = new SigTextEditor(this);
    m_verilogEditor->SetModEventMask(
        wxSTC_MOD_INSERTTEXT |
        wxSTC_MOD_DELETETEXT
    );
    m_verilogEditor->Bind(
        wxEVT_STC_MODIFIED,
        &MainFrame::OnStcModified,
        this
    );

    // 异步IDE分析
    m_analysisCenter = new AsyncAnalysisCenter(this);
    this->Bind(EVT_ANALYSIS_COMPLETE, &MainFrame::OnAnalysisComplete, this);

    // 项目树面板
    m_projectTreePanel = new ProjectTreePanel(this);
    // 监听项目加载事件，及时把路径传给插件
    this->Bind(EVT_PROJECT_LOADED, [this](wxCommandEvent& evt) {
        ISigPlugin* p = m_pluginMgr->GetPlugin("DeepSeek_Assistant");
        if (p) {
            p->SetProjectRoot(std::string(evt.GetString().ToUTF8().data()));
        }
    });
    this->Bind(wxEVT_MENU, &MainFrame::OnOpenFileFromTree, this, ID_OPEN_FILE_FROM_TREE);

    // 侧边工具栏
    wxAuiToolBar* sideBar = new wxAuiToolBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_TB_VERTICAL | wxAUI_TB_NO_TOOLTIPS);
    sideBar->SetBackgroundColour(wxColour(225, 230, 235));
 
    // 终端
    m_terminalCtrl = new TerminalCtrl(this);

    auto GetIcon = [&](const wxString& path) {
        wxBitmapBundle bundle = wxBitmapBundle::FromSVGFile(path, wxSize(24, 24));
        return bundle.GetBitmap(FromDIP((tbIconSize, tbIconSize)));
        };

    sideBar->AddTool(ID_PROJ, wxEmptyString, GetIcon("res\\icons\\project.svg"), "Project Manager", wxITEM_CHECK);
    sideBar->AddTool(ID_FLOW, wxEmptyString, GetIcon("res\\svg_icons\\icon.svg"), "SigFlow Tree", wxITEM_CHECK);
    sideBar->AddTool(ID_TBOX, wxEmptyString, GetIcon("res\\icons\\lib.svg"), "Component Library", wxITEM_CHECK);
    sideBar->ToggleTool(ID_PROJ, 1);
    sideBar->SetArtProvider(new MyCustomToolBarArt());


    // 插件加载
    m_pluginMgr = new PluginManager();

    // 1. 获取当前 main.exe 的绝对路径
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();
    // 2. 提取 exe 所在的目录
    wxString exeDir = wxFileName(exePath).GetPath();
    // 3. 拼接出 plugins 文件夹的绝对路径
    wxString pluginDir = exeDir + wxFileName::GetPathSeparator() + "plugins";

    // 打印出来确认一下（可选）
    m_terminalCtrl->PrintOutput("Plugin Directory: " + pluginDir);

    // 4. 加载插件
    m_pluginMgr->LoadPlugins(pluginDir.ToStdString());

    // 获取所有插件列表，准备在菜单或工具栏显示
    const auto& plugins = m_pluginMgr->GetAllPlugins();
    for (auto* p : plugins) {
        m_terminalCtrl->PrintOutput(p->GetName() + " Loaded\n");
    }

    ISigPlugin* pDeepSeek = m_pluginMgr->GetPlugin("DeepSeek_Assistant");
    
    // 如果插件存在，优先把当前打开的项目路径传递给插件（若 ProjectTreePanel 已加载项目）
    if (pDeepSeek) {
        wxString projRoot = m_projectTreePanel->GetProjectRoot();
        if (!projRoot.IsEmpty()) {
            pDeepSeek->SetProjectRoot(std::string(projRoot.ToUTF8().data()));
        }
    }





    wxSimplebook* leftSideNotebook = new wxSimplebook(this, wxID_ANY);

    m_projectTreePanel->Reparent(leftSideNotebook);
    m_sigFlowTreePanel->Reparent(leftSideNotebook);
    m_toolbox->Reparent(leftSideNotebook);
    leftSideNotebook->AddPage(m_projectTreePanel, "Project Manager");
    leftSideNotebook->AddPage(m_sigFlowTreePanel, "SigFlow Tree");
    leftSideNotebook->AddPage(m_toolbox, "Component Library");

    Bind(wxEVT_TOOL, [=](wxCommandEvent& e) {
        int clickedId = e.GetId();
        wxAuiPaneInfo& pane = m_auiMgr.GetPane(leftSideNotebook);
        // 1. 实现互斥选中（就像 Notebook 切换标签一样）
        sideBar->ToggleTool(ID_PROJ, clickedId == ID_PROJ);
        sideBar->ToggleTool(ID_FLOW, clickedId == ID_FLOW);
        sideBar->ToggleTool(ID_TBOX, clickedId == ID_TBOX);

        // 2. 刷新工具栏视觉状态
        sideBar->Refresh();

        // 3. 切换右侧面板（假设你用了 wxSimplebook）
        if (clickedId == ID_PROJ) {
            leftSideNotebook->SetSelection(0);
            pane.Caption("Project Manager");
        }
        else if (clickedId == ID_FLOW) {
            leftSideNotebook->SetSelection(1);
            pane.Caption("SigFlow Tree");
        }
        else if (clickedId == ID_TBOX) {
            leftSideNotebook->SetSelection(2);
            pane.Caption("Component Library");
        }
        m_auiMgr.Update();
        }, ID_PROJ, ID_TBOX); // 
    sideBar->Realize();

    // 顶端工具栏
    wxAuiToolBar* topBar = new wxAuiToolBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_TB_HORIZONTAL | wxAUI_TB_PLAIN_BACKGROUND);

    topBar->SetToolBitmapSize(tbIconSize);

    // --- 左侧：项目与控制组 ---
    topBar->AddTool(ID_TB_NEW, "New", GetIcon("res\\svg_icons\\new_project.svg"), "New Project");
    topBar->AddTool(ID_TB_OPEN, "Open", GetIcon("res\\svg_icons\\open_project.svg"), "Open Project");
    topBar->AddTool(ID_TB_SAVE, "Save", GetIcon("res\\svg_icons\\save_project.svg"), "Save All");
    topBar->AddSeparator();

    topBar->AddTool(ID_TB_RECLAIM, "Reclaim", GetIcon("res\\svg_icons\\reclaim.svg"), "Reclaim Memory");
    topBar->AddSeparator();

    topBar->AddTool(ID_TB_START, "Start", GetIcon("res\\svg_icons\\start.svg"), "Start Simulation");
    topBar->AddTool(ID_TB_STOP, "Stop", GetIcon("res\\svg_icons\\end.svg"), "Stop Simulation");


    topBar->Realize();
    // 绑定顶端工具栏按钮事件：将工具栏按钮的点击映射到 MainFrame 的业务函数
    Bind(wxEVT_TOOL, [this](wxCommandEvent& e) {
        switch (e.GetId()) {
        case ID_TB_NEW:
            DoFileNew();
            break;
        case ID_TB_OPEN:
            DoFileOpen();
            break;
        case ID_TB_SAVE:
            DoFileSave();
            break;
        case ID_TB_RECLAIM:
            DoSimClean(); // 复用清理接口作为回收占位行为
            break;
        case ID_TB_START:
            DoSimRun();
            break;
        case ID_TB_STOP:
            DoSimReset();
            break;
        default:
            break;
        }
    }, ID_TB_NEW, ID_TB_STOP);
    
    wxAuiNotebook* rightNotebook = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_TAB_EXTERNAL_MOVE | wxAUI_NB_TAB_SPLIT);

    m_sfnPropertyPanel->Reparent(rightNotebook);
    rightNotebook->AddPage(m_sfnPropertyPanel, "Property");
    if (pDeepSeek) {
        wxPanel* aiPanel = pDeepSeek->CreatePanel(rightNotebook);
        rightNotebook->AddPage(aiPanel, "DeepSeek Assistant");
    }

    wxSplitterWindow* mainSplitter = new wxSplitterWindow(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxSP_LIVE_UPDATE | wxSP_3DSASH | wxBORDER_NONE);
    m_verilogEditor->Reparent(mainSplitter);
    m_canvas->Reparent(mainSplitter);
    int initialCanvasHeight = FromDIP(860);
    mainSplitter->SplitHorizontally(m_canvas, m_verilogEditor, initialCanvasHeight);; // 0 表示平分
    mainSplitter->SetMinimumPaneSize(FromDIP(50)); // 防止某个窗口被缩成 0 找不到了



    wxAuiNotebook* bottomNotebook = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_TAB_EXTERNAL_MOVE | wxAUI_NB_TAB_SPLIT);

    
    bottomNotebook->AddPage(m_terminalCtrl, "Terminal");



    // 1. 先最大化窗口，确保尺寸基准正确
    this->Maximize(true);
    this->Layout(); // 让基础布局先跑一遍

    // 2. 获取当前真正的物理可用区域
    wxSize dcSize = this->GetClientSize();

    // --- 定义比例 ---
    int leftW = dcSize.x * 0.1;  // 15%
    int rightW = dcSize.x * 0.1;  // 40%
    int bottomH = dcSize.y * 0.1; // 30%

    // 3. 配置 Pane
    m_auiMgr.AddPane(topBar, wxAuiPaneInfo()
        .Name("topBar")
        .Top()
        .Layer(8)
        .MinSize(10000, 24)
        .CaptionVisible(false)
        .CloseButton(false)
        .Gripper(false)
        .Fixed()
        .PaneBorder(false) // 移除 AUI 管理的边框
        .Movable(false)    // 固定位置，防止用户拖动导致 UI 错位
    );

    m_auiMgr.AddPane(sideBar, wxAuiPaneInfo()
        .Name("sideNav")
        .Left()
        .Layer(10)               // 高 Layer 值保证它在最左侧“长条”显示
        .CaptionVisible(false)
        .CloseButton(false)
        .PinButton(false)
        .Gripper(false)
        .PaneBorder(false)       // 尝试使用 PaneBorder
        .Fixed()
        .MinSize(FromDIP(45), -1)
        .BestSize(FromDIP(45), -1));

    m_auiMgr.AddPane(leftSideNotebook, wxAuiPaneInfo()
        .Name("left_sidebar").Caption("Project Manager").Left().Layer(9)
        .BestSize(leftW, -1)
        .MinSize(FromDIP(100), -1)
        .FloatingSize(leftW, 600) // 诱导 AUI 记录这个宽度
        .PaneBorder(false)
        .Floatable(false)
        .CaptionVisible(true).CloseButton(false).MaximizeButton(false));

    m_auiMgr.AddPane(rightNotebook, wxAuiPaneInfo()
        .Name("right_sidebar").Caption("Side Panel").Right().Layer(8)
        .BestSize(rightW, -1)
        .MinSize(FromDIP(100), -1)
        .FloatingSize(rightW, 600)
        .MaximizeButton(true)
        .CloseButton(false));

    m_auiMgr.AddPane(bottomNotebook, wxAuiPaneInfo()
        .Name("bottom_tabs").Caption("Console").Bottom().Layer(8)
        .BestSize(-1, bottomH)
        .MinSize(-1, FromDIP(80))
        .FloatingSize(800, bottomH)
        .MaximizeButton(true)
        .CloseButton(false));

    m_auiMgr.AddPane(mainSplitter, wxAuiPaneInfo()
        .Name("center_area")
        .CenterPane()       // 设为中心区域
        .PaneBorder(false));

    // --- 4. 暴力修正方案：手动干预 Sash 位置 ---
    m_auiMgr.Update();

    // 如果 Update 后还是没变，这是因为 AUI 的内部状态已经锁定。
    // 我们尝试手动修改 PaneInfo 里的 dock_size 并再次强制 Update。
    this->CallAfter([this]() {
        if (m_canvas) {
            m_canvas->AdjustScaleToFit(); // 你自定义的强制适配函数
        }
        });
    m_auiMgr.GetPane("left_sidebar").BestSize(leftW, -1);
    m_auiMgr.GetPane("right_sidebar").BestSize(rightW, -1);
    m_auiMgr.GetPane("bottom_tabs").BestSize(-1, bottomH);

    m_auiMgr.Update();

   


    m_auiMgr.SetFlags(m_auiMgr.GetFlags() |
        wxAUI_MGR_ALLOW_ACTIVE_PANE |
        wxAUI_MGR_ALLOW_FLOATING |    // 允许浮动
        wxAUI_MGR_LIVE_RESIZE);       // 实时调整大小，体验更好
    ModernDockArt* mda = new ModernDockArt();
    m_auiMgr.SetArtProvider(mda);
    mda->UpdateMetrics(this);
    this->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& evt) {
        ModernDockArt* art = static_cast<ModernDockArt*>(m_auiMgr.GetArtProvider());
        art->UpdateMetrics(this);
        m_auiMgr.Update();
        evt.Skip();
        });

    m_auiMgr.Update();


}

MainFrame::~MainFrame()
{
    m_auiMgr.UnInit(); 

    m_verilogEditor = nullptr;
    m_auiMgr.UnInit();
}

void MainFrame::OnToolboxElement(wxCommandEvent& evt)
{

    wxString name = evt.GetString();
    //m_canvas->AddElement(clone);     
    m_canvas->SetCurrentComponent(name);  
}

bool MirrorDirectory(const wxString& source, const wxString& dest) {
    if (!wxDir::Exists(dest)) {
        if (!wxFileName::Mkdir(dest, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
            return false;
        }
    }

    wxDir dir(source);
    if (!dir.IsOpened()) return false;

    wxString filename;
    // 1. 复制所有文件
    bool cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES);
    while (cont) {
        wxCopyFile(source + wxFileName::GetPathSeparator() + filename,
            dest + wxFileName::GetPathSeparator() + filename, true);
        cont = dir.GetNext(&filename);
    }

    // 2. 递归处理子目录 (跳过 .git 和 .sigflow 自身，防止无限递归)
    cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_DIRS);
    while (cont) {
        if (filename != ".sigflow" && filename != ".git" && filename != ".cache") {
            MirrorDirectory(source + wxFileName::GetPathSeparator() + filename,
                dest + wxFileName::GetPathSeparator() + filename);
        }
        cont = dir.GetNext(&filename);
    }
    return true;
}



void DumpTree(TSNode node, const wxString& src, int indent) {
    wxString line;

    line << "|-";
    for (int i = 0; i < indent; ++i)
        line << "-";

    line << "{" << indent << "L}" << ts_node_type(node);

    if (ts_node_is_named(node))
        line << " [named]";

    if (ts_node_is_missing(node)) line << " [missing]";
    if (ts_node_has_error(node)) line << " [has error]";
    if (ts_node_is_error(node)) line << " [error]";


    line << "  (" << ts_node_start_byte(node)
        << "," << ts_node_end_byte(node) << ")";

    line << "  text=\""
        << src.substr(ts_node_start_byte(node),
            ts_node_end_byte(node) - ts_node_start_byte(node))
        << "\n";

    wxLogDebug("%s", line);
    //OutputDebugStringA(line);

    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        DumpTree(ts_node_child(node, i), src, indent + 1);
}


#include "VerilogStructuring.h"
#include <tree_sitter/api.h>
extern "C" TSLanguage* tree_sitter_verilog();

//ֻ�Ǵ�һ���´��ڣ���������д������κθı�
void MainFrame::DoFileOpenProject() {
    wxDirDialog dlg(this, "Open Project Directory", "",
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);

    if (dlg.ShowModal() == wxID_OK) {
        wxString path = dlg.GetPath();

        wxProgressDialog progress("Loading Project", "Initializing...",
            100, this,
            wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH);


        m_projectTreePanel->LoadProject(path);
        m_currentProjectPath = path;

        sigTree->LoadProject(path.ToStdString());


        wxString fullPath = path + wxFileName::GetPathSeparator() + "sigflow.project";
        wxFile file(fullPath);
        wxString content;
        file.ReadAll(&content);
        std::string utf8Content = content.ToUTF8().data();

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errs;


        if (reader->parse(utf8Content.c_str(), utf8Content.c_str() + utf8Content.size(), &root, &errs)) {
            if (root["paths"].isMember("source_files")) {
                auto& sourceFiles = root["paths"]["source_files"];
                int totalFiles = sourceFiles.size();
                progress.SetRange(totalFiles + 2); // 文件数 + 解析JSON(1) + 镜像(1)

                int currentStep = 0;

                for (const auto& file : root["paths"]["source_files"]) {

                    currentStep++;
                    wxString fileName = file.asString();

                    // --- 2. 更新进度条文字 ---
                    progress.Update(currentStep, "Parsing: " + fileName);

                    // 1. 获取文件的绝对路径
                    wxFileName fn1(file.asString());
                    fn1.MakeAbsolute(path);
                    wxString wxAbsPath = fn1.GetFullPath();
                    std::string absPath1 = wxAbsPath.ToStdString();

                    // 2. 读取该文件的实际内容 (关键步骤)
                    wxFile vFile(wxAbsPath, wxFile::read);
                    if (!vFile.IsOpened()) continue; // 如果文件打不开，跳过

                    wxString fileContent;
                    vFile.ReadAll(&fileContent);
                    vFile.Close();

                    std::string stdCode = fileContent.ToStdString();

                    // 3. 为当前文件构造 Tree-sitter 资源
                    TSParser* parser = ts_parser_new();
                    ts_parser_set_language(parser, tree_sitter_verilog());

                    // 解析当前读取到的 stdCode，而不是全局的 sp.stable_code
                    TSTree* new_tree = ts_parser_parse_string(parser, nullptr, stdCode.c_str(), stdCode.length());
                    
                    if (new_tree) {
                        TSNode rootNode = ts_tree_root_node(new_tree);
                        // 调试打印
                        // DumpTree(rootNode, stdCode, 0); 

                        TSTreeCursor cursor = ts_tree_cursor_new(rootNode);

                        // 4. 更新数据模型
                        // 注意：这里传入的是当前文件的路径 absPath1 和当前文件的代码 stdCode
                        DumpTree(rootNode, stdCode, 0);
                        sigTree->UpdateTreeFromTS(&cursor, sigTree->root, absPath1, stdCode);

                        // 清理 TS 局部资源
                        ts_tree_cursor_delete(&cursor);
                        ts_tree_delete(new_tree);
                    }
                    ts_parser_delete(parser);
                }

                // 5. 所有文件解析完成后，一次性刷新 UI
                // 不要在 for 循环内部 Refresh，否则文件多了会非常卡
                
                sigTree->LinkInstsWithDefs();
                sigTree->PrintTree();
            }
        }
        for (auto defId : sigTree->GetDefinitions()) {
            m_toolbox->AddDefinition(defId);
        }

        progress.Update(progress.GetRange() - 1, "Creating Workspace Mirror...");

        wxString workspacePath = m_currentProjectPath + wxFileName::GetPathSeparator() +
            ".sigflow" + wxFileName::GetPathSeparator() + "workspace";

        //wxLogStatus("Mirroring project to workspace...");

        if (MirrorDirectory(m_currentProjectPath, workspacePath)) {
            //wxLogMessage("Project mirrored to: %s", workspacePath);

            // 2. 更新内部状态
            m_currentProjectPath = m_currentProjectPath;
            m_workspacePath = workspacePath; // 建议在 MainFrame 增加此成员变量
            m_projectName = wxFileName(path).GetFullName();

            // 3. 让左侧树加载原始路径（用户感知），但编译器使用 workspacePath
            m_projectTreePanel->LoadProject(m_currentProjectPath);
            RefreshTitle();
        }

        progress.Update(progress.GetRange(), "Load Complete!");
        wxCommandEvent evt;
        OnSFTreeChanged(evt);
    }
}


void MainFrame::DoFileNew() {
    // Create a new project directory with basic structure and open it in the project tree
    // 1) Ask for parent folder
    wxDirDialog dirDlg(this, "Select parent folder for new project", "",
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dirDlg.ShowModal() != wxID_OK) return;
    wxString parent = dirDlg.GetPath();

    // 2) Ask for project name
    wxTextEntryDialog nameDlg(this, "Enter project name:", "New Project", "NewProject");
    if (nameDlg.ShowModal() != wxID_OK) return;
    wxString projName = nameDlg.GetValue();
    if (projName.IsEmpty()) {
        wxMessageBox("Project name cannot be empty", "Error", wxOK | wxICON_ERROR, this);
        return;
    }

    // 3) Build project path and check
    wxFileName fn(parent, projName);
    wxString projPath = fn.GetFullPath();
    if (wxDirExists(projPath)) {
        wxDir dir(projPath);
        wxString anyName;
        bool notEmpty = dir.IsOpened() && dir.GetFirst(&anyName);
        if (notEmpty) {
            int res = wxMessageBox("The folder already exists and is not empty. Overwrite?", "Confirm",
                wxYES_NO | wxICON_QUESTION, this);
            if (res != wxYES) return;
        }
    }
    else if (wxFileExists(projPath)) {
        int res = wxMessageBox("A file with the same name exists. Overwrite?", "Confirm",
            wxYES_NO | wxICON_QUESTION, this);
        if (res != wxYES) return;
        wxRemoveFile(projPath);
    }

    // 4) Create directory structure
    if (!wxFileName::Mkdir(projPath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        wxMessageBox("Failed to create project folder", "Error", wxOK | wxICON_ERROR, this);
        return;
    }
    wxString srcDir = projPath + wxFileName::GetPathSeparator() + "src";
    wxString libDir = projPath + wxFileName::GetPathSeparator() + "lib";
    wxString sigflowDir = projPath + wxFileName::GetPathSeparator() + ".sigflow";
    wxString workspaceDir = sigflowDir + wxFileName::GetPathSeparator() + "workspace";
    wxString simDir = sigflowDir + wxFileName::GetPathSeparator() + "sim";
    wxFileName::Mkdir(srcDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(libDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(workspaceDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(simDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    // 5) Create a minimal sigflow.project JSON
    wxString projectJson = "{\n  \"build\": { \"top_module\": [] },\n  \"paths\": { \"source_files\": [], \"library_files\": [] }\n}\n";
    wxString projFile = projPath + wxFileName::GetPathSeparator() + "sigflow.project";
    wxFile pfile;
    if (pfile.Open(projFile, wxFile::write)) {
        pfile.Write(projectJson);
        pfile.Close();
    }

    // 6) Create a sample top Verilog file to get started
    wxString sampleTop = srcDir + wxFileName::GetPathSeparator() + "top.v";
    wxFile sampleFile;
    if (sampleFile.Open(sampleTop, wxFile::write)) {
        wxString sampleCode = "module top();\n    // TODO: add signals and logic\nendmodule\n";
        sampleFile.Write(sampleCode);
        sampleFile.Close();
    }

    // 7) Create README
    wxString readmePath = projPath + wxFileName::GetPathSeparator() + "README.md";
    wxFile rfile;
    if (rfile.Open(readmePath, wxFile::write)) {
        rfile.Write(wxString::Format("# %s\n\nThis is a new SigFlow project.", projName));
        rfile.Close();
    }

    // 8) Load the project into UI
    m_projectTreePanel->LoadProject(projPath);
    m_currentProjectPath = projPath;
    m_workspacePath = workspaceDir;
    m_projectName = projName;
    RefreshTitle();

    // 9) Open sample file in editor so user can start coding
    DoFileOpen(sampleTop);
}

//�����ļ���ʵ�֣����������ĸ�����
void MainFrame::DoFileSave() {
    //// 1. �����ǰ�ĵ�û��·����δ��������������"����Ϊ"
    //if (m_currentFilePath.IsEmpty()) {
    //    // ����DoFileSaveAs()�����״α��棨��ʵ�ָ÷�����
    //    DoFileSaveAs();
    //    return;
    //}

    //// 2. ���Խ���ǰ�ĵ�����д���ļ�
    //bool saveSuccess = SaveToFile(m_currentFilePath);

    //// 3. ���ݱ���������״̬
    //if (saveSuccess) {
    //    m_isModified = false;  // ����ɹ������Ϊδ�޸�
    //    //UpdateTitle();         // ���´��ڱ��⣨�Ƴ�"*"���޸ı�ǣ�
    //    SetStatusText(wxString::Format("�ѱ���: %s", m_currentFilePath));
    //}
    //else {
    //    wxMessageBox(
    //        wxString::Format("����ʧ��: %s", m_currentFilePath),
    //        "����",
    //        wxOK | wxICON_ERROR,
    //        this
    //    );
    //}
    m_verilogEditor->SaveFile();
}

// ��������������ǰ�ĵ�����д��ָ��·�����޸�ΪXML��ʽ��
bool MainFrame::SaveToFile(const wxString& filePath) {
    // 1. ����XML���ݣ���ʹΪ��Ҳ����д�룩
    wxString xmlContent = GenerateFileContent();

    // 2. ���Դ��ļ�д�루���Դ�ʧ�ܵ������
    wxFile outputFile;
    outputFile.Open(filePath, wxFile::write);  // ���жϴ򿪽����ֱ�ӳ���д��

    // 3. д�����ݣ�����֤д������
    outputFile.Write(xmlContent);
    outputFile.Close();

    // 4. ǿ�Ʒ���true��Ĭ�ϱ���ɹ�
    return true;
}

wxString MainFrame::GenerateFileContent()
{
    /*
    // 1. ����XML�ĵ�
    wxXmlDocument doc;

    // 2. ���ڵ� <project>
    wxXmlNode* root = new wxXmlNode(wxXML_ELEMENT_NODE, "project");
    root->AddAttribute("source", "2.7.1");
    root->AddAttribute("version", "1.0");
    doc.SetRoot(root);

    // 3. ע��
    root->AddChild(new wxXmlNode(wxXML_COMMENT_NODE,
        "This file is intended to be loaded by Logisim "
        "(http://www.cburch.com/logisim/)"));

    // 4. ����Ϣ
    AddLibraryNode(root, "0", "#Wiring");
    AddLibraryNode(root, "1", "#Gates");

    // 5. ��·�ڵ�
    wxXmlNode* circuit = new wxXmlNode(wxXML_ELEMENT_NODE, "circuit");
    circuit->AddAttribute("name", "main");
    root->AddChild(circuit);

    // ����Ԫ����Ϣ���Ƴ���ת�Ƕ���ش��룩
    for (const auto& elem : m_canvas->GetElements()) {
        wxXmlNode* element = new wxXmlNode(wxXML_ELEMENT_NODE, "element");
        element->AddAttribute("name", elem.GetName());  // ����Ԫ������
        element->AddAttribute("x", wxString::Format("%d", elem.GetPos().x));  // ����X����
        element->AddAttribute("y", wxString::Format("%d", elem.GetPos().y));  // ����Y����
        // �Ƴ��������й���rotation�Ĵ���
        // element->AddAttribute("rotation", wxString::Format("%d", elem.GetRotation()));
        circuit->AddChild(element);
    }

    // 7. ����������Ϣ
    for (const auto& wire : m_canvas->GetWires()) {
        // ֱ�ӷ���Wire���pts��Ա������ȡ�㼯��
        const auto& pts = wire.pts;  // �ؼ��޸ģ�ʹ��wire.pts���wire.GetPoints()
        if (pts.size() < 2) continue;

        wxXmlNode* wireNode = new wxXmlNode(wxXML_ELEMENT_NODE, "wire");
        // ������㣨��һ���㣩���յ㣨���һ���㣩
        wireNode->AddAttribute("from", wxString::Format("(%d,%d)", pts[0].pos.x, pts[0].pos.y));
        wireNode->AddAttribute("to", wxString::Format("(%d,%d)", pts.back().pos.x, pts.back().pos.y));

        // �����м�㣨�����ڣ�
        if (pts.size() > 2) {
            wxString midPoints;
            for (size_t i = 1; i < pts.size() - 1; ++i) {
                midPoints += wxString::Format("(%d,%d);", pts[i].pos.x, pts[i].pos.y);
            }
            wireNode->AddAttribute("midpoints", midPoints);
        }
        circuit->AddChild(wireNode);
    }

    // 8. ���XML����
    wxStringOutputStream strStream;
    doc.Save(strStream, wxXML_DOCUMENT_TYPE_NODE);
    return strStream.GetString();*/
    return "";
}

void MainFrame::DoFileOpen(const wxString& path)
{
    /*
    wxString filePath = path;

    // 如果用户没有提供路径，显示文件选择对话框
    if (filePath.IsEmpty()) {
        wxFileDialog openDialog(
            this,
            wxT("打开文件"),
            wxT(""),
            wxT(""),
            wxT("电路文件 (*.circ)|*.circ|所有文件 (*.*)|*.*"),
            wxFD_OPEN | wxFD_FILE_MUST_EXIST
        );

        if (openDialog.ShowModal() != wxID_OK) {
            return;
        }
        filePath = openDialog.GetPath();
    }

    // 检查文件扩展名
    wxFileName fn(filePath);
    wxString ext = fn.GetExt().Lower();

    // 注：不再支持直接打开单个 .v 文件，必须通过项目方式打开

    // 尝试读取文件内容
    wxFile file;
    if (!file.Open(filePath, wxFile::read)) {
        wxMessageBox(wxT("无法打开文件: ") + filePath, wxT("错误"), wxOK | wxICON_ERROR);
        return;
    }

    // ��ȡXML����
    wxString xmlContent;
    file.ReadAll(&xmlContent);
    file.Close();

    // ����XML
    wxXmlDocument doc;
    wxStringInputStream stream(xmlContent);
    if (!doc.Load(stream)) {
        wxMessageBox("�ļ���ʽ����: " + filePath, "����", wxOK | wxICON_ERROR);
        return;
    }

    // ��յ�ǰ����
    //m_canvas->ClearAll();

    // �������ڵ�
    wxXmlNode* root = doc.GetRoot();
    if (!root || root->GetName() != "project") {
        wxMessageBox("��Ч�ĵ�·�ļ�", "����", wxOK | wxICON_ERROR);
        return;
    }

    // ���ҵ�·�ڵ�
    wxXmlNode* circuit = root->GetChildren();
    while (circuit) {
        if (circuit->GetName() == "circuit") {
            break;
        }
        circuit = circuit->GetNext();
    }

    if (!circuit) {
        wxMessageBox("�ļ���δ�ҵ���·��Ϣ", "����", wxOK | wxICON_ERROR);
        return;
    }

    // ����Ԫ��������
    wxXmlNode* child = circuit->GetChildren();
    while (child) {
        // ����Ԫ�����Ƴ���ת�Ƕ���ش��룩
        if (child->GetName() == "element") {
            wxString name = child->GetAttribute("name");
            int x = wxAtoi(child->GetAttribute("x", "0"));
            int y = wxAtoi(child->GetAttribute("y", "0"));
            // �Ƴ��������й���rotation�Ķ�ȡ
            // int rotation = wxAtoi(child->GetAttribute("rotation", "0"));

            //m_canvas->AddElement(name, wxPoint(x, y));
            // 同时移除设置旋转角度的逻辑（如果有的话）
        }

        // �������ߣ���DoFileOpen�����У�
        else if (child->GetName() == "wire") {
            wxString fromStr = child->GetAttribute("from");
            wxString toStr = child->GetAttribute("to");
            wxString midPointsStr = child->GetAttribute("midpoints", "");

            // ��������� (x,y)
            auto parsePoint = [](const wxString& str) -> wxPoint {
                int x = 0, y = 0;
                if (sscanf(str.ToUTF8().data(), "(%d,%d)", &x, &y) == 2) {
                    return wxPoint(x, y);
                }
                return wxPoint(0, 0);
                };

            // �ؽ�pts����
            std::vector<ControlPoint> pts;
            pts.push_back({ parsePoint(fromStr), CPType::Pin });  // ��㣨Pin���ͣ�

            // �����м��
            if (!midPointsStr.IsEmpty()) {
                wxArrayString midPoints = wxSplit(midPointsStr, ';');
                for (const auto& ptStr : midPoints) {
                    if (ptStr.IsEmpty()) continue;
                    pts.push_back({ parsePoint(ptStr), CPType::Bend });  // �м��Ϊ�۵�
                }
            }

            pts.push_back({ parsePoint(toStr), CPType::Free });  // �յ㣨Free���ͣ�

            // ����Wire�����ӵ�����
            Wire wire;
            wire.pts = pts;  // ֱ�Ӹ�ֵ��Wire��pts��Ա
            wire.GenerateCells();  // ��������㣨������ʾһ���ԣ�
            m_canvas->AddWire(wire);
        }

        child = child->GetNext();
    }

    // 更新状态
    m_currentFilePath = filePath;
    m_isModified = false;
    SetTitle(wxFileName(filePath).GetFullName());
    static_cast<MainMenuBar*>(GetMenuBar())->AddFileToHistory(filePath);
    SetStatusText("�Ѵ�: " + filePath);*/
}


void MainFrame::OnToolSelected(wxCommandEvent& evt) {
  wxString toolName = evt.GetString();
    std::map<wxString, wxVariant> currentProps;
   
}




















// 1. ����ΪBookShelf�淶��.node�ļ�
bool MainFrame::SaveAsNodeFile(const wxString& filePath)
{
    /*
    wxFile file;
    // ���Դ��ļ�����ʧ�ܷ���false
    if (!file.Exists(filePath)) {
        if (!file.Create(filePath))
            return false;
    }
    if (!file.Open(filePath, wxFile::write))
        return false;

    wxString content;
    const auto& elements = m_canvas->GetElements();
    int numTotalNodes = elements.size();  // �ܵ�Ԫ�������л���Ԫ����
    int numTerminals = 0;                 // �ն˵�Ԫ������ͳ�ƴ�I/O���ŵ�Ԫ����

    // ��һ����ͳ���ն˵�Ԫ����������/������ŵ�Ԫ����Ϊ�նˣ�
    for (const auto& elem : elements)
    {
        if (!elem.GetInputPins().empty() || !elem.GetOutputPins().empty())
            numTerminals++;
    }

    // �ڶ�����д��.node�ļ�ͷ����NumNodes + NumTerminals��
    content += wxString::Format("NumNodes %d\n", numTotalNodes);
    content += wxString::Format("NumTerminals %d\n", numTerminals);

    // ��������д��ÿ����Ԫ����ϸ��Ϣ����Ԫ�� + ���� + �߶� + �ն˱�ǣ�
    for (const auto& elem : elements)
    {
        // ��ȡ��Ԫ������Ϣ�����ơ�λ�ã����ڼ�����ߣ�
        wxString nodeName = elem.GetName();
        wxRect bounds = elem.GetBounds();  // ͨ��Ԫ���߽�������
        int width = bounds.GetWidth();     // ��Ԫ���ȣ����أ��ɰ����ջ���Ϊ��m���˴��������ص�λ��
        int height = bounds.GetHeight();   // ��Ԫ�߶ȣ�����site�߶ȣ��ĵ�Ĭ��12���˴���ʵ�ʱ߽�ȡ����

        // �ж��Ƿ�Ϊ�ն˵�Ԫ����I/O���ţ�
        bool isTerminal = (!elem.GetInputPins().empty() || !elem.GetOutputPins().empty());

        // ƴ�ӵ�Ԫ�У��ն˵�Ԫ���"terminal"��ǣ���ͨ��Ԫ�����������Ϣ
        if (isTerminal)
        {
            content += wxString::Format("%s %d %d terminal\n",
                nodeName, width, height);
        }
        else
        {
            content += wxString::Format("%s %d %d\n",
                nodeName, width, height);
        }
    }

    // д���ļ����ر�
    file.Write(content);
    file.Close();*/
    return true;
}

bool MainFrame::SaveAsNetFile(const wxString& filePath)
{
    /*
    // ���Դ��������ļ�
    wxFile file;
    if (!file.Exists(filePath))
    {
        if (!file.Create(filePath))
        {
            wxMessageBox("�޷�����.net�ļ���", "����", wxOK | wxICON_ERROR);
            return false;
        }
    }
    if (!file.Open(filePath, wxFile::write))
    {
        wxMessageBox("�޷���.net�ļ�����д�룡", "����", wxOK | wxICON_ERROR);
        return false;
    }

    // �ռ������еĵ��ߺ�Ԫ������
    const auto& wires = m_canvas->GetWires();       // ���軭����GetWires()�������ص����б�
    const auto& elements = m_canvas->GetElements(); // ���軭����GetElements()��������Ԫ���б�
    int numTotalNets = wires.size();
    int numTotalPins = 0;

    // �ṹ�壺�洢���Źؼ���Ϣ������ƥ�䣩
    struct PinInfo {
        wxString cellName;    // ����Ԫ������
        wxString pinType;     // �������ͣ�I/O��
        wxPoint absPos;       // ���ž������꣨��������ϵ��
        wxPoint offset;       // �������Ԫ����ƫ������
    };
    std::vector<PinInfo> allPins;

    // 1. Ԥ��������Ԫ����������Ϣ����������+���ͣ�
    for (const auto& elem : elements)
    {
        wxPoint elemPos = elem.GetPos();          // ��ȡԪ���ڻ����ľ���λ��
        wxString cellName = elem.GetName();

        // ������������
        for (const auto& pin : elem.GetInputPins())
        {
            // �������ž������� = Ԫ��λ�� + �������ƫ��
            wxPoint pinAbsPos(
                elemPos.x + pin.pos.x,
                elemPos.y + pin.pos.y
            );
            allPins.push_back({
                cellName,
                "I",  // �������ű��
                pinAbsPos,
                wxPoint(pin.pos.x, pin.pos.y)  // ���ƫ��
                });
        }

        // �����������
        for (const auto& pin : elem.GetOutputPins())
        {
            wxPoint pinAbsPos(
                elemPos.x + pin.pos.x,
                elemPos.y + pin.pos.y
            );
            allPins.push_back({
                cellName,
                "O",  // ������ű��
                pinAbsPos,
                wxPoint(pin.pos.x, pin.pos.y)
                });
        }

        // ���⴦��"Pin (Output)"��Ԫ����������Ϊ������ţ�
        if (cellName == "Pin (Output)")
        {
            allPins.push_back({
                cellName,
                "O",  // ��ȷΪ�������
                elemPos,  // ����λ�ü�Ϊ����λ��
                wxPoint(0, 0)  // �������ƫ��Ϊ(0,0)
                });
        }
    }

    // 2. �������е��ߣ�����������Ϣ
    wxString content;
    for (int netIdx = 0; netIdx < numTotalNets; ++netIdx)
    {
        const auto& wire = wires[netIdx];
        if (wire.pts.size() < 2)  // ������Ч���ߣ�������Ҫ�����յ㣩
            continue;

        // ���������ߵ������յ���Ϊ��Ч���ţ������м���Ƶ㣩
        std::vector<wxPoint> validPinPositions;
        validPinPositions.push_back(wire.pts[0].pos);                // ���
        validPinPositions.push_back(wire.pts[wire.pts.size() - 1].pos);  // �յ�
        int netDegree = validPinPositions.size();  // �̶�Ϊ2����Ч���ߣ�
        numTotalPins += netDegree;

        // д������ͷ����Ϣ���������+��������
        wxString netName = wxString::Format("n%d", netIdx);
        content += wxString::Format("NetDegree %d %s\n", netDegree, netName);

        // 3. ƥ��ÿ����Ч���ŵ���Ӧ��Ԫ��
        const int COORD_TOLERANCE = 3;  // ����������̣�3��������Ϊƥ�䣩
        for (const auto& pinPos : validPinPositions)
        {
            wxString cellName = "unknown_cell";
            wxString pinType = "I";
            wxPoint offset(0, 0);

            // ����Ԥ����������б�����������ƥ�������
            for (const auto& pinInfo : allPins)
            {
                int dx = abs(pinPos.x - pinInfo.absPos.x);
                int dy = abs(pinPos.y - pinInfo.absPos.y);
                if (dx <= COORD_TOLERANCE && dy <= COORD_TOLERANCE)
                {
                    cellName = pinInfo.cellName;
                    pinType = pinInfo.pinType;
                    offset = pinInfo.offset;
                    break;  // �ҵ�ƥ������ź��˳�ѭ��
                }
            }

            // д��������Ϣ����ʽ��Ԫ���� ����:Xƫ�� Yƫ�ƣ�
            content += wxString::Format("%s %s:%d %d\n",
                cellName, pinType, offset.x, offset.y);
        }
    }

    // 4. д���ļ�ͷ������������������������
    wxString header;
    header += wxString::Format("NumNets %d\n", numTotalNets);
    header += wxString::Format("NumPins %d\n", numTotalPins);
    content = header + content;

    // д���ļ����ر�
    file.Write(content);
    file.Close();*/
    return true;
}

// 3. �������޸ģ�.node�ļ����津������������ԭ�߼���
void MainFrame::DoFileSaveAsNode()
{
    wxFileDialog saveDialog(this,
        "Save as BookShelf .node File",
        "",
        "circuit.node",  // Ĭ���ļ���
        "BookShelf Node Files (*.node)|*.node",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (saveDialog.ShowModal() == wxID_CANCEL)
        return;

    wxString filePath = saveDialog.GetPath();
    bool success = SaveAsNodeFile(filePath);
    if (success)
    {
        SetStatusText(wxString::Format("Saved BookShelf .node file: %s", filePath));
    }
    else
    {
        wxMessageBox("Failed to save .node file (check file permissions)", "Error", wxOK | wxICON_ERROR);
    }
}

// 4. �������޸ģ�.net�ļ����津������������ԭ�߼���
void MainFrame::DoFileSaveAsNet()
{
    wxFileDialog saveDialog(this,
        "Save as BookShelf .net File",
        "",
        "circuit.net",  // Ĭ���ļ���
        "BookShelf Net Files (*.net)|*.net",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (saveDialog.ShowModal() == wxID_CANCEL)
        return;

    wxString filePath = saveDialog.GetPath();
    bool success = SaveAsNetFile(filePath);
    if (success)
    {
        SetStatusText(wxString::Format("Saved BookShelf .net file: %s", filePath));
    }
    else
    {
        wxMessageBox("Failed to save .net file (check file permissions)", "Error", wxOK | wxICON_ERROR);
    }
}






















// ��������������Ԫ����ڵ�
void MainFrame::AddLibraryNode(wxXmlNode* parent, const wxString& name, const wxString& desc) {
    wxXmlNode* lib = new wxXmlNode(wxXML_ELEMENT_NODE, wxT("lib"));
    lib->AddAttribute(wxT("name"), name);
    lib->AddAttribute(wxT("desc"), desc);
    parent->AddChild(lib);
}

// �������������ӵ��߽ڵ�
void MainFrame::AddWireNode(wxXmlNode* parent, const wxString& from, const wxString& to) {
    wxXmlNode* wire = new wxXmlNode(wxXML_ELEMENT_NODE, wxT("wire"));
    wire->AddAttribute(wxT("from"), from);
    wire->AddAttribute(wxT("to"), to);
    parent->AddChild(wire);
}
// ������ʵ��"����Ϊ"�����������״α��棩
void MainFrame::DoFileSaveAs() {
    // �����ļ�ѡ��Ի���
    wxFileDialog saveDialog(
        this,
        "����Ϊ",
        "",
        "Untitled.circ",  // Ĭ���ļ���
        "��·�ļ� (*.circ)|*.circ|�����ļ� (*.*)|*.*",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT  // ��ʾ���������ļ�
    );

    // �û�ȡ���򷵻�
    if (saveDialog.ShowModal() != wxID_OK) {
        return;
    }

    // ��ȡ�û�ѡ���·��
    wxString newPath = saveDialog.GetPath();
    m_currentFilePath = newPath;

    // ִ�б���
    DoFileSave();

    // ����ѡ�����ӵ�����ļ���ʷ
    static_cast<MainMenuBar*>(GetMenuBar())->AddFileToHistory(newPath);
}




void MainFrame::OnAbout(wxCommandEvent&)
{
    wxMessageBox(wxString::Format(wxT("MyLogisim\n%s"), wxVERSION_STRING),
        wxT("About"), wxOK | wxICON_INFORMATION, this);
}

void MainFrame::DoEditUndo()
{
}

void MainFrame::DoEditCut() { wxMessageBox("Edit->Cut"); }
void MainFrame::DoEditCopy() { wxMessageBox("Edit->Copy"); }
void MainFrame::DoEditPaste() { wxMessageBox("Edit->Paste"); }
void MainFrame::DoEditDelete() { 
}
void MainFrame::DoEditDuplicate() { wxMessageBox("Edit->Duplicate"); }
void MainFrame::DoEditSelectAll() { wxMessageBox("Edit->SelectAll"); }
void MainFrame::DoEditRaiseSel() { wxMessageBox("Edit->Raise Selection"); }
void MainFrame::DoEditLowerSel() { wxMessageBox("Edit->Lower Selection"); }
void MainFrame::DoEditRaiseTop() { wxMessageBox("Edit->Raise to Top"); }
void MainFrame::DoEditLowerBottom() { wxMessageBox("Edit->Lower to Bottom"); }
void MainFrame::DoEditAddVertex() { wxMessageBox("Edit->Add Vertex"); }
void MainFrame::DoEditRemoveVertex() { wxMessageBox("Edit->Remove Vertex"); }

void MainFrame::DoProjectAddCircuit() { wxMessageBox("Project->Add Circuit"); }
void MainFrame::DoProjectLoadLibrary() { wxMessageBox("Project->Load Library"); }
void MainFrame::DoProjectUnloadLibraries() { wxMessageBox("Project->Unload Libraries"); }
void MainFrame::DoProjectMoveCircuitUp() { wxMessageBox("Project->Move Circuit Up"); }
void MainFrame::DoProjectMoveCircuitDown() { wxMessageBox("Project->Move Circuit Down"); }
void MainFrame::DoProjectSetAsMain() { wxMessageBox("Project->Set As Main Circuit"); }
void MainFrame::DoProjectRemoveCircuit() { wxMessageBox("Project->Remove Circuit"); }
void MainFrame::DoProjectRevertAppearance() { wxMessageBox("Project->Revert Appearance"); }
void MainFrame::DoProjectViewToolbox() { wxMessageBox("Project->View Toolbox"); }
void MainFrame::DoProjectViewSimTree() { wxMessageBox("Project->View Simulation Tree"); }
void MainFrame::DoProjectEditLayout() { wxMessageBox("Project->Edit Circuit Layout"); }
void MainFrame::DoProjectEditAppearance() { wxMessageBox("Project->Edit Circuit Appearance"); }
void MainFrame::DoProjectAnalyzeCircuit() { wxMessageBox("Project->Analyze Circuit"); }
void MainFrame::DoProjectGetStats() { wxMessageBox("Project->Get Circuit Statistics"); }
void MainFrame::DoProjectOptions() { wxMessageBox("Project->Options"); }

void MainFrame::DoSimSetEnabled(bool on)
{
    wxMessageBox(wxString::Format("Simulation %s", on ? "enabled" : "disabled"));
}
void MainFrame::DoSimReset() { wxMessageBox("Simulation reset"); }
void MainFrame::DoSimStep() { wxMessageBox("Simulation step"); }
void MainFrame::DoSimGoOut() { wxMessageBox("Go Out To State"); }
void MainFrame::DoSimGoIn() { wxMessageBox("Go In To State"); }
void MainFrame::DoSimTickOnce() { wxMessageBox("Tick Once"); }
void MainFrame::DoSimTicksEnabled(bool on)
{
    wxMessageBox(wxString::Format("Ticks %s", on ? "enabled" : "disabled"));
}
void MainFrame::DoSimSetTickFreq(int hz)
{
    wxMessageBox(wxString::Format("Tick frequency set to %d Hz", hz));
}
void MainFrame::DoSimLogging() { wxMessageBox("Logging dialog"); }

void MainFrame::DoWindowCombinationalAnalysis()
{
    wxMessageBox("Window->Combinational Analysis");
}
void MainFrame::DoWindowPreferences()
{
    wxMessageBox("Window->Preferences");
}
void MainFrame::DoWindowSwitchToDoc(const wxString& title)
{
    wxMessageBox("Switch to document: " + title);
    // �����߼�����������л�����Ӧ�Ӵ��ڻ���ͼ
}

void MainFrame::DoHelpTutorial()
{
    wxMessageBox("Help->Tutorial");
}
void MainFrame::DoHelpUserGuide()
{
    wxMessageBox("Help->User's Guide");
}
void MainFrame::DoHelpLibraryRef()
{
    wxMessageBox("Help->Library Reference");
}
void MainFrame::DoHelpAbout()
{
    wxMessageBox(wxString::Format("MyLogisim\n%s", wxVERSION_STRING),
        wxT("About"), wxOK | wxICON_INFORMATION, this);
}


void MainFrame::OnOpenFileFromTree(wxCommandEvent& evt) {
    wxString path = evt.GetString();
    m_verilogEditor->OpenFile(path);   // 你已有的打开文件逻辑
    m_currentFilePath = path;
    FileNode* fn = sigTree->GetFileNode(path.ToStdString());
    m_sigFlowTreePanel->SetFileNode(fn);

    m_canvas->SaveOrNotWindow();
    m_canvas->SetFileNode(fn);
    
    RefreshTitle();
}


void MainFrame::OnUndoStackChanged()
{
    wxMenuBar* bar = GetMenuBar();
    if (!bar) return;

    wxMenu* editMenu = bar->GetMenu(1);      // Edit �˵�
    if (!editMenu) return;

    wxMenuItem* undoItem = editMenu->FindItem(wxID_UNDO);
    if (undoItem)
    {

    }

}

void MainFrame::OnClose(wxCloseEvent& event) {
    // 1. �����ļ���
    wxString fileName = m_currentFilePath.IsEmpty()
        ? wxString("Untitled")
        : wxFileName(m_currentFilePath).GetFullName();

    // 2. ʹ�� wxMessageDialog ԭ���Ի��򣨽�����������ť��
    // ȥ��ͼ����ز���������Ĭ�ϵ� wxICON_QUESTION����ѡ��
    wxMessageDialog dialog(this,
        wxString::Format("What should happen to your unsaved changes to %s?", fileName),
        "Confirm Close",
        wxYES_NO | wxCANCEL | wxYES_DEFAULT); // ������������ť������

    // 3. ������ť�߼�
    int result = dialog.ShowModal();
    switch (result) {
    case wxID_YES:
        if (m_currentFilePath.IsEmpty()) {
            DoFileSaveAs();
        }
        else {
            DoFileSave();
        }
        event.Skip(); // �����رմ���
        break;
    case wxID_NO:
        event.Skip(); // �����رմ��ڣ������棩
        break;
    case wxID_CANCEL:
        // ��ִ�� Skip()����ֹ���ڹر�
        break;
    }
}

wxString MainFrame::GetWorkspaceCopyPath(const wxString& m_currentFilePath) {
    // 1. 创建文件对象
    wxFileName fileObj(m_currentFilePath);

    // 2. 计算相对于项目根目录的相对路径
    // 执行后，fileObj 将不再存有绝对路径，而是变为 "src/top.v" 这种形式
    if (fileObj.MakeRelativeTo(m_currentProjectPath)) {

        // 3. 将相对路径拼接到工作区根目录下
        // 这里的路径加法会自动处理反斜杠
        wxString targetPath = m_workspacePath + wxFileName::GetPathSeparator() + fileObj.GetFullPath();

        return targetPath;
    }

    return wxEmptyString; // 如果不在项目内，返回空
}

void MainFrame::OnAnalysisComplete(wxThreadEvent& event) {
    AnalysisResult result = event.GetPayload<AnalysisResult>();


    if (result.linted) m_verilogEditor->VisualFeedBack(result.lint);
    //if (result.parsed) m_canvas->
}

void MainFrame::OnStcModified(wxStyledTextEvent& event)
{
    if (m_verilogEditor->m_isLoading) return;
    int type = event.GetModificationType();
    int pos = event.GetPosition();
    int len = event.GetLength();
    wxString text = event.GetText();

    if (type & wxSTC_MOD_INSERTTEXT)
    {
        m_structuring->OnInsert(pos, text.ToStdString());
    }

    if (type & wxSTC_MOD_DELETETEXT)
    {
        m_structuring->OnDelete(pos, len);
    }

    // 启动或重置定时器（例如 300ms）
    m_textTimer->StartOnce(300);
}

void MainFrame::OnAnalysisTimer(wxTimerEvent&)
{
    if (m_structuring->IsEmpty())
        return;

    int L = m_structuring->Start();
    int R = m_structuring->End();

    wxString delta = m_verilogEditor->GetTextRange(L, R);
    wxString stable = m_verilogEditor->GetTextRange(0, L) + m_verilogEditor->GetTextRange(R, m_verilogEditor->GetLength());

    wxLogDebug("delta:\n" + delta + "\n");
    wxLogDebug("stable:\n" + stable);

    /*
    if (delta.StartsWith("module")) {
        // 如果已经是以 module 开头，可以不做处理或执行其他逻辑
    }
    else {
        // 只有不以 module 开头时，才包裹模板
        delta = "module _tmp;\n" + delta + "\nendmodule\n";
    }*/
    delta = "module _tmp;\n" + delta + "\nendmodule\n";
    

    AnaPac ap  = StructuringX(delta.ToStdString());
    if (!ap.has_error) {
        m_structuring->Clear();
        TSTreeCursor cursor = ts_tree_cursor_new(ap.node);
        std::string fp = m_verilogEditor->m_currentFilePath.ToStdString
            ();
        std::string code = delta.ToStdString();
        sigTree->UpdateTreeFromTS(&cursor, sigTree->GetFileNode(fp)->GetChildren()[0], fp, code);



    }
    // 交给分析模块
    //AnalyzeDelta(L, delta.ToStdString());

    //m_structuring.Clear();
}


// 在 MainFrame 中实现
void MainFrame::RefreshTitle() {
    wxString title = "SigFlow";
    wxFileName fileObj(m_currentFilePath);
    if (fileObj.MakeRelativeTo(m_currentProjectPath)) {
        if (!m_projectName.IsEmpty()) {
            title += " [" + m_projectName  + wxFileName::GetPathSeparator() + fileObj.GetFullPath() + "]";
        }

    }
    else {
        title += " [" + m_projectName + wxFileName::GetPathSeparator() + "]";
    }
    this->SetTitle(title);
}


void MainFrame::OnSFNodeActivated(wxCommandEvent& event) {
    SigTreeNode* node = static_cast<SigTreeNode*>(event.GetClientData());
    if (node && m_sfnPropertyPanel) {
        m_sfnPropertyPanel->LoadNode(node);
    }
}
void MainFrame:: OnSFTreeChanged(wxCommandEvent& event) {
    m_sigFlowTreePanel->Fresh();
    m_sfnPropertyPanel->Fresh();
    m_canvas->Refresh();
}

void MainFrame::OnSFNodeAdded(wxCommandEvent& event) {
    OnSFTreeChanged(event);
    m_canvas->SigFlowNodeAdded(static_cast<SigTreeNode*>(event.GetClientData()));
}

void MainFrame::OnSFNodeDeleted(wxCommandEvent& event) {
    OnSFTreeChanged(event);
    m_canvas->SigFlowNodeDeleted(static_cast<SigTreeNode*>(event.GetClientData()));
}

void MainFrame::OnSFNodeChanged(wxCommandEvent& event) {
    OnSFTreeChanged(event);
    m_canvas->SigFlowNodeChanged(static_cast<SigTreeNode*>(event.GetClientData()));
}



void MainFrame::PropertyLoadNode(SigTreeNode* node) {
    m_sfnPropertyPanel->LoadNode(node);
}




// ============================================================================
// Verilator 仿真接口实现
// ============================================================================

// 从 sigflow.project 读取编译配置
bool MainFrame::LoadProjectConfig(const wxString& projectPath, 
                                   wxString& outTopModule,
                                   std::vector<wxString>& outSourceFiles)
{
    wxString configPath = projectPath + "\\sigflow.project";
    
    if (!wxFileExists(configPath)) {
        OutputDebugStringA("sigflow.project not found\n");
        return false;
    }
    
    // 读取文件内容
    wxFile file(configPath, wxFile::read);
    if (!file.IsOpened()) {
        OutputDebugStringA("Failed to open sigflow.project\n");
        return false;
    }
    
    wxString jsonContent;
    file.ReadAll(&jsonContent);
    file.Close();
    
    // 解析 JSON
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    
    std::string jsonStr = jsonContent.ToUTF8().data();
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    
    if (!reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.size(), &root, &errors)) {
        OutputDebugStringA(("JSON parse error: " + errors + "\n").c_str());
        return false;
    }
    
    // 读取 top_module
    if (root.isMember("build") && root["build"].isMember("top_module")) {
        const Json::Value& topModules = root["build"]["top_module"];
        if (topModules.isArray() && !topModules.empty()) {
            outTopModule = wxString::FromUTF8(topModules[0].asString());
            OutputDebugStringA(("Top module from config: " + outTopModule.ToStdString() + "\n").c_str());
        }
    }
    
    // 读取 source_files
    if (root.isMember("paths") && root["paths"].isMember("source_files")) {
        const Json::Value& sources = root["paths"]["source_files"];
        if (sources.isArray()) {
            for (const auto& src : sources) {
                wxString relPath = wxString::FromUTF8(src.asString());
                wxString fullPath = projectPath + "\\" + relPath;
                // 将正斜杠转换为反斜杠
                fullPath.Replace("/", "\\");
                outSourceFiles.push_back(fullPath);
                OutputDebugStringA(("Source file: " + fullPath.ToStdString() + "\n").c_str());
            }
        }
    }
    
    // 读取 library_files
    if (root.isMember("paths") && root["paths"].isMember("library_files")) {
        const Json::Value& libs = root["paths"]["library_files"];
        if (libs.isArray()) {
            for (const auto& lib : libs) {
                wxString relPath = wxString::FromUTF8(lib.asString());
                if (!relPath.IsEmpty()) {
                    wxString fullPath = projectPath + "\\" + relPath;
                    fullPath.Replace("/", "\\");
                    outSourceFiles.push_back(fullPath);
                    OutputDebugStringA(("Library file: " + fullPath.ToStdString() + "\n").c_str());
                }
            }
        }
    }
    
    return !outTopModule.IsEmpty() && !outSourceFiles.empty();
}

void MainFrame::DoSimCompile()
{
    OutputDebugStringA("=== DoSimCompile ENTER ===\n");
    
    // 安全检查
    if (!this) {
        OutputDebugStringA("ERROR: this is NULL\n");
        return;
    }
    
    // 1. 检查项目是否打开 - 使用临时变量避免多次访问
    wxString projectPath = m_currentProjectPath;
    if (projectPath.IsEmpty()) {
        wxMessageBox(wxT("请先打开项目"), wxT("编译仿真"), wxOK | wxICON_WARNING);
        return;
    }
    
    // 2. 从 sigflow.project 读取配置
    wxString topModule;
    std::vector<wxString> verilogFiles;
    
    if (!LoadProjectConfig(projectPath, topModule, verilogFiles)) {
        // 配置文件读取失败，回退到手动收集和输入
        OutputDebugStringA("Failed to load project config, falling back to manual mode\n");
        
        // 手动收集 src/*.v 和 lib/*.v
        wxString srcDir = projectPath + "\\src";
        wxString libDir = projectPath + "\\lib";
        
        if (wxDir::Exists(srcDir)) {
            wxDir dir;
            if (dir.Open(srcDir)) {
                wxString filename;
                bool hasFile = dir.GetFirst(&filename, "*.v", wxDIR_FILES);
                while (hasFile) {
                    verilogFiles.push_back(srcDir + "\\" + filename);
                    hasFile = dir.GetNext(&filename);
                }
            }
        }
        
        if (wxDir::Exists(libDir)) {
            wxDir dir;
            if (dir.Open(libDir)) {
                wxString filename;
                bool hasFile = dir.GetFirst(&filename, "*.v", wxDIR_FILES);
                while (hasFile) {
                    verilogFiles.push_back(libDir + "\\" + filename);
                    hasFile = dir.GetNext(&filename);
                }
            }
        }
        
        if (verilogFiles.empty()) {
            wxMessageBox(wxT("项目中没有找到 Verilog 文件\n请确保项目包含 src/ 或 lib/ 目录"), 
                         wxT("编译仿真"), wxOK | wxICON_WARNING);
            return;
        }
        
        // 询问顶层模块名
        wxFileName projectFn(projectPath);
        wxString defaultTopModule = projectFn.GetFullName();
        
        wxString prompt;
        prompt.Printf("找到 %u 个 Verilog 文件\n请输入顶层模块名称:", (unsigned)verilogFiles.size());
        
        wxTextEntryDialog dialog(NULL, prompt, "编译仿真", defaultTopModule);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        
        topModule = dialog.GetValue();
        if (topModule.IsEmpty()) {
            wxMessageBox(wxT("顶层模块名称不能为空"), wxT("编译仿真"), wxOK | wxICON_WARNING);
            return;
        }
    } else {
        // 配置读取成功，询问用户确认
        wxString confirmMsg = wxT("从 sigflow.project 读取的配置:\n顶层模块: ");
        confirmMsg += topModule;
        confirmMsg += wxT("\n源文件数: ");
        confirmMsg += wxString::Format(wxT("%u"), (unsigned)verilogFiles.size());
        confirmMsg += wxT("\n\n确认编译?");
        
        int result = wxMessageBox(confirmMsg, wxT("编译仿真"), wxYES_NO | wxICON_QUESTION);
        if (result != wxYES) {
            return;
        }
    }
    if (topModule.IsEmpty()) {
        wxMessageBox(wxT("顶层模块名称不能为空"), wxT("编译仿真"), wxOK | wxICON_WARNING);
        return;
    }
    
    // 4. 初始化仿真引擎
    if (!m_simEngine) {
        m_simEngine = std::make_unique<SimulationEngine>();
    }
    
    // 关键：设置项目根目录（用于确定.sigflow缓存位置）
    m_simEngine->SetProjectRoot(projectPath);
    
    // 5. 设置编译输出回调（显示编译日志）
    m_simEngine->SetCompileOutputCallback([this](const wxString& line, bool isError) {
        // 可以在这里输出到日志窗口
        OutputDebugStringA(isError ? "[ERR] " : "[OUT] ");
        OutputDebugStringA(line.ToUTF8());
        OutputDebugStringA("\n");
    });
    
    // 6. 执行编译
    SetStatusText(wxT("正在编译仿真模型..."), 0);
    SimulationCompileResult result = m_simEngine->Compile(topModule, verilogFiles);
    SetStatusText(result.success ? wxT("编译完成") : wxT("编译失败"), 0);
    
    // 7. 显示结果 - 使用字符串拼接避免 Printf 问题
    if (result.success) {
        wxString successMsg = wxT("编译成功!\nDLL路径: ");
        successMsg += result.dllPath;
        wxMessageBox(successMsg, wxT("编译完成"), wxOK | wxICON_INFORMATION);
    } else {
        wxString errorMsg = wxT("编译失败\n\n");
        if (result.errorMessage.Contains(wxT("Verilator"))) {
            errorMsg += wxT("Verilator 阶段失败，请检查代码语法\n");
        } else if (result.errorMessage.Contains(wxT("DLL"))) {
            errorMsg += wxT("DLL 编译失败\n");
            errorMsg += wxT("建议：检查 .sigflow\\sim\\");
            errorMsg += topModule;
            errorMsg += wxT("\\compile_dll.bat 手动调试");
        }
        errorMsg += wxT("\n\n详细错误：\n") + result.errorMessage;
        wxMessageBox(errorMsg, wxT("编译失败"), wxOK | wxICON_ERROR);
    }
    
    OutputDebugStringA("=== DoSimCompile EXIT ===\n");
}

void MainFrame::DoSimRun()
{
    if (!m_simEngine || !m_simEngine->IsCompiled("")) {
        wxMessageBox(wxT("请先编译仿真模型"), wxT("运行仿真"), wxOK | wxICON_WARNING);
        return;
    }

    // 选择输出VCD文件路径
    wxFileDialog saveDialog(
        this,
        "保存波形文件",
        m_currentProjectPath,
        "waveform.vcd",
        "VCD files (*.vcd)|*.vcd",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (saveDialog.ShowModal() == wxID_CANCEL) {
        return;
    }

    wxString vcdPath = saveDialog.GetPath();
    
    // 运行仿真
    SetStatusText(wxT("正在运行仿真..."), 0);
    SimulationRunResult result = m_simEngine->RunSimulation(vcdPath);

    if (result.success) {
        wxString msg = wxT("仿真完成!\n波形文件: ");
        msg += result.vcdPath;
        wxMessageBox(msg, wxT("仿真完成"), wxOK | wxICON_INFORMATION);
        
        // TODO: 打开波形查看器或显示波形
    } else {
        wxString msg = wxT("仿真失败!\n");
        msg += result.errorMessage;
        wxMessageBox(msg, wxT("仿真错误"), wxOK | wxICON_ERROR);
    }

    SetStatusText(wxT("就绪"), 0);
}

void MainFrame::DoSimClean()
{
    if (!m_simEngine) {
        wxMessageBox(wxT("没有仿真缓存需要清理"), wxT("清理缓存"), wxOK | wxICON_INFORMATION);
        return;
    }

    wxString topModule = wxGetTextFromUser(
        wxT("请输入要清理的顶层模块名称 (留空清理所有):"),
        wxT("清理仿真缓存"),
        wxT(""),
        this
    );

    if (topModule.IsEmpty()) {
        // 询问是否清理所有
        int result = wxMessageBox(
            wxT("确定要清理所有仿真缓存吗?"),
            wxT("确认清理"),
            wxYES_NO | wxICON_QUESTION
        );
        if (result != wxYES) {
            return;
        }
    }

    if (m_simEngine->CleanCache(topModule)) {
        wxMessageBox(wxT("缓存清理完成"), wxT("清理完成"), wxOK | wxICON_INFORMATION);
    } else {
        wxMessageBox(wxT("缓存清理失败"), wxT("错误"), wxOK | wxICON_ERROR);
    }
}
