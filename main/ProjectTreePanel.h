#pragma once
#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/fswatcher.h>

#define ID_OPEN_FILE_FROM_TREE (wxID_HIGHEST + 100)

class ProjectTreePanel : public wxPanel {
public:
    ProjectTreePanel(wxWindow* parent);

    void LoadProject(const wxString& projectRoot);
    void RefreshTree();

private:
    wxTreeCtrl* m_tree;
    wxString m_projectRoot;
    wxFileSystemWatcher* watcher;

    void BuildTree(const wxString& path, wxTreeItemId parent, int level);
    void OnItemActivated(wxTreeEvent& evt);
    wxString ResolveItemPath(wxTreeItemId id);

    void OnFileSystemChanged(wxFileSystemWatcherEvent& evt);
    void AddWatchRecursive(const wxString& dir);


    wxString GetPathFromItem(wxTreeItemId id);

    void NewFile(wxTreeItemId parent);
    void DelFile(wxTreeItemId file);
    void NewDir(wxTreeItemId parent);
    void DelDir(wxTreeItemId dir);
};

class FileTreeItemData : public wxTreeItemData
{
public:
    wxString path;
    FileTreeItemData(const wxString& p) : path(p) {}
};
