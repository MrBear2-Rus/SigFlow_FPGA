#include "ProjectTreePanel.h"
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/artprov.h>

wxDEFINE_EVENT(EVT_PROJECT_LOADED, wxCommandEvent);

namespace {

// 只订阅"内容发生变化"的文件系统事件。
//
// 绝对不能包含 wxFSW_EVENT_ACCESS：
//   Linux(inotify) 下，"打开目录 / 遍历目录 / 只读关闭目录"分别会产生
//   IN_OPEN / IN_ACCESS / IN_CLOSE_NOWRITE，而 wxFSW_EVENT_ACCESS 正好覆盖它们。
//   本面板的 RefreshTree() 恰恰是靠遍历目录来工作的，于是会形成
//       遍历目录 → 事件 → RefreshTree() → 再遍历目录 → 事件 → ...
//   的自激循环：UI 线程被 100% 占满，GTK 永远没机会派发第一次 expose，
//   主窗口表现为"整片全黑"。实测该循环可达约 1000 次/秒。
//   注意 Windows 之所以看不到这个问题，是因为 NTFS 默认关闭 last-access 更新，
//   FILE_NOTIFY_CHANGE_LAST_ACCESS 基本不会因为"读目录"而触发——属于典型的
//   一侧能跑、一侧必炸的跨平台陷阱。
//
// WARNING/ERROR 是错误事件（不是文件变化），保留以便上报监视失败。
const int kProjectWatchEvents =
    wxFSW_EVENT_CREATE | wxFSW_EVENT_DELETE | wxFSW_EVENT_RENAME |
    wxFSW_EVENT_MODIFY | wxFSW_EVENT_ATTRIB |
    wxFSW_EVENT_WARNING | wxFSW_EVENT_ERROR;

// 事件去抖窗口：合成/布局布线会在短时间内写入成百上千个文件，
// 逐个重建整棵树会让 UI 长时间失去响应。
const int kRefreshDebounceMs = 250;

} // namespace

ProjectTreePanel::ProjectTreePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    m_tree = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_DEFAULT_STYLE);
    m_projectRoot = "";

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_tree, 1, wxEXPAND | wxALL, 0);
    this->SetSizer(sizer);
    InitTreeIcons();
    // 动态绑定：当 m_tree 触发 Item Activated 事件时，调用当前类的 OnItemActivated 函数
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &ProjectTreePanel::OnItemActivated, this);

    m_refreshTimer.SetOwner(this, ID_TREE_REFRESH_TIMER);
    Bind(wxEVT_TIMER, &ProjectTreePanel::OnRefreshTimer, this, ID_TREE_REFRESH_TIMER);
}

ProjectTreePanel::~ProjectTreePanel()
{
    // 先停掉挂起的去抖定时器，避免销毁后回调打到悬空面板。
    m_refreshTimer.Stop();
    m_refreshPending = false;
    if (watcher) {
        // 先停止回调，再销毁 watcher，避免窗口关闭后事件投递到悬空面板。
        watcher->Unbind(wxEVT_FSWATCHER, &ProjectTreePanel::OnFileSystemChanged, this);
        watcher->RemoveAll();
        delete watcher;
        watcher = nullptr;
    }
}




bool IsValidSigFlowProject(const wxString& root)
{
    return wxFileExists(root + "/sigflow.project");
}


void ProjectTreePanel::AddWatchRecursive(const wxString& dir)
{
    // .sigflow 是程序生成目录；监听它会把构建和缓存变化误当成用户文件修改。
    if (!watcher || !wxDirExists(dir) || IsGeneratedPath(dir)) {
        return;
    }

    watcher->Add(wxFileName(dir), kProjectWatchEvents);

    wxDir d(dir);
    if (!d.IsOpened()) return;

    wxString name;
    // wxDIR_NO_FOLLOW：符号链接不再下钻，避免目录成环时无限递归/爆栈。
    bool cont = d.GetFirst(&name, "", wxDIR_DIRS | wxDIR_NO_FOLLOW);
    while (cont) {
        if (name.CmpNoCase(".sigflow") != 0) {
            AddWatchRecursive(dir + "/" + name);
        }
        cont = d.GetNext(&name);
    }
}

