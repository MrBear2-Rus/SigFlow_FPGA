#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/fswatcher.h>

#define ID_OPEN_FILE_FROM_TREE (wxID_HIGHEST + 100)

// 当项目被加载时会发送该事件（Event string = project root path）
wxDECLARE_EVENT(EVT_PROJECT_LOADED, wxCommandEvent);

class ProjectTreePanel : public wxPanel {
public:
    ProjectTreePanel(wxWindow* parent);

    void LoadProject(const wxString& projectRoot);
    void RefreshTree();
    // 返回当前加载的项目根路径（如果有）
    wxString GetProjectRoot() const { return m_projectRoot; }

private:
    wxTreeCtrl* m_tree;
    wxString m_projectRoot;
    wxFileSystemWatcher* watcher;

    void BuildTree(const wxString& path, wxTreeItemId parent);
    void OnItemActivated(wxTreeEvent& evt);
    wxString ResolveItemPath(wxTreeItemId id);

    void OnFileSystemChanged(wxFileSystemWatcherEvent& evt);
    void AddWatchRecursive(const wxString& dir);

    void InitTreeIcons();
};

class FileTreeItemData : public wxTreeItemData
{
public:
    wxString path;
    FileTreeItemData(const wxString& p) : path(p) {}
};
