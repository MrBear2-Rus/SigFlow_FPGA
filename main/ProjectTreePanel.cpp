#include "ProjectTreePanel.h"
#include <wx/dir.h>
#include <wx/filename.h>

ProjectTreePanel::ProjectTreePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    m_projectRoot = "";

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    m_tree = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_DEFAULT_STYLE);
   
    wxBoxSizer* toolSizer = new wxBoxSizer(wxHORIZONTAL);
    wxBitmapBundle bundleFile = wxBitmapBundle::FromSVGFile("res/svg_icons/new_file.svg", wxSize(16, 16));
    wxBitmapButton* nf = new wxBitmapButton(this, wxID_ANY, bundleFile, wxDefaultPosition, wxDefaultSize, wxBU_AUTODRAW);
    toolSizer->Add(nf, 0, wxALL, 2);

    // 2. 新建目录按钮 (nd)
    wxBitmapBundle bundleDir = wxBitmapBundle::FromSVGFile("res/svg_icons/new_dir.svg", wxSize(16, 16));
    wxBitmapButton* nd = new wxBitmapButton(this, wxID_ANY, bundleDir, wxDefaultPosition, wxDefaultSize, wxBU_AUTODRAW);
    toolSizer->Add(nd, 0, wxALL, 2);

    sizer->Add(toolSizer, 0, wxEXPAND | wxALL, 2);
    sizer->Add(m_tree, 1, wxEXPAND | wxALL, 0);

    this->SetSizer(sizer);
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &ProjectTreePanel::OnItemActivated, this);
    nf->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxTreeItemId selected = m_tree->GetSelection();
        if (selected.IsOk()) {
            this->NewFile(selected);
        }
        else {
            wxTreeItemId root = m_tree->GetRootItem();
            if (root.IsOk()) this->NewFile(root);
        }
        });

    nd->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxTreeItemId selected = m_tree->GetSelection();
        if (selected.IsOk()) {
            this->NewDir(selected);
        }
        else {
            wxTreeItemId root = m_tree->GetRootItem();
            if (root.IsOk()) this->NewDir(root);
        }
        });



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

    m_tree->Freeze();
    m_tree->DeleteAllItems();

    wxTreeItemId rootId = m_tree->AddRoot(
        wxFileName(m_projectRoot).GetFullName(),
        -1, -1,
        new FileTreeItemData(m_projectRoot)
    );

    // 调用时初始层级为 1
    BuildTree(m_projectRoot, rootId, 1);

    // 根节点总是展开
    m_tree->Expand(rootId);
    m_tree->Thaw();
}

void ProjectTreePanel::BuildTree(const wxString& path, wxTreeItemId parent, int level)
{
    wxDir dir(path);
    if (!dir.IsOpened()) return;

    wxString filename;
    // 使用默认 flag，不包含隐藏文件
    // 如果想更严谨，可以使用 wxDIR_DIRS | wxDIR_FILES (默认不含 wxDIR_HIDDEN)
    bool cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_DIRS | wxDIR_FILES);

    while (cont) {
        // 逻辑过滤：跳过以 . 开头的文件/文件夹
        if (filename.StartsWith(".")) {
            cont = dir.GetNext(&filename);
            continue;
        }

        wxFileName fn(path, filename);
        wxString full = fn.GetFullPath();

        if (wxDirExists(full)) {
            auto id = m_tree->AppendItem(
                parent,
                filename,
                -1, -1,
                new FileTreeItemData(full)
            );

            // 递归构建子树，层级 +1
            BuildTree(full, id, level + 1);

            // 展开逻辑：如果当前层级 <= 3，则展开该目录
            if (level < 3) {
                m_tree->Expand(id);
            }
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

wxString ProjectTreePanel::GetPathFromItem(wxTreeItemId id) {
    if (!id.IsOk()) return "";
    FileTreeItemData* data = static_cast<FileTreeItemData*>(m_tree->GetItemData(id));
    return data ? data->path : wxString("");
}

void ProjectTreePanel::NewFile(wxTreeItemId parent) {
    wxString parentPath = GetPathFromItem(parent);
    if (!wxDirExists(parentPath)) return;

    wxTextEntryDialog dlg(this, "Enter file name (with extension, e.g., top.v):", "New File");
    if (dlg.ShowModal() == wxID_OK) {
        wxFileName newFile(parentPath, dlg.GetValue());
        wxString fullPath = newFile.GetFullPath();

        if (newFile.FileExists()) {
            wxMessageBox("File already exists!", "Error", wxICON_ERROR);
            return;
        }

        // 物理创建
        wxFile file;
        if (file.Create(fullPath)) {
            file.Close();
            // UI 刷新：手动添加节点或全量刷新
            // 这里建议直接 AppendItem 保证交互生命力，无需重绘整个树
            m_tree->AppendItem(parent, dlg.GetValue(), -1, -1, new FileTreeItemData(fullPath));
            m_tree->Expand(parent);
        }
    }
}

// 2. 新建目录
void ProjectTreePanel::NewDir(wxTreeItemId parent) {
    wxString parentPath = GetPathFromItem(parent);
    if (!wxDirExists(parentPath)) return;

    wxTextEntryDialog dlg(this, "Enter directory name:", "New Directory");
    if (dlg.ShowModal() == wxID_OK) {
        wxFileName newDir(parentPath, "");
        newDir.AppendDir(dlg.GetValue());
        wxString fullPath = newDir.GetPath();

        if (wxDirExists(fullPath)) {
            wxMessageBox("Directory already exists!", "Error", wxICON_ERROR);
            return;
        }

        // 物理创建
        if (wxMkdir(fullPath)) {
            auto id = m_tree->AppendItem(parent, dlg.GetValue(), -1, -1, new FileTreeItemData(fullPath));
            m_tree->SetItemHasChildren(id, true);
            m_tree->Expand(parent);
        }
    }
}

// 3. 删除文件
void ProjectTreePanel::DelFile(wxTreeItemId fileId) {
    if (fileId == m_tree->GetRootItem()) return; // 不允许删除根节点

    wxString path = GetPathFromItem(fileId);
    if (wxMessageBox("Are you sure you want to delete this file?\n" + path,
        "Confirm Delete", wxYES_NO | wxICON_WARNING) == wxYES) {
        if (wxRemoveFile(path)) {
            m_tree->Delete(fileId);
        }
        else {
            wxMessageBox("Failed to delete file. It might be in use.", "Error", wxICON_ERROR);
        }
    }
}

// 4. 删除目录
void ProjectTreePanel::DelDir(wxTreeItemId dirId) {
    if (dirId == m_tree->GetRootItem()) return;

    wxString path = GetPathFromItem(dirId);
    if (wxMessageBox("Are you sure you want to delete this directory and ALL its contents?",
        "Confirm Delete", wxYES_NO | wxICON_ERROR) == wxYES) {

        // wxFileName::Rmdir 支持递归删除 (使用 wxPATH_RMDIR_RECURSIVE)
        if (wxFileName::Rmdir(path, wxPATH_RMDIR_RECURSIVE)) {
            m_tree->Delete(dirId);
        }
        else {
            wxMessageBox("Failed to delete directory.", "Error", wxICON_ERROR);
        }
    }
}