bool ProjectTreePanel::IsGeneratedPath(const wxString& path) const
{
    if (path.IsEmpty() || m_projectRoot.IsEmpty()) {
        return false;
    }

    wxString normalizedPath = path;
    normalizedPath.Replace("\\", "/");
    normalizedPath.MakeLower();

    wxString normalizedRoot = m_projectRoot;
    normalizedRoot.Replace("\\", "/");
    normalizedRoot.Trim(true);
    normalizedRoot.MakeLower();

    const wxString generatedRoot = normalizedRoot + "/.sigflow";
    return normalizedPath == generatedRoot || normalizedPath.StartsWith(generatedRoot + "/");
}


void ProjectTreePanel::InitTreeIcons() {

    wxSize sz = wxSize(24, 24);
    wxImageList* images = new wxImageList(sz.x, sz.y, true);

    // 添加图标（可以从艺术资源、图标文件或位图加载）
    // 这里的顺序要和上面的 enum 对应
    auto GetIcon = [&](const wxString& path) {
        wxBitmapBundle bundle = wxBitmapBundle::FromSVGFile(path, sz);
        return bundle.GetBitmap(sz);
        };

    images->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, wxSize(24, 24))); // Folder 0
    images->Add(wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_OTHER, wxSize(24, 24))); // File 1
    // 将图像列表交给树控件管理
    m_tree->AssignImageList(images);
    
}









void ProjectTreePanel::LoadProject(const wxString& projectRoot)
{
    m_projectRoot = projectRoot;

    // 通知宿主：项目已经加载（事件携带项目路径）
    wxCommandEvent evt(EVT_PROJECT_LOADED);
    evt.SetString(projectRoot);
    wxPostEvent(GetParent(), evt);

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
    // 显式重建时撤销挂起的去抖请求，避免紧接着再重建一次。
    m_refreshTimer.Stop();
    m_refreshPending = false;

    if (m_projectRoot.IsEmpty() || !wxDirExists(m_projectRoot))
        return;

    m_tree->Freeze();   // 防止闪烁
    m_tree->DeleteAllItems();

    wxTreeItemId rootId = m_tree->AddRoot(
        wxFileName(m_projectRoot).GetFullName(),
        0, 0,
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
    // wxDIR_NO_FOLLOW：不下钻符号链接（防目录成环导致无限递归）。
    bool cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES | wxDIR_DIRS | wxDIR_NO_FOLLOW);
    while (cont) {
        wxFileName fn(path, filename);
        wxString full = fn.GetFullPath();

        // 构建产物/缓存不进项目树：与 AddWatchRecursive 的跳过规则保持一致。
        // （此前只注释掉了"."前缀过滤且未排除 .sigflow，导致 Yosys/Verilator 的
        //   成千上万个中间文件都被塞进树里，重建一次就是几秒卡顿。）
        if (IsGeneratedPath(full)) {
            cont = dir.GetNext(&filename);
            continue;
        }

        if (wxDirExists(full)) {
            auto id = m_tree->AppendItem(
                parent,
                filename,
                0, 0,
                new FileTreeItemData(full)
            );
            m_tree->SetItemHasChildren(id, true);
            BuildTree(full, id);
        }
        else {
            m_tree->AppendItem(
                parent,
                filename,
                1, 1,
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
    const int changeType = evt.GetChangeType();

    // 防御性过滤（双保险）：即便将来有人又把 wxFSW_EVENT_ACCESS 加回订阅掩码，
    // 也绝不能因为"本面板自己遍历目录"而产生的事件去重建树，否则会退化成
    // "读目录 → 事件 → 重建 → 再读目录" 的 100% CPU 自激循环。
    if (changeType & wxFSW_EVENT_ACCESS) {
        return;
    }
    // 监视器自身的错误/告警事件不表示文件发生变化。
    if (changeType & (wxFSW_EVENT_WARNING | wxFSW_EVENT_ERROR)) {
        return;
    }
    if (IsGeneratedPath(evt.GetPath().GetFullPath()) ||
        IsGeneratedPath(evt.GetNewPath().GetFullPath())) {
        return;
    }
    ScheduleRefresh();
}

void ProjectTreePanel::ScheduleRefresh() {
    if (m_refreshPending) {
        return; // 已排队，去抖窗口内的其余事件直接合并进来
    }
    m_refreshPending = true;
    m_refreshTimer.StartOnce(kRefreshDebounceMs);
}

void ProjectTreePanel::OnRefreshTimer(wxTimerEvent&) {
    if (!m_refreshPending) {
        return;
    }
    m_refreshPending = false;
    RefreshTree();
}
