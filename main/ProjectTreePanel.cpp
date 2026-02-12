#include "ProjectTreePanel.h"
#include <wx/dir.h>
#include <wx/filename.h>

ProjectTreePanel::ProjectTreePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    m_tree = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_DEFAULT_STYLE);
    m_projectRoot = "";

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_tree, 1, wxEXPAND | wxALL, 0);
    this->SetSizer(sizer);

    // 动态绑定：当 m_tree 触发 Item Activated 事件时，调用当前类的 OnItemActivated 函数
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &ProjectTreePanel::OnItemActivated, this);
}




bool IsValidSigFlowProject(const wxString& root)
{
    return wxFileExists(root + "/sigflow.project");
}


void ProjectTreePanel::AddWatchRecursive(const wxString& dir)
{
    watcher->Add(wxFileName(dir), wxFSW_EVENT_ALL);

    wxDir d(dir);
    if (!d.IsOpened()) return;

    wxString name;
    bool cont = d.GetFirst(&name, "", wxDIR_DIRS);
    while (cont) {
        AddWatchRecursive(dir + "/" + name);
        cont = d.GetNext(&name);
    }
}












void ProjectTreePanel::LoadProject(const wxString& projectRoot)
{
    m_projectRoot = projectRoot;

    if (!IsValidSigFlowProject(projectRoot)) {
        wxMessageBox("Not a SigFlow project. Opened as plain folder.");
    }

    if (!watcher) {
        watcher = new wxFileSystemWatcher();
        watcher->Bind(wxEVT_FSWATCHER,
            &ProjectTreePanel::OnFileSystemChanged, this);
    }
    else {
        watcher->RemoveAll();
    }

    AddWatchRecursive(projectRoot);
    RefreshTree();
}


void ProjectTreePanel::RefreshTree()
{
    if (m_projectRoot.IsEmpty() || !wxDirExists(m_projectRoot))
        return;

    m_tree->Freeze();   // 防止闪烁
    m_tree->DeleteAllItems();

    wxTreeItemId rootId = m_tree->AddRoot(
        wxFileName(m_projectRoot).GetFullName(),
        -1, -1,
        new FileTreeItemData(m_projectRoot)
    );

    BuildTree(m_projectRoot, rootId);

    m_tree->Expand(rootId);
    m_tree->Thaw();
}

void ProjectTreePanel::BuildTree(const wxString& path, wxTreeItemId parent)
{
    wxDir dir(path);
    if (!dir.IsOpened()) return;

    wxString filename;
    bool cont = dir.GetFirst(&filename);
    while (cont) {
        //if (filename.StartsWith(".")) continue;

        wxFileName fn(path, filename);
        wxString full = fn.GetFullPath();

        if (wxDirExists(full)) {
            auto id = m_tree->AppendItem(
                parent,
                filename,
                -1, -1,
                new FileTreeItemData(full)
            );
            m_tree->SetItemHasChildren(id, true);
            BuildTree(full, id);
        }
        else {
            m_tree->AppendItem(
                parent,
                filename,
                -1, -1,
                new FileTreeItemData(full)
            );
        }

        cont = dir.GetNext(&filename);
    }
}



void ProjectTreePanel::OnItemActivated(wxTreeEvent& evt) {
    wxTreeItemId id = evt.GetItem();
    wxString path = ResolveItemPath(id);

    if (!path.empty() && wxFileExists(path)) {
        wxCommandEvent openEvt(wxEVT_MENU, ID_OPEN_FILE_FROM_TREE);
        openEvt.SetString(path);
        wxPostEvent(GetParent(), openEvt);
    }
}

wxString ProjectTreePanel::ResolveItemPath(wxTreeItemId id)
{
    auto* data = dynamic_cast<FileTreeItemData*>(m_tree->GetItemData(id));
    return data ? data->path : wxString();

}

void ProjectTreePanel::OnFileSystemChanged(wxFileSystemWatcherEvent& evt) {
    RefreshTree();
}
