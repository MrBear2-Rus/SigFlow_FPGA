#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/filehistory.h>
#include <wx/config.h>

// 前置声明
class MainFrame;

class ProjectStartWindow : public wxDialog
{
public:
    // 构造/析构
    ProjectStartWindow(wxWindow* parent = nullptr, wxWindowID id = wxID_ANY,
        const wxString& title = wxString::FromUTF8("SigFlow"),
        const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize(800, 600),
        long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    ~ProjectStartWindow();
    void SaveRecentProjects();      // 保存历史记录到配置文件
    wxString m_selectedProjectDir;
    wxString GetProjectDir();
private:
    // 控件
    wxListCtrl* m_recentProjectsList = nullptr;
    // 历史记录核心对象（最多保存10条）
    wxFileHistory m_fileHistory{ 9 };
    MainFrame* m_mainFrame = nullptr;

    wxTextCtrl* m_logCtrl = nullptr;

    // 核心函数
    void InitUI();                  // 初始化界面
    void LoadRecentProjects();      // 加载历史记录到列表
   
    void AddProjectToHistory(const wxString& path); // 添加路径到历史
    void Log(const wxString& msg, const wxString& level = "INFO");
    // 事件处理
    void OnOpenProject(wxCommandEvent& evt);
    void OnNewProject(wxCommandEvent& evt);
    void OnExit(wxCommandEvent& evt);
    void OnRecentProjectDblClick(wxListEvent& evt);
    void OpenProject(const wxString& projectDir);
    void OnDeleteProject(wxCommandEvent& evt);
    void LoadLogFromFile();
    DECLARE_EVENT_TABLE()
};
