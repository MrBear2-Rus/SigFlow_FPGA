#include "ProjectStartWindow.h"
#include "platform/PlatformPaths.h"
#include "platform/Log.h"
#include "MainFrame.h"
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/dirdlg.h>
#include <wx/filefn.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>

#include <algorithm>
#include <vector>

// 事件ID
enum
{
    ID_BTN_NEW_PROJECT = wxID_HIGHEST + 100,
    ID_BTN_OPEN_PROJECT,
    ID_BTN_EXIT,
    ID_LIST_RECENT_PROJECTS,
    ID_BTN_DELETE_PROJECT,
};

// 事件表
wxBEGIN_EVENT_TABLE(ProjectStartWindow, wxDialog)
EVT_BUTTON(ID_BTN_NEW_PROJECT, ProjectStartWindow::OnNewProject)
EVT_BUTTON(ID_BTN_OPEN_PROJECT, ProjectStartWindow::OnOpenProject)
EVT_BUTTON(ID_BTN_EXIT, ProjectStartWindow::OnExit)
EVT_BUTTON(ID_BTN_DELETE_PROJECT, ProjectStartWindow::OnDeleteProject)
EVT_LIST_ITEM_ACTIVATED(ID_LIST_RECENT_PROJECTS, ProjectStartWindow::OnRecentProjectDblClick)
wxEND_EVENT_TABLE()

namespace {

// Logs belong to the user's profile.  The executable directory is often
// read-only (and may be protected by Windows), including for portable builds.
wxString StartupLogPath()
{
    const wxString directory = wxStandardPaths::Get().GetUserLocalDataDir();
    return directory.empty() ? wxString() : wxFileName(directory, "sigflow.log").GetFullPath();
}

wxString LegacyStartupLogPath()
{
    return sigflow::platform::JoinPath(sigflow::platform::ExecutableDir(), "sigflow.log");
}

#ifdef _WIN32
void RecoverHistoryFromLegacyLog(wxConfigBase& config)
{
    bool migrated = false;
    if (config.Read("/Migration/LegacyLogHistoryV1", &migrated) && migrated) return;
    if (config.Read("/Migration/RegistryHistoryV1", &migrated) && migrated) return;

    wxLogNull suppressExpectedFileErrors;
    wxFile oldLog(LegacyStartupLogPath());
    if (!oldLog.IsOpened() || oldLog.Length() > 2 * 1024 * 1024) return;
    wxString contents;
    if (!oldLog.ReadAll(&contents)) return;

    std::vector<wxString> recovered;
    wxStringTokenizer lines(contents, "\r\n", wxTOKEN_STRTOK);
    const wxString addMarker = "[INFO] Add to history: ";
    const wxString removeMarker = "[INFO] Removed from history: ";
    while (lines.HasMoreTokens()) {
        const wxString line = lines.GetNextToken();
        const int addAt = line.Find(addMarker);
        const int removeAt = line.Find(removeMarker);
        if (addAt == wxNOT_FOUND && removeAt == wxNOT_FOUND) continue;
        const bool added = addAt != wxNOT_FOUND;
        wxString path = line.Mid((added ? addAt : removeAt) +
                                 (added ? addMarker : removeMarker).length());
        path.Trim(true).Trim(false);
        if (path.empty()) continue;
        recovered.erase(std::remove_if(recovered.begin(), recovered.end(),
                                       [&path](const wxString& item) {
                                           return item.CmpNoCase(path) == 0;
                                       }), recovered.end());
        if (added) recovered.insert(recovered.begin(), path);
    }

    // Keep the new file's history in front.  The old log is a recovery source,
    // not the authority once the user has opened projects with this build.
    std::vector<wxString> merged;
    for (int index = 1; index <= 9; ++index) {
        wxString path;
        if (config.Read(wxString::Format("/file%d", index), &path) && !path.empty()) {
            merged.push_back(path);
        }
    }
    const std::size_t currentCount = merged.size();
    for (const wxString& path : recovered) {
        const bool duplicate = std::any_of(merged.begin(), merged.end(),
                                           [&path](const wxString& item) {
                                               return item.CmpNoCase(path) == 0;
                                           });
        if (!duplicate && merged.size() < 9) merged.push_back(path);
    }
    if (merged.size() > currentCount) {
        for (int index = 1; index <= 9; ++index) {
            config.Write(wxString::Format("/file%d", index),
                         index <= static_cast<int>(merged.size()) ? merged[index - 1] : wxString());
        }
    }
    config.Write("/Migration/LegacyLogHistoryV1", true);
    config.Flush();
}
#endif

} // namespace

// 构造函数：启动时加载配置文件中的历史记录
ProjectStartWindow::ProjectStartWindow(wxWindow* parent, wxWindowID id, const wxString& title,
    const wxPoint& pos, const wxSize& size, long style)
    : wxDialog(parent, id, title, pos, size, style)
{
    // Migrate old Windows registry history first; if that key is inaccessible,
    // recover the remaining recent-project events from the old startup log.
    wxConfigBase* config = wxConfig::Get();  // 统一身份，见 MainMenuBar::LoadHistory 的说明
#ifdef _WIN32
    RecoverHistoryFromLegacyLog(*config);
#endif
    m_fileHistory.Load(*config);

   

    // 初始化界面 + 加载历史记录到列表
    InitUI();

    LoadLogFromFile();

    LoadRecentProjects();

    Log("Application started");

    // 窗口居中
    Centre(wxBOTH);
    // wxApp 初始化时已自动注册全部 image handlers，不要再手动调用 wxInitAllImageHandlers
    wxBitmapBundle svgIcon = wxBitmapBundle::FromSVGFile(sigflow::platform::ResourcePath("res/svg_icons/icon.svg"), wxSize(24, 24));
    wxIcon icon = svgIcon.GetIconFor(this);
    if (icon.IsOk()) SetIcon(icon);
}


