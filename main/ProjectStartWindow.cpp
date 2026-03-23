#include "ProjectStartWindow.h"
#include "MainFrame.h"
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/dirdlg.h>
#include <wx/filefn.h>

// 事件ID
enum
{
    ID_BTN_NEW_PROJECT = wxID_HIGHEST + 100,
    ID_BTN_OPEN_PROJECT,
    ID_BTN_EXIT,
    ID_LIST_RECENT_PROJECTS
};

// 事件表
wxBEGIN_EVENT_TABLE(ProjectStartWindow, wxDialog)
EVT_BUTTON(ID_BTN_NEW_PROJECT, ProjectStartWindow::OnNewProject)
EVT_BUTTON(ID_BTN_OPEN_PROJECT, ProjectStartWindow::OnOpenProject)
EVT_BUTTON(ID_BTN_EXIT, ProjectStartWindow::OnExit)
EVT_LIST_ITEM_ACTIVATED(ID_LIST_RECENT_PROJECTS, ProjectStartWindow::OnRecentProjectDblClick)
wxEND_EVENT_TABLE()

// 构造函数：启动时加载配置文件中的历史记录
ProjectStartWindow::ProjectStartWindow(wxWindow* parent, wxWindowID id, const wxString& title,
    const wxPoint& pos, const wxSize& size, long style)
    : wxDialog(parent, id, title, pos, size, style)
{
    // 加载历史记录：从配置文件读取到内存
    wxConfig config("Sigflow");
    m_fileHistory.Load(config);

    // 初始化界面 + 加载历史记录到列表
    InitUI();
    LoadRecentProjects();

    // 窗口居中
    Centre(wxBOTH);
    wxInitAllImageHandlers();
    wxBitmapBundle svgIcon = wxBitmapBundle::FromSVGFile("res\\svg_icons\\icon.svg", wxSize(24, 24));
    wxIcon icon = svgIcon.GetIconFor(this);
    SetIcon(icon);
}

// 析构函数
ProjectStartWindow::~ProjectStartWindow()
{
    // 退出时保存历史记录到硬盘（可选启用）
    //SaveRecentProjects();
}

// 初始化界面：左侧列表 + 右侧垂直按钮
void ProjectStartWindow::InitUI()
{
    // 主容器：水平布局
    wxBoxSizer* mainSizer = new wxBoxSizer(wxHORIZONTAL);

    // 左侧区域：最近项目列表
    wxBoxSizer* leftSizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText* titleText = new wxStaticText(this, wxID_ANY,
        "Start", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    titleText->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
    leftSizer->Add(titleText, 0, wxALL | wxEXPAND, 10);

    // 最近项目列表控件
    m_recentProjectsList = new wxListCtrl(this, ID_LIST_RECENT_PROJECTS,
        wxDefaultPosition, wxSize(-1, 400),
        wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
    m_recentProjectsList->InsertColumn(0, "Project Path: ", wxLIST_FORMAT_LEFT, 500);
    leftSizer->Add(m_recentProjectsList, 1, wxALL | wxEXPAND, 10);
    mainSizer->Add(leftSizer, 4, wxEXPAND | wxALL, 10);

    // 右侧区域：垂直按钮组
    wxBoxSizer* rightSizer = new wxBoxSizer(wxVERTICAL);
    wxSize btnSize(150, 40); // 统一按钮尺寸

    // 新建项目按钮
    wxButton* btnNew = new wxButton(this, ID_BTN_NEW_PROJECT, "New Project");
    btnNew->SetMinSize(btnSize);
    rightSizer->Add(btnNew, 0, wxALL | wxALIGN_CENTER, 10);

    // 打开项目按钮
    wxButton* btnOpen = new wxButton(this, ID_BTN_OPEN_PROJECT, "Open Project");
    btnOpen->SetMinSize(btnSize);
    rightSizer->Add(btnOpen, 0, wxALL | wxALIGN_CENTER, 10);

    // 退出按钮
    wxButton* btnExit = new wxButton(this, ID_BTN_EXIT, "Exit");
    btnExit->SetMinSize(btnSize);
    rightSizer->Add(btnExit, 0, wxALL | wxALIGN_CENTER, 10);

    mainSizer->Add(rightSizer, 1, wxALIGN_CENTER | wxALL, 20);

    // 设置布局和窗口最小尺寸
    SetSizer(mainSizer);
    SetMinSize(wxSize(800, 500));
}

// 加载历史记录到列表控件
void ProjectStartWindow::LoadRecentProjects()
{
    m_recentProjectsList->DeleteAllItems();

    // 遍历历史记录并添加到列表
    for (size_t i = 0; i < m_fileHistory.GetCount(); ++i)
    {
        wxString path = m_fileHistory.GetHistoryFile(i);
        long itemIdx = m_recentProjectsList->InsertItem(i, path);
        m_recentProjectsList->SetItemData(itemIdx, i); // 绑定索引
    }
}

// 保存历史记录到配置文件
void ProjectStartWindow::SaveRecentProjects()
{
    wxConfig config("Sigflow");
    m_fileHistory.Save(config);
    config.Flush();
}

// 添加路径到历史记录
void ProjectStartWindow::AddProjectToHistory(const wxString& path)
{
    m_fileHistory.AddFileToHistory(path); // 自动去重，最新在前
    LoadRecentProjects(); // 刷新列表
    SaveRecentProjects(); // 立即保存
}

// 打开项目按钮事件
void ProjectStartWindow::OnOpenProject(wxCommandEvent& evt)
{
    wxDirDialog dlg(this, "Select Project Folder",
        wxGetCwd(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);

    if (dlg.ShowModal() == wxID_OK)
    {
        OpenProject(dlg.GetPath());
    }
}

// 实际打开项目逻辑
void ProjectStartWindow::OpenProject(const wxString& projectDir)
{
    if (!wxDir::Exists(projectDir))
    {
        wxMessageBox("Folder does not exist!", "Prompt", wxOK);
        return;
    }

    AddProjectToHistory(projectDir);
    m_selectedProjectDir = projectDir;
    SaveRecentProjects();
    EndModal(wxID_OK);
}

// 双击最近项目列表事件
void ProjectStartWindow::OnRecentProjectDblClick(wxListEvent& evt)
{
    long itemIdx = evt.GetIndex();
    if (itemIdx == -1) return;

    // 获取对应路径并打开项目
    size_t historyIdx = m_recentProjectsList->GetItemData(itemIdx);
    wxString projectDir = m_fileHistory.GetHistoryFile(historyIdx);
    OpenProject(projectDir);
}

// 新建项目按钮事件
void ProjectStartWindow::OnNewProject(wxCommandEvent& evt)
{
    m_selectedProjectDir = ""; // 空路径表示新建
    SaveRecentProjects();
    EndModal(wxID_OK);
}

// 退出按钮事件
void ProjectStartWindow::OnExit(wxCommandEvent& evt)
{
    EndModal(wxID_CANCEL);
}

// 获取选中的项目路径
wxString ProjectStartWindow::GetProjectDir()
{
    return m_selectedProjectDir;
}