void ProjectStartWindow::LoadLogFromFile()
{
    if (!m_logCtrl) return;

    wxString logPath = StartupLogPath();
    if (logPath.empty() || !wxFileExists(logPath)) logPath = LegacyStartupLogPath();
    if (!wxFileExists(logPath)) return;

    wxLogNull suppressExpectedFileErrors;
    wxFile file(logPath);
    if (!file.IsOpened()) return;

    wxString content;
    if (!file.ReadAll(&content)) return;

    m_logCtrl->SetValue(content);  // 一次性加载全部日志

    // 滚动到底部
    m_logCtrl->ShowPosition(m_logCtrl->GetLastPosition());
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
    wxBoxSizer* rootSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* topSizer = new wxBoxSizer(wxHORIZONTAL);

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
    topSizer->Add(leftSizer, 4, wxEXPAND | wxALL, 10);

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

    topSizer->Add(rightSizer, 1, wxALIGN_CENTER | wxALL, 20);

    // 删除按钮
    wxButton* btnDelete = new wxButton(this, ID_BTN_DELETE_PROJECT, "Remove Selected");
    btnDelete->SetMinSize(btnSize);
    rightSizer->Add(btnDelete, 0, wxALL | wxALIGN_CENTER, 10);

    m_logCtrl = new wxTextCtrl(this, wxID_ANY, "",
        wxDefaultPosition, wxSize(-1, 150),
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);

    rootSizer->Add(topSizer, 1, wxEXPAND);
    rootSizer->Add(m_logCtrl, 0, wxEXPAND | wxALL, 5);

    SetSizer(rootSizer);
    SetMinSize(wxSize(800, 500));
}

void ProjectStartWindow::OnDeleteProject(wxCommandEvent& evt)
{
    long item = m_recentProjectsList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

    if (item == -1)
    {
        wxMessageBox("Please select a project first!", "Prompt", wxOK);
        return;
    }

    // 获取对应历史索引；列表项 data 可能因历史重建而失效，必须先做边界检查，
    // 否则 RemoveFileFromHistory 会用越界下标触发 wxArrayString::RemoveAt 断言。
    const long historyIndex = m_recentProjectsList->GetItemData(item);
    if (historyIndex < 0 || static_cast<size_t>(historyIndex) >= m_fileHistory.GetCount())
    {
        Log("Skipped stale recent-project entry.");
        LoadRecentProjects();
        return;
    }
    const size_t historyIdx = static_cast<size_t>(historyIndex);

    wxString path = m_fileHistory.GetHistoryFile(historyIdx);

    // 从历史记录删除
    m_fileHistory.RemoveFileFromHistory(historyIdx);

    Log("Removed from history: " + path);

    // 刷新UI
    LoadRecentProjects();

    // 保存到配置文件（关键！）
    SaveRecentProjects();
}

void ProjectStartWindow::Log(const wxString& msg, const wxString& level)
{
    wxString time = wxDateTime::Now().FormatISOTime();
    wxString line = "[" + time + "][" + level + "] " + msg + "\n";

    if (m_logCtrl)
        m_logCtrl->AppendText(line);

    // The in-window log remains useful if the profile itself is read-only.
    const wxString logPath = StartupLogPath();
    if (logPath.empty()) return;
    wxLogNull suppressExpectedFileErrors;
    const wxString directory = wxFileName(logPath).GetPath();
    if (!wxDirExists(directory) &&
        !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) return;
    wxFile file(logPath, wxFile::write_append);
    if (!file.IsOpened()) {
        SIGFLOW_LOG("ProjectStartWindow: cannot open startup log for append\n");
        return;
    }
    const wxScopedCharBuffer utf8 = line.ToUTF8();
    if (!utf8.data() ||
        file.Write(utf8.data(), utf8.length()) != static_cast<wxFileOffset>(utf8.length())) {
        SIGFLOW_LOG("ProjectStartWindow: cannot write startup log\n");
    }
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
    wxConfigBase* config = wxConfig::Get();  // 统一身份，见 MainMenuBar::LoadHistory 的说明
    m_fileHistory.Save(*config);
    config->Flush();
}

// 添加路径到历史记录
void ProjectStartWindow::AddProjectToHistory(const wxString& path)
{
    m_fileHistory.AddFileToHistory(path); // 自动去重，最新在前
    Log("Add to history: " + path);
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
        Log("Folder not exist: " + projectDir, "ERROR");
        wxMessageBox("Folder does not exist!", "Prompt", wxOK);
        return;
    }
    Log("Opening project: " + projectDir);

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
    const long historyIndex = m_recentProjectsList->GetItemData(itemIdx);
    if (historyIndex < 0 || static_cast<size_t>(historyIndex) >= m_fileHistory.GetCount()) return;
    const size_t historyIdx = static_cast<size_t>(historyIndex);
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
