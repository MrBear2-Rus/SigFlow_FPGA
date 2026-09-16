# SigFlow 跨平台（Windows / Linux）运行时缺陷定位报告

> 对象：`CMake_SigFlow`（CMake + GCC + wxWidgets 3.2 的跨平台 EDA）
> 目标平台：Windows（MinGW-w64 GCC）与 Linux（GCC）
> 方法：**以源码精读为主**定位缺陷；对其中"确定性致命"的结论，用一次受控运行（`./build/sigflow`）+ `gdb` + `/proc` + inotify 取证复核
> 结论口径：每条都给出 `文件:行号` 与代码片段、Windows/Linux 各自的失败形态、以及具体改法

---

## 0. 摘要

### 0.1 一句话结论

Linux 上"构建成功但一跑就废"不是单个 bug，而是**三类系统性问题叠加**，外加两个"必死"的确定性故障：

| # | 系统性问题 | 本质 | 后果 |
|---|---|---|---|
| A | **路径分隔符** | 全仓约 50 处硬编码 `"\\"`，Linux 下 `\` 是**普通文件名字符**而非分隔符 | 目录/文件被创建到错误位置；读取路径永远匹配不上写入路径；FPGA/仿真整条链路断 |
| B | **字符编码 / 字节-字符混淆** | `ToStdString()`、`c_str()`、`length()` 三者在 Windows(ANSI/wchar) 与 Linux(UTF-8 locale) 语义不同 | tree-sitter 越界读、nlohmann 抛异常、Scintilla 偏移错乱、进程输出整块丢失 |
| C | **平台 API 假设** | POSIX `execv` 不走 PATH、inotify 全事件掩码、串口 by-id 取 basename；Windows 侧句柄继承泄漏、命名管道 OVERLAPPED 误用 | Linux 侧工具链/串口静默不可用；Windows 侧并发任务假死 |

两个确定性故障（已实测）：

| 故障 | 现象 | 实测结论 |
|---|---|---|
| **F1** | 打开项目后主窗口**整片全黑**、界面永不出现、进程不退 | UI 线程被 `wxFileSystemWatcher` **自激事件循环**占满，实测 **24 924 次/25 秒** `RefreshTree()`，CPU 100% |
| **F2** | 选"New Project"后**取消**任一对话框 | `OnInit` 返回 `false` → 进程**静默 exit(255)**，不留任何窗口与提示 |

### 0.2 缺陷分布

| 级别 | 数量 | 说明 |
|---|---|---|
| **P0 阻断** | 12 | 必然崩溃 / 必然死循环 / 必然退出 / 功能完全不可用 |
| **P1 严重** | 21 | 数据损坏、静默失败、并发竞态、Windows 或 Linux 单侧失效 |
| **P2 一般** | 27 | 局部正确性问题、资源泄漏、HiDPI、健壮性 |
| 合计 | **60** | 其中 **39 条只在单一平台成立**（正是"跨平台"痛点所在） |

### 0.3 必须先修的三件事

1. **P0-1** 树面板文件监视自激（否则 Linux 上只要打开项目，界面就永远全黑）。
2. **P0-2** 启动流程非致命化（否则用户一按"取消"程序就消失）。
3. **P0-3 / P0-4 / P0-5 / P0-6 / P0-7 / P0-8 / P0-9**（A、B、C 三类问题的代表项，一次机械修改即可让 Linux 的仿真与 FPGA 链路重新可用）。

---

## 1. 复现环境与证据采集方法

| 项目 | 值 |
|---|---|
| 系统 | Linux（KDE Plasma / KWin，Xwayland `:1`） |
| 编译器 | GCC（`/usr/bin/c++`），C++20 |
| wxWidgets | 系统 wx 3.2.11（`wxUSE_UNICODE_UTF8 = 0` → `wxString` 内部为 `wchar_t`） |
| 构建目录 | `build/`（无 `CMAKE_BUILD_TYPE`，即 Debug）、`build-linux/`（Release，系统 wx） |
| 被测产物 | `build/sigflow`（注意：**实际文件名是小写 `sigflow`**，仓库内不存在 `SigFlow`） |

采集手段：

```bash
# 1) 干净启动并抓完整 stderr/stdout
HOME=$PWD/.debug/home GDK_BACKEND=x11 ./build/sigflow > run.out 2>&1

# 2) UI 线程到底在"转"还是在"等"：采样 /proc
cat /proc/<pid>/stat          # 字段 3=state, 14/15=utime/stime
cat /proc/<pid>/syscall       # 阻塞在哪个系统调用
cat /proc/<pid>/task/*/stat   # 每线程状态

# 3) 拿栈：ptrace_scope=1，只能"先由 gdb 启动，再用 gdb Python 看门狗发 SIGINT"
gdb -q -batch -x gdb.cmds --args ./build/sigflow
#   python: threading.Thread(target=...os.kill(inferior_pid, SIGINT)...)
#   run / thread apply all bt 22

# 4) 断点计数（判断是"事件风暴"还是"单次慢操作"）
break ProjectTreePanel::RefreshTree
commands
silent
set $nrf = $nrf + 1
continue
end

# 5) 看监视器真实掩码（这是 F1 的定案证据）
cat /proc/<pid>/fdinfo/<inotify_fd>
```

---

## 2. 运行时实测事实

### 2.1 前置问题：`build/` 产物比源码旧 2.5 小时

- `build/sigflow` 时间戳 **16:31**，而 `main/MainFrame.cpp`、`main/cMain.cpp`、`main/ProjectStartWindow.cpp`、
  `main/platform/PlatformPaths.h` 的时间戳是 **18:52**。
- 直接运行旧二进制时，日志出现 `LoadCanvas: file not found!`：

```
MainFrame: JSON full path = [/home/nokna/Codes/C/CMake_SigFlow/canvas_elements.json]
LoadCanvas: file not found!
```

  而当前源码 `main/MainFrame.cpp:555` 已经改成用 `ExecutableDir()` 定位：

```cpp
wxString jsonPath = wxFileName(sigflow::platform::ExecutableDir(), "canvas_elements.json").GetFullPath();
```

- **重新构建后该问题消失**，路径变为 `.../build/canvas_elements.json`（正确）。
- 结论：**"构建成功"不等于"产物最新"**。`build/` 是 Debug 且与源码不同步，`build-linux/` 才是 Release 且较新。
  报告后续所有实测均基于重新构建后的 `build/sigflow`。

### 2.2 事故 F1：主窗口全黑 + UI 线程 100% CPU（根因：文件监视自激）

**现象**

截图（`spectacle` 抓屏）显示：窗口标题为 `SigFlow [no project]`，客户区**从上到下整片纯黑**，
没有菜单栏、没有工具栏、没有任何面板；进程还活着，但 `sigflow.log` 停在
`Opening project: /home/nokna/Codes/SigFlowWorkspace/Linux2.0`，窗口标题永远不刷新。
对照实验：同一沙箱里运行 `zenity --info` 能正常渲染 —— **排除是合成器/沙箱渲染问题，确认是应用自身**。

**证据 1：主线程在"跑"而不是"等"**

`/proc` 采样（每 0.7 s 一次，连续 28 s）：

```
 2.1 rc=None state=R cpu=0.62s d=0.55 thr=16 wchan=0 syscall=?
 2.8 rc=None state=R cpu=1.30s d=0.68 wchan=0 syscall=?
 ...
28.0 rc=None state=R cpu=26.16s d=0.69 wchan=0 syscall=?
```

`state=R`（Running）、`d≈0.69 s / 0.7 s`（**≈99% 满载**）、`wchan=0`（不在内核等待），
且从启动后约 2 s 起持续到采样结束 —— 纯用户态死循环，不是慢，是**不收敛**。

**证据 2：gdb 全线程栈（主线程）**

```
Thread 1 (LWP 31) "sigflow":
#0  getdents64 () at /usr/lib/libc.so.6
#1  readdir64 () at /usr/lib/libc.so.6
#2  ??? () at /usr/lib/libwx_baseu-3.2.so.0
#3  ProjectTreePanel::BuildTree(wxString const&, wxTreeItemId) ()
#4  ProjectTreePanel::BuildTree(wxString const&, wxTreeItemId) ()
#5  ProjectTreePanel::RefreshTree() ()
#6  ProjectTreePanel::OnFileSystemChanged(wxFileSystemWatcherEvent&) ()
...
#12 ??? () at /usr/lib/libwx_baseu-3.2.so.0
#13 ??? () at /usr/lib/libwx_gtk3u_core-3.2.so.0
#14 g_main_context_iteration ()
#16 gtk_main_iteration ()
#18 wxGUIEventLoop::DoYieldFor(long) ()
#19 wxEventLoopBase::YieldFor(long) ()
#20 wxGenericProgressDialog::DoBeforeUpdate(bool*) ()
#21 wxGenericProgressDialog::Update(int, wxString const&, bool*) ()
```

链条非常清楚：**`wxProgressDialog::Update()` 会泵 GTK 事件循环**，泵到的正是
`wxFileSystemWatcher` 事件 → `RefreshTree()` → `BuildTree()`（递归 `readdir`）。

**证据 3：断点计数 —— 是"事件风暴"不是"单次慢操作"**

`break ProjectTreePanel::OnFileSystemChanged` + `break ProjectTreePanel::RefreshTree`，运行 25 秒：

```
OnFileSystemChanged = 24924
RefreshTree         = 24925
```

**≈1 000 次/秒**。项目本身只有 6 个文件（`src/top.v` + `README.md` + `.sigflow/workspace` 镜像），
单次 `BuildTree` 是微秒级，所以只可能是**事件源源不断**。

**证据 4（定案）：inotify 监视掩码是 `fff`，包含 ACCESS/OPEN/CLOSE_NOWRITE**

```
fd 13 -> anon_inode:inotify
     inotify wd:3 ino:20304a mask:fff ignored_mask:0
     inotify wd:2 ino:203049 mask:fff ignored_mask:0
     inotify wd:1 ino:203048 mask:fff ignored_mask:0
```

- `mask:fff` = `IN_ALL_EVENTS`，其中包含 `IN_ACCESS(0x001)`、`IN_OPEN(0x020)`、`IN_CLOSE_NOWRITE(0x010)`。
- 代码侧完全对应：

```cpp
// main/ProjectTreePanel.cpp:50
watcher->Add(wxFileName(dir), wxFSW_EVENT_ALL);      // 默认还带 ACCESS
```

```cpp
// 3rd/wxWidgets-3.2.9/include/wx/fswatcher.h:45,51
wxFSW_EVENT_ACCESS = 0x10,
wxFSW_EVENT_ALL = wxFSW_EVENT_CREATE | wxFSW_EVENT_DELETE | ... | wxFSW_EVENT_ACCESS | wxFSW_EVENT_ATTRIB | ...
```

```cpp
// main/ProjectTreePanel.cpp:215-221  —— 只过滤"路径"，不过滤"事件类型"
void ProjectTreePanel::OnFileSystemChanged(wxFileSystemWatcherEvent& evt) {
    if (IsGeneratedPath(evt.GetPath().GetFullPath()) ||
        IsGeneratedPath(evt.GetNewPath().GetFullPath())) {
        return;
    }
    RefreshTree();          // ← 无论 CREATE / MODIFY / **ACCESS** 都全量重建
}
```

**根因链（自激闭环）**

```
RefreshTree() → BuildTree() → opendir/readdir/closedir（对被监视目录的只读访问）
        │                                   │
        │                                   ▼
        │                        inotify 产生 IN_OPEN / IN_ACCESS / IN_CLOSE_NOWRITE
        │                                   │
        │                                   ▼
        └────────── RefreshTree() ← OnFileSystemChanged()   ←─── wxFSW_EVENT_ACCESS
```

读目录本身触发了"目录被访问"事件，事件又去读目录。三个被监视目录（项目根、`src`、`lib`）
每轮至少产生 9 个事件，呈指数扩散，直到把内核队列（`max_queued_events = 16384`）打满，
此后稳定在"消费速度 ≈ 1000/s"，UI 线程 100% 被这个闭环吃掉 —— 所以 GTK 永远没机会派发
第一次 `expose`，窗口保持全黑。

**为什么 Windows 侧不明显（跨平台不对称的关键）**

`wxFSW_EVENT_ACCESS` 在 Windows 后端映射为 `FILE_NOTIFY_CHANGE_LAST_ACCESS`，
而 NTFS 自 Vista 起**默认关闭 last-access 更新**（`NtfsDisableLastAccessUpdate=1`），
`ReadDirectoryChangesW` 基本不会因为"读目录"而报事件 —— 于是 Windows 上这个闭环不成立，Linux 上必炸。
**这正是"Windows 能跑、Linux 一跑就死"的典型形态。**

**修法（三处，缺一不可）**

```cpp
// ① 只订阅真正关心的语义事件（主修）
//    位置：main/ProjectTreePanel.cpp:50
const int kWatchMask = wxFSW_EVENT_CREATE | wxFSW_EVENT_DELETE |
                       wxFSW_EVENT_MODIFY | wxFSW_EVENT_RENAME;   // 明确不含 ACCESS
watcher->Add(wxFileName(dir), kWatchMask);

// ② 兜底：即便将来又订阅了 ACCESS，也要按类型早退
//    位置：main/ProjectTreePanel.cpp:215
void ProjectTreePanel::OnFileSystemChanged(wxFileSystemWatcherEvent& evt) {
    if (evt.GetChangeType() & wxFSW_EVENT_ACCESS) return;          // 自身读目录引起，直接丢
    if (IsGeneratedPath(evt.GetPath().GetFullPath()) ||
        IsGeneratedPath(evt.GetNewPath().GetFullPath())) return;
    ScheduleRefresh();                                             // 不再同步全量重建
}

// ③ 合并/去抖 + 重入保护：1000 次/秒也要收敛成 1 次
//    位置：main/ProjectTreePanel.{h,cpp}（新增 wxTimer 成员）
void ProjectTreePanel::ScheduleRefresh() {
    if (m_refreshPending) return;
    m_refreshPending = true;
    m_refreshTimer.StartOnce(250);        // 250ms 内的所有事件合并成一次
}
void ProjectTreePanel::OnRefreshTimer(wxTimerEvent&) {
    m_refreshPending = false;
    RefreshTree();
}
```

补充（同一函数内的第二个隐患）：`BuildTree` 会递归进 `.sigflow/`，而 `AddWatchRecursive` 刻意跳过它，
两者不一致；且 `BuildTree` 对被生成的构建产物也建树。建议 `BuildTree` 用 `IsGeneratedPath()` 跳过，
并加 `wxDIR_NO_FOLLOW` + 深度上限（防符号链接成环导致栈溢出）。

```cpp
// main/ProjectTreePanel.cpp:159-193 现状：点目录跳过被注释掉了
//if (filename.StartsWith(".")) continue;      // ← 恢复，或改用 IsGeneratedPath(full)
```

---

### 2.3 事故 F2：取消"新建项目"对话框 → 进程静默退出（exit 255）

**现象**

`MainFrame` 已构造（stderr 有 `MainFrame: JSON full path = [...]`），随后进程在 ~20 s 退出，
**退出码 255**，无崩溃信号（不是 SIGSEGV/SIGABRT），无核心转储，窗口直接消失。

**根因（读代码即可确证）**

```cpp
// main/cMain.cpp:23-38
MainFrame* frame = new MainFrame();
frame->Centre(wxBOTH);
frame->Show(true);

if (!projectDir.IsEmpty()) {
    frame->SetProjectDir(projectDir);
} else {
    if (!frame->DoFileNew()) return false;   // ← wxApp::OnInit() 返回 false 会直接结束进程
}
SetTopWindow(frame);
```

`wxApp::OnInit()` 返回 `false` ⇒ 主循环不启动 ⇒ `wxEntry` 返回 `-1` ⇒ shell 看到 **255**。
而 `DoFileNew()` 在**一长串"用户取消/正常拒绝"路径上都返回 false**：

```cpp
// main/MainFrame.cpp:1477-1592（节选）
if (dirDlg.ShowModal()  != wxID_OK) return false;   // 1482 取消"选择父目录"
if (nameDlg.ShowModal() != wxID_OK) return false;   // 1487 取消"项目名"
if (projName.IsEmpty()) { ...; return false; }      // 1489 空名
if (res != wxYES) return false;                     // 1505 覆盖确认选"No"
if (res != wxYES) return false;                     // 1511 同名文件选"No"
if (!ConfirmCurrentWorkBeforeProjectSwitch()) return false;   // 1516 取消保存提示
if (!wxRemoveFile(projPath)) { ...; return false; } // 1518
if (!wxFileName::Mkdir(...)) { ...; return false; } // 1526
if (!WriteUtf8File(projFile, projectJson)) { ...; return false; } // 1560
```

实测日志佐证：`sigflow.log` 里**只有** `[INFO] Application started`（没有 `Opening project`），
说明走的是 `OnNewProject`（空目录）分支；配合 `GVFS-WARNING ... missing --filesystem=xdg-run/gvfsd privileges`
（GTK 文件选择器打开时的典型告警）与 `Theme parsing error: .../gtk-3.0/colors.css`，
可确认当时正停在 `wxDirDialog`，被取消后即 `return false` → 退出 255。

**影响**：Windows / Linux **完全一致**（不是跨平台差异，而是通用设计缺陷）。

**修法**

```cpp
// 方案 A（推荐）：把"取消"与"失败"分开，启动流程永不致命
bool MyApp::OnInit() override {
    ProjectStartWindow startWindow;
    int ret = startWindow.ShowModal();
    if (ret == wxID_CANCEL) return false;          // 只有"退出"才结束

    MainFrame* frame = new MainFrame();
    SetTopWindow(frame);                            // ← 移到 Show() 之前
    frame->Centre(wxBOTH);
    frame->Show(true);

    const wxString projectDir = startWindow.GetProjectDir();
    if (!projectDir.IsEmpty()) {
        frame->SetProjectDir(projectDir);
    } else {
        frame->DoFileNew();                         // 失败就停在空工程，不要 return false
    }
    return true;                                    // ← 无论如何都进入主循环
}
```

```cpp
// 方案 B（更彻底）：DoFileNew 用三态返回值区分 Cancel / Failed / Ok
enum class NewProjectResult { Ok, Cancelled, Failed };
NewProjectResult MainFrame::DoFileNew();
```

附带问题：`frame->Show(true)` 早于任何项目存在，而 `SetTopWindow(frame)` 却排在模态对话框**之后**，
导致对话框的属主窗口尚不是顶层窗口（Windows 上可能出现对话框跑到后面/模态范围异常）。

---

### 2.4 事故 F3：自带测试套件在 Linux 上 6/32 失败

```
$ cd build && ctest --output-on-failure
[FAIL] cmd echo exits 0
[FAIL] stdout captured
[FAIL] argv round-trip preserves spaced argument
[FAIL] raw command line passes through unescaped
[FAIL] timeout kills process tree
[FAIL] environment override reaches child process
...
6 TEST(S) FAILED
0% tests passed, 1 tests failed out of 1
```

**根因：测试本身是 Windows-only 硬编码**，且**没有任何 `#ifdef` 保护**：

```cpp
// tests/job_tests.cpp:42,51,61,70,85
echo.executable = "cmd.exe";
round.arguments = { "/d", "/s", "/c", "echo", "a b c" };
rawMode.executable = "cmd.exe /d /s /c echo raw-mode-ok";
slow.arguments = { "/d", "/s", "/c", "ping -n 5 127.0.0.1 >nul" };
request.arguments = { "/d", "/s", "/c", "echo %SIGFLOW_TEST_ENV%" };
```

```cpp
// tests/job_tests.cpp:209 —— 又是硬编码反斜杠
simRequest.sourceFiles = { project + "\\src.v" };
```

这意味着：**`ctest` 根本无法作为 Linux 构建门禁**，"构建成功"掩盖了 jobs 层在 Linux 上从未被验证的事实
（`main/jobs/PlatformProcess.cpp` 的 POSIX 分支注释自己也写着"当前仓库无 Linux 构建，尚未在真机编译验证"）。

**修法**：`#ifdef _WIN32` 分支保留 `cmd.exe` 用例，`#else` 用 `/bin/sh -c` / `sleep 5`；
路径用 `std::filesystem::path(project.ToStdString()) / "src.v"`。

---

### 2.5 其它日志与告警归类

| 日志 | 归类 | 说明 |
|---|---|---|
| `Debug: Adding duplicate image handler for 'PNG file'` … | **已修复** | `cMain.cpp` 已不再手动调用 `wxInitAllImageHandlers()`；旧 `errorlog.txt` 里的这条来自旧版本 |
| `GLib-GIO-CRITICAL: GFileInfo created without standard::size` | 环境/次要 | 来自 GTK 文件选择器与 GIO；沙箱文件系统（只读 `~/.local/share`）下更明显 |
| `Gtk-WARNING: Negative content width/height -4/-6 (allocation 10, extents 7x7) while allocating gadget (GtkButton)` | **真实缺陷**（P2） | 控件被分配到约 10px 高度，说明 AUI/工具栏布局在 Linux 上塌缩；与 `FromDIP` 缺失、`Maximize()` 早于 `Show()`（见 P2）相关 |
| `Gtk-WARNING: ... recently-used.xbel ... Read-only file system` | 环境 | `$HOME/.local/share` 只读所致，非应用缺陷 |
| `GVFS-WARNING: peer-to-peer connection failed ... missing --filesystem=xdg-run/gvfsd` | 环境 | 打开 GTK 文件选择器时的沙箱限制 |
| `dconf-CRITICAL: unable to create file '/run/user/1000/dconf/user'` | 环境 | 沙箱下 `/run/user` 只读 |
| `wxTreeCtrl ... lost focus even though it didn't have it` | 次要 | 旧日志中的 GTK 焦点告警 |
| `sigflow.log` 位置随 CWD 漂移 | **真实缺陷**（P2） | `ProjectStartWindow` 用相对路径 `"sigflow.log"`，从别处启动则日志面板空白、只读 CWD 下静默写失败 |

---

## 3. P0 —— 阻断级缺陷（12 条）

> 标注：`[实测]` = 本次运行/取证直接确认；`[精读]` = 源码层面确定。

### P0-1 [实测] 项目树文件监视自激，UI 线程永久 100% CPU、窗口全黑

- **位置**：`main/ProjectTreePanel.cpp:50`（`wxFSW_EVENT_ALL`）、`:215-221`（不过滤事件类型）、
  `:139-157`（`RefreshTree` 全量重建）、`:159-193`（`BuildTree` 递归且未跳过生成目录）
- **机理**：`RefreshTree → BuildTree → readdir` 触发 `IN_OPEN/IN_ACCESS/IN_CLOSE_NOWRITE`
  → `OnFileSystemChanged` → `RefreshTree` → …… 自激闭环（详见 §2.2）
- **影响**：**Linux 必现**（inotify 上报 ACCESS）；Windows 因 NTFS 默认关闭 last-access 更新而基本不触发
- **修法**：见 §2.2 的三处修改（收窄掩码 + 按类型早退 + 去抖合并）

### P0-2 [实测] 取消"新建项目"任一对话框 → 整个应用静默退出（exit 255）

- **位置**：`main/cMain.cpp:33`（`if (!frame->DoFileNew()) return false;`）+ `main/MainFrame.cpp:1482/1487/1489/1505/1511/1516/1518/1526/1560`
- **影响**：Windows / Linux 一致；用户按一次"取消"程序就消失，且无任何提示
- **修法**：见 §2.3（`OnInit` 恒返回 `true`；三态返回值区分取消与失败）

### P0-3 [精读] Arena 节点被 `delete`：非法释放 / double free（切换项目必崩）

```cpp
// main/SigTree.h:41-58 —— 节点全部来自 arena 的 placement new
template <typename T, typename... Args>
T* make(Args&&... args) {
    void* mem = allocate(sizeof(T), alignof(T));
    return new (mem) T(std::forward<Args>(args)...);
}
void reset() { blocks.clear(); cur = nullptr; remaining = 0; }
```

```cpp
// main/SigTree.cpp:879-887 —— 却对 arena 内存调用 operator delete
void SigTreeNode::ClearNode() {
    for (auto child : children) { if (child) { child->ClearNode(); delete child; } }
    children.clear();
}
```

```cpp
// main/SigTree.cpp:137-158 —— 先非法 delete，再 arena.reset() 二次释放
//  :137 void SigFlowTree::ClearTree() { ... ; :154 root->ClearNode(); ... ; :157 arena.reset(); }
    if (root) { root->ClearNode(); root = nullptr; }
    arena.reset();
}
void SigFlowTree::LoadProject(std::string projectPath) {
    if (root) { ClearTree(); }                       // ← 每次"换项目"都走这里
    this->root = arena.make<ProjectNode>(projectPath);
}
```

- **触发**：同一进程内**打开第二个项目**（`MainFrame::SetProjectDir` → `sigTree->LoadProject(...)`）。
- **后果**：对 `char[]` 块内部指针 `free()` → glibc `free(): invalid pointer` abort；命中块首则整块 1 MB
  提前释放、随后被 `arena.reset()` 再释放一次 → 堆破坏；析构函数还会在已释放内存上运行。
- **附带**：`Arena::reset()` 不调用析构，凡是被 arena 节点持有的 `std::string`/`std::vector`/`Statement`
  **全部泄漏**。
- **修法**：`ClearNode` 不得 `delete`。要么让 `Arena` 记录析构 thunk（`make` 时登记 `void(*)(void*)`，
  `reset` 逆序调用），要么树节点改用 `new`/`unique_ptr` 管理；`ClearNode` 只做 `children.clear()`。

### P0-4 [精读] tree-sitter 拿到的是"字符数"和"locale 转换缓冲"，越界读 + 解析截断

```cpp
// main/TreeSitterLinter.cpp:31（:49 TSTest、:60 GetStructNode 同）
TSTree* new_tree = ts_parser_parse_string(parser, nullptr, code.c_str(), code.length());
```

```cpp
// main/TreeSitterLinter.cpp:89-99 —— 用 tree-sitter 的字节偏移去索引同一个缓冲
uint32_t start = ts_node_start_byte(child);
uint32_t end   = ts_node_end_byte(child);
return std::string(source_buffer + start, end - start);
```

- **环境事实**：`/usr/lib/wx/include/gtk3-unicode-3.2/wx/setup.h:646 → wxUSE_UNICODE_UTF8 0`，
  即 `wxString` 内部为 `wchar_t`：`length()` 是**字符数**，`c_str()` 转 `const char*` 时走**当前 locale**。
- **失败形态**：
  - UTF-8 locale + 含中文：locale 转换产出 UTF-8 字节，字节数 > 字符数 → 只解析前 `length()` 字节，**文件尾部被静默截断**；
  - `LANG=C` / Windows ANSI 代码页无法表示该文本：转换缓冲为空或更短，而 `length()` 仍报字符数
    → tree-sitter **越界读堆内存**，`GetNodeName` 再按字节偏移二次越界。
- **活跃入口**：`main/VerilogManager.cpp:522` 每次编辑（300 ms 定时器）→ `VerilogStructuring.cpp:30-33` → `TSTest(wxString)`。
- **修法**：统一走字节。

```cpp
const wxScopedCharBuffer utf8 = code.ToUTF8();
TSTree* t = ts_parser_parse_string(parser, nullptr, utf8.data(), utf8.length());
TraverseNode(root_node, utf8.data(), res, id_count);   // 下游一律用 const char* + 字节长度
```

  并把 `Lint / TSTest / GetStructNode` 的形参从 `wxString` 改为 `const std::string&`（字节）。
  顺带：`GetStructNode`（`:60`）**泄漏 `new_tree`**（从未 `ts_tree_delete`），且未检查
  `ts_node_is_null(decl)`；`TreeSitterLinter::Lint` 里 `ts_tree_delete(new_tree)` 之前若 `new_tree == nullptr` 会崩。

### P0-5 [精读] POSIX 进程启动用 `execv`：不走 PATH，裸命令名必然失败

```cpp
// main/jobs/PlatformProcess.cpp:403（POSIX 分支）
execv(argv[0], argv.data());
_exit(127);
```

```cpp
// main/platform/SimToolchain.cpp:292（POSIX 分支）
request.executable = "g++";
```

- `execv` 要求可执行文件的**路径**，不搜索 `PATH`；只有 `execvp`/`posix_spawnp` 才搜索。
  于是子进程 `ENOENT → _exit(127)`。
- 父进程因为 `fork()` 成功而把 `result.started` 置 `true`（`:407`），调用方看到的是
  `"DLL编译失败 (错误码: 127)"` 且编译器输出为空 —— **Linux 上应用内仿真编译整条链路不可用**，
  而 Windows 的 `CreateProcessW` 会搜索 PATH，所以只在 Linux 炸。
- 同一函数还有：`quoteArguments == false` 的"原始命令行"模式在 POSIX **直接报错返回**
  （`"Raw command line is not supported on POSIX."`），任何依赖它的调用在 Linux 上不可用。
- **修法**：改 `execvp`（或 `posix_spawnp`）；若要修 #P1-6 的环境变量问题，则应先在父进程
  组装 `envp`，子进程仅调用 `execve`/`posix_spawnp`。

### P0-6 [精读] 读 `sigflow.project` 用反斜杠 → Linux 上所有仿真/FPGA 配置读取失败

```cpp
// main/MainFrame.cpp:2533-2539
const wxString projectDirectory = NormalizeProjectDirectoryPath(projectPath);
wxString configPath = projectDirectory + "\\sigflow.project";     // ← Linux 上 \ 是普通字符
if (!wxFileExists(configPath)) { SIGFLOW_LOG("sigflow.project not found\n"); return false; }
```

```cpp
// main/MainFrame.cpp:221
const wxString configPath = NormalizeProjectDirectoryPath(projectPath) + "\\sigflow.project";
```

- **写入侧却是对的**（`MainFrame.cpp:1559`/`1367`/`1154` 用 `wxFileName::GetPathSeparator()`），
  于是 Linux 上"写 `<proj>/sigflow.project`、读 `<proj>\sigflow.project`" → 永远读不到。
- **受害调用方**（全部硬失败）：`DoSimRun`(4565-4568)、`RunFpgaSynthesis`(3126)、`RunFpgaPack`(3961)、
  `DoFpgaPinBinding`(4314)、`RunTraceBridgeDebugBuild`(2802)、`GetTopModuleName`(4726)；
  以及 `LoadFpgaProjectOptions` 的 `RunFpgaSynthesis`(3146)/`RunFpgaRoute`(3615)/`RunFpgaPack`(3969)/
  `RunFpgaProgram`(4151)/`RunTraceBridgeDebugBuild`(2811)。
- **修法**：`JoinPath(projectDirectory, "sigflow.project")`（`JoinPath` 已在文件头引入）。

### P0-7 [精读] Yosys 预检硬编码 `yosys-abc.exe` → Linux 上综合永远启动不了

```cpp
// main/FpgaYosysRuntime.cpp:133
AddFileCheck(report, "yosys-abc", JoinPath(executableDirectory, "yosys-abc.exe"));
```

```cpp
// main/FpgaYosysRuntime.h:22
bool required = true;      // 新增检查默认 required
```

```cpp
// main/FpgaYosysRuntime.cpp:163-169 —— 任一 required 检查失败即判无效
for (const FpgaYosysRuntimeCheck& check : report.checks) {
    if (check.required && !check.passed) { report.valid = false; break; }
}
```

- Linux 发行版自带的是无扩展名的 `yosys-abc`，`.exe` 名不存在 → 预检恒失败 →
  `MainFrame.cpp:3242 if (!runtimeReport.valid)` → 直接中止综合。
- 同一函数 `GetShareDirectory()`（`:28-35`）解析 `exeDir/../share`，而 Linux 发行版是
  `/usr/share/yosys`，于是另外 15 项 `share:*` 检查也全灭。
- **修法**：`WithExecutableSuffix("yosys-abc")`；share 目录先试 `../share` 再试 `../share/yosys`；
  把 abc/share 检查改为平台条件性或非 required。

### P0-8 [精读] 引脚约束/CST 路径硬编码反斜杠 → Linux 上 PnR 永远找不到约束

```cpp
// main/fpga/FpgaPinBindingPanel.cpp:1078,1082
wxString FpgaPinBindingPanel::GetConstraintsPath() const {
    return m_projectPath + "\\.sigflow\\fpga\\constraints\\pin-bindings.json";
}
wxString FpgaPinBindingPanel::GetCstPath() const {
    return m_projectPath + "\\constraints\\" + m_topModule + ".cst";
}
```

- Linux 上整串是**一个文件名**，于是 CST 被写到项目的**兄弟位置**（`<parent>/proj\constraints\<top>.cst`），
  界面还提示 "CST generated successfully!"；而解析方 `CstValidator.cpp:42-53` 找的是
  `<proj>/constraints/`，`NextpnrExecutor.cpp:354-361` 于是报 `找不到 CST 约束文件` —— **布局布线必失败**。
- `pin-bindings.json` 同理：`Save`/`Load` 都错位，数据从不进项目。
- **修法**：两处都改 `sigflow::platform::JoinPath(...)`（参照 `FpgaSynthesisJob.cpp:263-274` 的正确写法）。

### P0-9 [精读] Linux 串口枚举取 basename → 枚举出来的口号无法打开

```cpp
// main/platform/SerialEnumerator.cpp:166-171
fs::path target = fs::read_symlink(entry.path(), readEc);
if (!readEc) {
    if (target.is_relative()) target = byId / target;
    info.name = target.lexically_normal().filename().string();   // /dev/ttyUSB0 → "ttyUSB0"
}
```

```cpp
// main/platform/SerialPort.cpp:296
fd = ::open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);   // 相对名 → ENOENT
```

- udev 对几乎每个 USB 串口都会建 `/dev/serial/by-id/*`，因此**这条是主分支**：
  Linux 上 USB 抓包/调试**整体不可用**；而解析同文件的 fallback 分支（`:189`）用的却是绝对路径，
  自相矛盾恰好证明这是笔误。Windows 侧（`COMx` + `\\.\` 前缀）正常。
- **修法**：`info.name = target.lexically_normal().string();`（保留绝对路径）。

### P0-10 [精读] `RemoveChildren()` 边遍历边 erase → 迭代器失效（每次克隆都触发）

```cpp
// main/SigTree.cpp:1183-1194
void SigTreeNode::RemoveChildren() {
    for (auto* cld : children) { RemoveChild(cld); }   // ← range-for 期间 erase
}
void SigTreeNode::RemoveChild(SigTreeNode* child) {
    auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end()) return;
    children.erase(it);
}
```

- `RemoveChildren()` 被**每个** `Clone()` 实现调用（`SigTree.h:178/200/238/313/340/359/399`），
  而 `CloneSubtreeToArena` 又在**每次** `AddChild`/`AddSignal`（即每次解析、每次画布/树编辑）中调用，
  即"高频路径上的 UB"：轻则子节点丢失/重复，重则崩溃。
- **修法**：`children.clear();`（克隆本来就只是要丢弃副本的子列表），或改为索引循环并在删除后不前进。

### P0-11 [精读] 多处未初始化成员被当指针/枚举/布尔读取

| 位置 | 声明 | 被读的位置 | 后果 |
|---|---|---|---|
| `main/CanvasElement.h:167` | `SecondNode* self;`（`SecondElement() = default`） | `CanvasElement.cpp:326`（`self == nullptr ? ... : self->identifier`） | 连 `== nullptr` 都是 UB；非空垃圾值 → 崩 |
| `main/CanvasElement.h:129` | `TopModuleBox::self` 同上 | 同上 | 同上 |
| `main/SigTree.h:325` | `TopNode* Definition;` | `CanvasPanel.cpp:1598`（`if (mn->Definition)`） | 不确定值当指针解引用 |
| `main/Wire.h:85` | `LogicSignal status;`（`Wire() = default`） | `Wire.cpp:37`（`status == LogicSignal::ZERO`） | 随机颜色/UB |
| `main/SigTree.h:106-110` | `PortDirection direction;` | `SigTree.cpp:1053`（`p.direction == PortDirection::In`） | assign 的端口分类随机，`out_ports` 可能为空 |
| `main/SigTree.h:552` | `AlwaysStatement() = default;` 未初始化 `is_blocking`/`delay` | `SigTree.cpp:1898-1904` | 生成 Verilog 读到垃圾 |

- **修法**：全部给类内初始化器（`= nullptr` / `= LogicSignal::ZERO` / `= PortDirection::InOut` / `= false` / `= 0`）。

### P0-12 [精读] Windows：可继承管道写端泄漏进并发子进程 → 任务假死

```cpp
// main/jobs/PlatformProcess.cpp:207-241
attributes.bInheritHandle = TRUE;                       // 读写两端都继承
if (!CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0) || ...)
SetHandleInformation(stdoutRead,  HANDLE_FLAG_INHERIT, 0);   // 只清读端
SetHandleInformation(stderrRead,  HANDLE_FLAG_INHERIT, 0);
...
CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, ...);   // 继承全部可继承句柄
```

- 任务 A 与任务 B 并发时（不同 Job 类型可并行：`JobRunner.h:54`，Yosys/nextpnr 各有 worker），
  B 的子进程继承了 A 的 `stdoutWrite`；A 的工具退出后，A 的读端**永远等不到 EOF**，
  于是 `stdoutPump.join()`/`stderrPump.join()`（`:295-296`）一直阻塞到 B 结束 —— 表现为
  "A 任务卡在 Running、Cancel 也杀不掉"。
- **修法（二选一）**：
  - 用 `STARTUPINFOEX` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 只传两个写端（推荐）；
  - 或 `bInheritHandle = FALSE` 建管道，再在 `CreateProcessW` 前后加"进程创建互斥锁"临时置位。

---

## 4. P1 —— 严重缺陷（21 条）

### 4.1 路径类（Linux 静默错位 / 功能失效）

| 编号 | 位置 | 问题 | 影响 |
|---|---|---|---|
| P1-1 | `MainFrame.cpp:3638` `exeDir + "\\..\\share"` | nextpnr share 目录拼错 | Linux 上 `ValidateNextpnrRuntime` 必失败 → 布局布线中止 |
| P1-2 | `MainFrame.cpp:395-399`（`FindTraceBridgeDebugRtl`）`+ "\\rtl\\debug"` | 调试 RTL 发现失败 | Linux 上 TraceBridge 调试构建不可用 |
| P1-3 | `MainFrame.cpp:3116/3117/3203/3274/3380-3387/3459-3466/3472-3473/3482/3507/3589-3590/3606/3760/3859/3872/3875/3878/3980/4388-4389/1227/1361/2846/2920/2930` 等 | 约 50 处 `"\\"` 拼接 | Linux 上 `yosys/nextpnr` 工作目录、日志、报告写成 `proj\yosys` 之类的**兄弟目录/单文件名** |
| P1-4 | `MainFrame.cpp:4388-4389`（`DoSimCompile` 兜底扫描 `src`/`lib`） | 反斜杠目录名 | Linux 上恒报 `项目中没有找到 Verilog 文件`，仿真无法启动 |
| P1-5 | `FpgaPinBindingPanel.cpp:1078/1082`、`FpgaConstraint` 相关 | 见 P0-8 | — |
| P1-6 | `main/debug/DebugOverlayBuilder.cpp:462-476,491` `sessionDir + "\\overlay"` / `"\\scripts\\run_yosys.ys"` | 反斜杠 | Linux 上创建出名字带 `\` 的目录，Yosys/nextpnr 无法解析脚本内路径 |
| P1-7 | `main/debug/DebugAcquisition.cpp:390-391` `outDir + "\\capture.raw"` / `"\\capture.vcd"` | 反斜杠 | Linux 上采集文件落到 `artifacts` 的**兄弟**位置，后续 `LoadSessionVcd`/导出 ZIP 找不到 |
| P1-8 | `main/debug/TraceBridgeWindow.cpp:2374-2376` `destination.Replace("/", "\\")` | 主动把 `/` 换成 `\` | Linux 上 ZIP 会话导入**必失败**（路径被揉成单文件名） |
| P1-9 | `main/fpga/NextpnrJob.cpp:215-221,318`、`FpgaSynthesisJobsPanel.cpp:268/321/322/400/408`、`NextpnrJobsPanel.cpp:266/309/310/376/397` | 反斜杠 | 日志/报告落到任务根目录，`GetSynthesisArtifacts` 永远列不到 |
| P1-10 | `FpgaToolWindow.cpp:428` `m_projectPath + "\\yosys"` | 反斜杠 | 与 `MainFrame` 读写"恰好一致"，但产生的是**兄弟目录** `proj\yosys` |

> **一处机械修改可同时消除 P0-6/P0-8/P1-1/P1-2/P1-3/P1-4/P1-9/P1-10**：
> 全仓禁止裸 `"\\"` 拼接路径，统一用 `sigflow::platform::JoinPath()` / `wxFileName`。
> `main/platform/PlatformPaths.h` 已经提供了正确工具，且 `main/fpga/FpgaYosysScriptGenerator.cpp:44-61`
> （`NormalizeYosysPath`：`MakeAbsolute + Normalize + Replace("\\","/")`）就是本仓的**正确范例**。

### 4.2 编码 / 字节-字符混淆类

| 编号 | 位置 | 问题 | Windows | Linux |
|---|---|---|---|---|
| P1-11 | `MainFrame.cpp:787/1231/1266/1276/1365/1398/1408/2342/2670` | 路径与源码内容用 `ToStdString()`（locale/ANSI）交给 tree-sitter 与 `SigFlowTree` | 含中文的目录/注释被转成 ANSI，越界字符丢失 → 解析与查找错 | UTF-8 locale 下侥幸正确 —— 所以最容易漏测 |
| P1-12 | `VerilogManager.cpp:334` `LineFromPosition(pos + text.size())` | Scintilla 字节偏移 + `wxString` 字符数混用（编辑器是 `wxSTC_CP_UTF8`） | 行号/折叠标记错位 | 同 |
| P1-13 | `VerilogManager.cpp:103/113/163/257/360/378/522`，`VerilogStructuring.h:52-57` | `ToStdString()` 进 tree-sitter / 结构化缓冲 | 非 ASCII 下缓冲变空或变短 | `LANG=C` 下同样出错 |
| P1-14 | `CanvasPanel.cpp:457/464/465/489/542/548` | `ToStdString()` → nlohmann `dump()` | 中文名不是合法 UTF-8 → `type_error.316` **未捕获 → `std::terminate`**，且可能损坏 `.tmp` | `LANG=C` 下变空串（静默丢数据） |
| P1-15 | `SFNPropertyPanel.cpp:96/122/147/177/307/523` | 属性面板改名走 `ToStdString()` | 中文标识符损坏后进入 UTF-8 模型 | 正常 |
| P1-16 | `main/jobs/PlatformProcess.cpp:66` `wxString::FromUTF8(buffer, accepted)` | 严格 UTF-8 解码；4096 字节读可能把多字节字符劈开 | 中文 Windows 的 CP936/GBK 诊断**整块 4096 字节被丢弃**；UTF-8 边界劈开时同样丢整块 | UTF-8 边界劈开时丢整块 |
| P1-17 | `main/fpga/CstValidator.cpp:127-150` | `Read()` 返回值不查 + `FromUTF8` 严格 | GBK 的 CST → `content` 为空 → 行数 0 → **校验"通过"**；短读时后段 NUL 被跳过，约束被静默丢弃 | 同 |
| P1-18 | `FpgaSynthesisJobsPanel.cpp:308-312`、`NextpnrJobsPanel.cpp:296-300` | `SetStyling(line.length()+1)` 用字符数对字节 | 中文报告后续行着色错位 | 同 |
| P1-19 | `canvas/模型层` `CanvasModel.cpp:40/82/39/65/89` | `asString()` → `wxString` 走 locale；颜色字符串不校验；`fontSize` 缺省为 0 | 非法颜色进 pen/brush（GTK 告警/断言）、`wxFont(0)` 非法 | 同 |
| P1-20 | `AsyncAnalysisCenter.h:63` + `.cpp:47-48` | `wxString` 隐式转 `std::string`（locale） | 非 ASCII 路径/代码被改写 | 正常 |
| P1-21 | `SimulationEngine.cpp:384-388` | `objDir.ToStdString()` 建/删目录且忽略 `ec` | 非 ASCII 路径失效，且旧 `obj_dir` 残留与新版本 obj 混用 | 正常 |

### 4.3 并发 / 生命周期 / 资源类

| 编号 | 位置 | 问题 | 影响 |
|---|---|---|---|
| P1-22 | `PlatformProcess.cpp:389-393` | **fork 后**在子进程里调用 `setenv()` + `wxString::ToUTF8()`（都非 async-signal-safe，会 malloc/加锁） | 多线程下 fork 时若他线程持锁 → 子进程 exec 前死锁 → 父进程等到超时再 SIGKILL；**间歇性卡死** |
| P1-23 | `PlatformProcess.cpp:207-221/368-373/384-388` | 错误路径不关句柄/描述符 | 句柄与 fd 泄漏（第二根管道创建失败、第二根 `pipe()` 失败、`fork()` 失败） |
| P1-24 | `PlatformProcess.cpp:390/436-441/458` | 只有子进程 `setpgid(0,0)`，父进程直接 `killpg` | 竞态窗口内 `killpg` 失败（`ESRCH` 未检查）→ 取消/超时失效 |
| P1-25 | `PlatformProcess.cpp:321-322/410-411` | `SetNonBlocking` 返回值不检查 | 失败则 `read` 阻塞、`pump.join()` 永久挂起 |
| P1-26 | `JobService.cpp:86/486-497/525-547` | Cancel（UI 线程）与 Finish（worker）无锁读改写同一 manifest，且共用同一 `.tmp` | 丢更新：Cancelled 可能被覆盖成 Succeeded，或状态迁移凭空消失 |
| P1-27 | `JobRunner.h:24-32/54/71-72` | worker 闭包持有最后一个 `shared_ptr` → 在 worker 线程析构 → `m_thread.join()` 自 join；`noexcept` 析构中抛 `system_error` → `std::terminate` | 潜在；另 `~MainFrame` 无条件 join 全部句柄，退出时可阻塞到工具超时（300–600 s） |
| P1-28 | `main/trace/TraceQueryService.cpp:113/136` | 在 worker 线程直接调用用户回调且无 `try/catch` | 回调抛异常 → 逃出线程函数 → `std::terminate` |
| P1-29 | `AsyncAnalysisCenter.cpp:63-73` | 持锁 `wxMilliSleep(50)` 忙等（注释说要改条件变量但没改） | UI 线程 `PushTask` 最长被阻塞 50 ms；任务入队后最坏等 50 ms |
| P1-30 | `AsyncAnalysisCenter.cpp:85-86`、`AsyncAnalysisCenter.h:47`；`MainFrame.cpp:652/1030-1048` | 每任务 `ts_parser_new()` 无 `ts_parser_delete`；`AsyncAnalysisCenter` 线程 detached 且析构为空；`MainFrame` 从不 delete 它 | 原生内存无界增长 + 线程/对象泄漏，`m_parentHandler` 可能比目标活得久 |
| P1-31 | `main/debug/ReplayScenario.cpp:268-278` | 在 worker 线程用 `std::filesystem::current_path()` **改全进程 CWD** 后跑 `std::system` | 构建期间（可能数分钟）全进程相对路径全部错位；异常路径还会把 CWD 留在错误位置 |
| P1-32 | `main/wave/WaveformGLCanvas.cpp:79-95/109-114` | `EnsureContext` 首次之后直接返回，`OnPaint` 不再 `SetCurrent`；且 `SetCurrent` 返回值不检查 | 多视图（Compare）互相画进对方上下文 → 面板空白/花屏；失败时无上下文执行 GL 调用（UB） |
| P1-33 | `main/wave/WaveformView.cpp:73-79` | 延迟回调捕获裸 `this`（`wxTheApp->CallAfter([this]{ OnGlFailed(); })`） | 视图先销毁 → use-after-free（同仓 `MainFrame.cpp:4076` 已有 `wxWeakRef` 正确写法） |
| P1-34 | `main/wave/WaveformGLRenderer.cpp:25-31/44-53/77-82`；`WaveformGLCanvas.cpp:49-50` | 假设固定管线（`glMatrixMode/glOrtho/glBegin`），属性表传 `nullptr`，`Ready()` 仅凭 `glGenTextures != 0` | 在 core profile 上所有绘制非法 → 只 `glClear`，但"就绪"为真，软件回退永不触发（**静默什么都不画**） |
| P1-35 | `main/wave/WaveformView.cpp:53-55` + `TraceViewPanel.cpp:178` | 唯一构造点传 `forceSoftware=true` | GL 后端是**死代码**，上述 GL 缺陷永远不会被构建/运行发现 |
| P1-36 | `FpgaSynthesisJob.cpp:59-62` | 路径包含性检查**无条件** `MakeLower()` | Linux 大小写敏感，`/proj` 会"包含" `/PROJ/x.v`，安全校验被削弱（`JobService.cpp:73-77` 已经用 `#ifdef __WXMSW__` 正确保护） |
| P1-37 | `main/platform/LocalPipe.cpp:94-110` | 对带 `FILE_FLAG_OVERLAPPED` 的命名管道调用 `ConnectNamedPipe(hPipe, nullptr)`（MSDN 明确禁止），返回值丢弃；`Close()` 无取消路径直接 `join()` | Windows 上客户端 `ERROR_PIPE_BUSY` 重试 50 次后失败；无人连接时 `Close()`/析构可永久阻塞 |
| P1-38 | `main/platform/DynamicLibrary.cpp:23-24` + `PluginManager.cpp:12-17` + `MainFrame.cpp:787` | Windows 侧全程窄字符：`LoadLibraryA` + `path::string()` + `ToStdString()`（未设 locale，失败时返回**空串**） | `C:\Users\张三\...\plugins` → `folderPath` 变 `""` → `fs::absolute("")` = **CWD** → 真实 plugins 目录从不扫描，反而去加载 CWD 里的任意 DLL；`GetLastError()` 也被丢弃 |
| P1-39 | `main/platform/SerialPort.cpp:319-329/402-415/449-453` | `IsSupportedBaud` 接受 110–3000000 任意值，但 `ToSpeedConstant` 只认固定表 | Linux 上设 250000 等波特率会**静默保持旧值**却返回成功（Windows 直接写 `dcb.BaudRate`，行为不一致） |
| P1-40 | `main/FpgaYosysExecutor.cpp:54`、`main/fpga/NextpnrExecutor.cpp:163` + `NextpnrExecutor.h:79`、`MainFrame.cpp:77` | `timeLimitSec` 默认 0（无超时），绕过了 `DefaultJobTimeoutSeconds` | 工具卡死则任务永远 `Running`，`WaitForSingleObject(INFINITE)` / POSIX 轮询永不返回 |
| P1-41 | `main/fpga/NextpnrExecutor.cpp:386` | `ValidateArtifact` 只判存在性 | 0 字节 `.pnr.json` 被判 `Succeeded` 并喂给 gowin_pack（`ToolJobs.cpp:623-624` 有正确的大小校验） |
| P1-42 | `main/trace/VcdLazyTraceSource.cpp:335-352`（配合 `:253-263`） | 从"最近的索引采样点"开始向后扫描，未先回放更早的值变化 | 稳定信号在 `t` 较大时被报成 `"x"`（t=0 恰好正确，所以初期渲染看不出） |
| P1-43 | `main/trace/TraceMemoryBudget.cpp:60-74` | `while (used > max && lru.size() > 1)` | 单条超限时永不淘汰 → 64 MB 预算形同虚设 |
| P1-44 | `main/debug/CaptureDecoder.cpp:84-86` | `(1u << width) - 1`，`width == 32` 时移位 UB（本机实测 mask = 0） | 32 位探针被解码成恒 0 |
| P1-45 | `main/debug/DebugProtocol.cpp:327-340` | `uint16_t pos` 在 `start+count > 65535` 时回绕，循环不收敛 | 公开 API 一旦按该参数调用 → 无限循环 + 无界内存 |
| P1-46 | `main/debug/DebugAcquisition.cpp:173-176` | minimal 协议把 `depth` 静默截断成 `uint16_t` | GUI 允许 10 000 000，实际设备只拿到低 16 位，且无报错 |
| P1-47 | `main/debug/DebugAcquisition.cpp:452-456` | 用中文字符串 `"超时"` 匹配错误 | 协议层报的是英文 `"response timeout"` → 超时被记成 `Failed`，分类失效 |

### 4.4 启动/UI 行为类

| 编号 | 位置 | 问题 | 影响 |
|---|---|---|---|
| P1-48 | `CanvasPanel.cpp:136` + `:801-821` | `OnPaint` → `LayoutScrollbars()` → `Refresh()` | 每次绘制又置脏 → **自我重绘死循环，长期 100% CPU**（与 P0-1 同类，静态确认） |
| P1-49 | `CanvasNoteBook.cpp:345/366` | 关标签页只 `wxPostEvent` 删除事件，随后 `evt.Skip()` 让 AUI 销毁页面，但 `cvses` 未同步移除 | 队列事件在 `SigFlowNodeDeleted` 里解引用已析构的 `CanvasPanel*` → **use-after-free** |
| P1-50 | `SigFlowTreePanel.cpp:334-341/357-359/526-528/541-543/637-639/723-725/738-740` | `[&, i]` 按引用捕获**循环内局部**控件指针 | 第二行复用同一栈槽 → 编辑第一行写到第二行（或读悬垂指针崩溃） |
| P1-51 | `CanvasEventHandler.cpp:855-872/876/901/913` | 拖拽路径索引 `m_compntIdx/m_textElemIdx/m_wireIdx` 无边界检查；早退导致 `m_movingWires` 比索引短 | 陈旧选择 → 越界读/崩；`CanvasPanel.h:144/168/169`、`CanvasPanel.cpp:1624` 同类 |
| P1-52 | `CanvasEventHandler.cpp:365-367/374-377/1097-1116` | 早退早于 `SetSelectState(IDLE)` | 工具状态机卡在 `DRAG_SELECT` → 选择"粘"在光标上，无按键也持续写元素坐标 |
| P1-53 | `CanvasEventHandler.cpp:511-512` | 对**已是客户区坐标**的 `evt.GetPosition()` 再做 `ScreenToClient` | Ctrl+滚轮缩放锚点偏移（离屏幕原点越远越明显） |
| P1-54 | `CanvasEventHandler.cpp:437-470` | Ctrl+Z/S/C/V/X 被吞掉：既不处理也不 `evt.Skip()` | 画布有焦点时 Ctrl+S **完全无效**（用户以为已保存）、撤销/粘贴全部失效 |
| P1-55 | `CanvasEventHandler.cpp:1128-1238` + `CanvasPanel.cpp:1623-1683/1023-1028` | 删除元素/导线/文本均不 `SetModified(true)` | 擦除后关闭标签页不提示保存 → **改动静默丢失**；且无撤销 |
| P1-56 | `CanvasEventHandler.cpp:926-935` | `pts[1]` / `pts[size()-2]` 无长度检查 | 少于 2 点的导线 → 越界写（`size()-2` 对空 vector 回绕成巨大值） |
| P1-57 | `CanvasPanel.cpp:975-983` + `SigTree.cpp:1282-1285` | `AddNewWire`/`AddWire` 未防 `tn == nullptr`、未查 `AddSignal` 返回 `nullptr` | 顶层被移除后画线 → 空指针解引用 |
| P1-58 | `SigFlowTreePanel.cpp:214/249-251` vs `:438-448` | "Add Always Block" 菜单可用，但创建对话框对 `Always` 返回 `nullptr` | 点了没反应（静默无效） |
| P1-59 | `CanvasPanel.cpp:1720-1726` | 既 `wxPostEvent` 又 `evt.Skip()` | `OnSFNodeActivated` 执行两次（`LoadNode` ×2、`AddSignalByName` ×2） |
| P1-60 | `MainMenuBar.cpp:124/150-159` 与 `ProjectStartWindow.cpp:34/197-199` | 最近项目历史用了**三套不同的 config 身份**：`*wxConfig::Get()`、`wxConfig("MyLogisim")`、`wxConfig("Sigflow")` | 启动窗口的"最近项目"、菜单"打开最近"、删除按钮操作的是不同存储：删掉的项目又出现，菜单列表永不持久化；MSW 上还会触发"config 已存在"断言 |

---

## 5. P2 —— 一般缺陷（27 条，摘要）

| # | 位置 | 问题 |
|---|---|---|
| P2-1 | `CMakeLists.txt:293-296`（及 `tests/CMakeLists.txt:72-75`） | 用 `file(GLOB)` 收集**尚未构建**的本地 wx DLL → 全新配置时列表为空 → Windows 上 exe 启动即缺 DLL 弹框，且普通 `cmake --build` 无法自愈（只有重新 configure 才行）。应改用 `$<TARGET_FILE:wx::core>` 等生成器表达式 |
| P2-2 | `MainFrame.cpp:1753-1886` | `DoFileOpen()` **整体被注释掉**，但文件菜单、工具栏打开按钮、FPGA 工具窗、波形导航、最近文件菜单都在调用它 → 完全静默无操作；最近文件菜单还会 `AddFileToHistory`，看起来像成功 |
| P2-3 | `MainFrame.cpp:1068-1103/1310-1328` | 每次打开项目都把**整个项目同步拷贝**到 `.sigflow/workspace`，而唯一消费者 `GetWorkspaceCopyPath`（`:2434-2450`）**零调用**；拷贝是"只增不删"的伪镜像，失败还会顺带跳过 `RefreshTitle()`；大项目直接冻结 UI |
| P2-4 | `MainFrame.cpp:2207/2210` | Save As 的标题与过滤器字面量里含 **U+FFFD 替换字符**（`"����Ϊ"`），任何平台都显示乱码 |
| P2-5 | `MainFrame.cpp:4420/4421/4732/4733`（及 4199/3208/3219/3246/3428） | 中文用**窄字符串**字面量（未 `wxT()`/`FromUTF8`）→ Windows 上乱码，Linux UTF-8 locale 正常 |
| P2-6 | `MainFrame.cpp:1486-1497` | 项目名只查非空，不校验是否为单一路径分量 → `../other` 可在所选目录之外创建项目（`wxPATH_MKDIR_FULL` 会补全中间层） |
| P2-7 | `MainFrame.cpp:4692-4709` | `wxGetTextFromUser` 的"取消"与"留空"都返回空串 → 取消直接进入"清理全部缓存"确认框 |
| P2-8 | `MainFrame.cpp:555-557` + `CanvasModel.cpp:16-22` | `canvas_elements.json` 加载失败只 `MyLog` 一行、返回值不检查 → 元件库静默为空（本次实测的 `LoadCanvas: file not found!` 即此类） |
| P2-9 | `MainFrame.cpp:229/1237/1369-1370` | `ReadAll(...)` 返回值不检查 → 与"项目静默为空"同症状 |
| P2-10 | `MainFrame.cpp:924-934` vs `cMain.cpp:24-25` | 构造函数里就 `Maximize(true)` 并据此算 AUI `BestSize`，但 `Show()` 在之后 → wxGTK 常忽略未 realize 的 maximize，Linux/Windows 布局基准不一致（也解释日志里的 `Negative content width/height`） |
| P2-11 | `MainFrame.cpp:4421` | 模态文本对话框父窗口传 `NULL`（`:4692` 的 `DoSimClean` 却正确传 `this`） |
| P2-12 | `MainFrame.cpp:1671-1685` + `:1687-1750` | `SaveToFile()` 忽略 `Open` 失败且无条件 `return true`；`GenerateFileContent()` 被掏空返回 `""` |
| P2-13 | `ProjectStartWindow.cpp:57-72/166-178` | 日志用相对路径 `"sigflow.log"` 且 I/O 结果全不检查 → 从别处启动日志面板空白，只读 CWD 下静默失败（违背本仓 `PlatformPaths.h` 自己的约定） |
| P2-14 | `main/ToolboxPanel.cpp:25/241/244/283-293` | 图标用 CWD 相对路径 `"res/svg/"`（其余代码已迁到 `ResourcePath`）→ 从桌面/快捷方式启动则全部图标消失；且用 `*wxWHITE_BRUSH` 清背景，把注释声称的"透明底"画成**不透明白块**，暗色主题下更难看出 |
| P2-15 | `main/CanvasEventHandler.cpp:40-44/52-56/318-322/723`、`ToolStateMachine.cpp:56-61` | 每次切换工具都新建 `wxImage`/`wxCursor`，`GetOptionInt` 的热点返回值被丢弃（两个 PNG 里也没有热点块）→ 光标热点在 (0,0)，导线/橡皮光标偏离约 12 px；且路由时每个鼠标移动事件都 new 一个 `wxCursor` |
| P2-16 | `main/CanvasNoteBook.cpp:252-264/291-298` | 每次 `AddCustomButton()` 都往 tab 控件**再绑一个** `wxEVT_PAINT` 且从不 `Unbind` → 切换 N 次文件后每次绘制执行 N 个 handler（每个还 `CallAfter`+`Raise`+`Refresh`） |
| P2-17 | `main/SigTree.cpp:1519-1524` + `CanvasPanel.cpp:1712-1718` + `Wire.cpp:118-121` | 重命名信号时 `RefreshSignal` 调用 `SetSelf(sn)`，而 `SetSelf` 在 self 相同时是**空操作** → 导线标签保留旧名直到重建画布；`sn == nullptr` 时还会解引用 |
| P2-18 | `main/SigFlowTreePanel.cpp` 多处、`SFNPropertyPanel.cpp:201/445` | 把节点内部 `std::string` 的**裸指针**存进控件 `SetClientData` | 节点被替换/重载后 handler 写悬垂指针 |
| P2-19 | `main/SigTree.cpp:489-499` | 门级解析只连 `in_conn[0]/[1]`，而 `GateInstNode` 固定只有 `in1/in2/out` → `and (o,a,b,c)` 的 `c` **静默丢失**，回写 Verilog 与源网表不一致 |
| P2-20 | `main/SigTree.cpp:1881-1887`（占位符生成见 `:194`） | 用朴素 `find/replace` 替换 `in1_1` 之类占位符 → `in1_10` 的前缀被误替换成 `<conn>0`，生成代码被破坏 |
| P2-21 | `main/SigTree.cpp:1189-1194/1199-1206` + `SigTree.h:265-271` | `RemoveChild` 不清 `child->parent`；`SignalNode` 又**自行声明**了一个 `parent` 和协变 `GetParent()`，基类/派生类各写一半 → `GetParent()` 时而悬垂、时而为 null（`SFNPropertyPanel.cpp:288`、`CanvasNoteBook.cpp:233` 直接解引用） |
| P2-22 | `main/SFNPropertyPanel.cpp:288-289/502-503` | `static_cast<TopNode*>(sn->GetParent())` 不做 null/类型检查 |
| P2-23 | `main/SigTree.cpp:1011-1030` | `for (auto p : sen->in_ports) { ... p.SetSignalTo(sn); }` **漏了 `&`** → 只改副本，`Port::signal` 永远是空/陈旧 |
| P2-24 | `main/SigTree.cpp:1782` | `ContinuousAssignNode::GetName()` 直接取 `out_ports[0]`（`ToVerilog()` 有防护，它没有）+ `Port::direction` 未初始化（见 P0-11）→ 越界读 |
| P2-25 | 全仓 | 大量硬编码像素/pt 尺寸（`SigTextEditor.cpp:43/51/180/204`、`Wire.h:55`、`Wire.cpp:38`、`CanvasElement.cpp:375/578/451/648`、`ToolboxPanel.cpp:71/225/249`、`ProjectTreePanel.cpp:87`、`CanvasNoteBook.cpp:275`、`CanvasPanel.cpp:803-805`） → HiDPI 下边距裁字、图标模糊、线宽随缩放变化；`"Segoe UI"` 在 Linux 静默回退 |
| P2-26 | `main/CanvasPanel.cpp:511-514/1035-1041/1496-1500` | `Read()/SetTopNode()/GetSecondElements()` 不防 `tn == nullptr`（同文件 `Save()` 429-435 有三层检查） |
| P2-27 | 其他次要项 | `ToolStateMachine.cpp:130-137` `IsIdle()` 漏 `m_eraserState`；`CanvasElement.cpp:168-261` switch 缺 `default`/多个类型无形状；`CanvasPanel.cpp:414-416` `HitWireSection` 无 `-1` 检查；`Wire.cpp:26-34` 竖直导线标签画在 (0,0)；`CanvasPanel.cpp:618/620` 每次移动两次全量扫导线、`:218` 深拷贝 Wire；`ToolboxPanel.cpp:613-623` 拖拽未 `evt.Allow()` 且无 drop target；`SigTextEditor.cpp:706-718` 空 vector 下 `size()-1` 下溢；`CanvasEventHandler.cpp:474-505` 缺 `break` 导致平移工具下 Delete 仍删元素；`VerilogStructuring.cpp:76-84` CRLF 敏感；`NextpnrLogParser.cpp:117/277` 分词丢空行导致行号漂移、版本正则脆弱；`JobRunner.h:93` 关停期输出丢失；`JobService.cpp:388` 任务 ID 秒级时间戳可跨实例碰撞；`JobService.cpp:448-456` 单个 manifest 读失败导致整个列表失败；`FpgaConstraint.cpp:403-408/30-38` 向量端口丢 `[0]`；`FpgaConstraint.cpp:668-680` 约束表非原子写；`FpgaPinBindingPanel.cpp:908`、`NextpnrReport.cpp:200` 写入返回值不检查却报成功 |

---

## 6. 专项清单

### 6.1 专项一：路径分隔符（一次性机械修复）

**正确工具已在仓库里**（`main/platform/PlatformPaths.h`）：`JoinPath`、`PathSeparator`、
`WithExecutableSuffix`、`SharedLibrarySuffix`、`PathListSeparator`、`ExecutableDir`、`ResourcePath`、`Utf8Path`。

**正确范例**（可照抄）：
- `main/fpga/FpgaYosysScriptGenerator.cpp:44-61`（`MakeAbsolute` + `Normalize` + `Replace("\\","/")` + 引号包裹）
- `main/jobs/JobService.cpp:354-368`（`GetPaths` 全程 `wxFileName::GetPathSeparator()`）
- `main/FpgaSynthesisJob.cpp:263-274`

**MainFrame.cpp 中确认"坏"的反斜杠位置**（供逐个替换）：
`221, 395, 397, 398, 399, 670, 758, 1227, 1361, 2534, 2846, 2920, 2930, 3116, 3117, 3203, 3231, 3274,
3380-3382, 3385-3387, 3396, 3397, 3459-3461, 3464-3466, 3472, 3473, 3482, 3483, 3507, 3589, 3590,
3606, 3638, 3760, 3859, 3872, 3875, 3878, 3980, 4388, 4389`

**其余文件**：`fpga/FpgaPinBindingPanel.cpp:1078,1082`、`fpga/NextpnrJob.cpp:215-221,318`、
`fpga/FpgaSynthesisJobsPanel.cpp:268,321,322,400,408`、`fpga/NextpnrJobsPanel.cpp:266,309,310,376,397`、
`fpga/FpgaToolWindow.cpp:428`、`debug/DebugOverlayBuilder.cpp:462-476,491`、`debug/DebugAcquisition.cpp:390-391`、
`debug/TraceBridgeWindow.cpp:2374-2376`、`tests/job_tests.cpp:209`。

**验证方式（不必跑 GUI）**：在 Linux 上对 `sigflow.project` 所在目录执行一次完整的
"打开项目 → 生成 CST → 综合"，检查是否**没有**出现名字里带 `\` 的目录或文件：

```bash
find ~/Codes/SigFlowWorkspace -name '*\\*' -print      # 修复后应为空
```

### 6.2 专项二：编码与字节/字符偏移（统一约定）

**问题根源**：`wxUSE_UNICODE_UTF8 = 0`（本机 wx 3.2.11，`/usr/lib/wx/include/gtk3-unicode-3.2/wx/setup.h:646`）。
`wxString` 内部 `wchar_t`，于是：

| 表达式 | 语义 | 跨平台差异 |
|---|---|---|
| `s.length()` / `s.size()` | **字符数** | 与字节数无关 |
| `(const char*)s.c_str()` | **当前 locale** 转换出的字节 | Windows=ANSI/CP936；Linux=UTF-8 locale 才对；`LANG=C` 时可能为 0 字节 |
| `s.ToStdString()` | 同上，走 `wxConvLibc` | 同上；转换失败**静默返回空串** |
| `s.ToUTF8()` | 明确 UTF-8 | 唯一可移植的选择 |
| `wxString::FromUTF8(...)` | 明确 UTF-8 解码，**非法输入返回空串** | 截断/无效字节会导致"整块丢失" |

**统一约定（建议写进 CONTRIBUTING）**：
1. 跨进程/跨库边界（tree-sitter、nlohmann、Scintilla、子进程管道、文件读写）一律 **UTF-8 字节**：
   用 `ToUTF8()` 取字节、`utf8.length()` 取长度、`FromUTF8()` 回程。
2. **字节偏移只与字节缓冲配用**：Scintilla/tree-sitter 的位置绝不能和 `wxString::length()` 混算。
3. 严格解码处必须提供回退与显式报错（`PlatformProcess`、`CstValidator` 当前都是静默丢整块）。
4. 用户可见字符串字面量统一 `wxT(...)` 或 `wxString::FromUTF8(...)`。

**重点位置**：`MainFrame.cpp:787/1231/1266/1276/1365/1398/1408/2342/2670`、
`TreeSitterLinter.cpp:31/49/60/89-99`、`VerilogManager.cpp:103/113/163/257/334/360/378/522`、
`CanvasPanel.cpp:457/464/465/489/542/548`、`SFNPropertyPanel.cpp:96/122/147/177/307/523`、
`jobs/PlatformProcess.cpp:66`、`fpga/CstValidator.cpp:127-150`、`Simulation/SimulationEngine.cpp:384-388`。

### 6.3 专项三：线程与对象生命周期

| 类别 | 位置 | 要点 |
|---|---|---|
| fork 安全性 | `jobs/PlatformProcess.cpp:389-393` | fork 后只允许 async-signal-safe 调用；`setenv`/`ToUTF8` 都要搬到 fork 前 |
| 锁与忙等 | `AsyncAnalysisCenter.cpp:63-73`、`JobService.cpp:486-547` | 持锁 sleep；manifest 无锁读改写 |
| 后台线程碰 UI | `MainFrame.cpp` 多处 `wxTheApp->CallAfter`（正确）、`WaveformView.cpp:73-79`（裸 `this`，错误） | 统一 `wxWeakRef` |
| 线程内异常 | `trace/TraceQueryService.cpp:113/136` | 回调必须 `try/catch`，否则 `std::terminate` |
| 全局 CWD | `debug/ReplayScenario.cpp:268-278` | 不得在 worker 线程改全进程 CWD |
| 对象所有权 | `SigTree` Arena（P0-3）、`AsyncAnalysisCenter`（P1-30）、`CanvasNoteBook::cvses`（P1-49）、`JobRunner.h`（P1-27） | 统一 `unique_ptr`/`wxWeakRef`/稳定 ID，禁用裸指针跨异步边界 |

---

## 7. 建议的修复路线图

> 每一批都**可独立编译、独立验证**，且尽量以"不跑 GUI 也能验证"的方式设计。

| 批次 | 内容 | 验证方式 |
|---|---|---|
| **第 1 批（止血，~半天）** | P0-1 监视自激（3 处）<br>P0-2 启动非致命化 | ① 打开项目后窗口在 1 s 内正常绘出、`top` 中 UI 线程 CPU < 5%<br>② 点 New Project → 取消 → 应用仍在 |
| **第 2 批（一键修路径）** | §6.1 全量替换 `"\\"` → `JoinPath` | `find ... -name '*\\*'` 为空；`LoadProjectConfig` 能读到 manifest |
| **第 3 批（一键修编码）** | §6.2 统一 UTF-8 字节边界（先修 P0-4、P1-11~P1-18） | 含中文注释的 `.v` 解析不截断；`luit -encoding GBK` 下不再崩 |
| **第 4 批（进程/任务）** | P0-5 `execvp`、P1-22 环境变量前置、P0-12 句柄白名单、P1-23 泄漏、P1-24/25 竞态、P1-26 锁 | `ctest` 全绿（先把测试 OS 分支化）；并发跑 2 个 job 均能正常结束 |
| **第 5 批（崩溃/内存）** | P0-3 Arena、P0-10 迭代器、P0-11 初始化、P1-49 UAF、P1-51/52/56/57、P1-48 重绘 | 连续打开 3 个不同项目不崩；ASan/Valgrind 干净 |
| **第 6 批（FPGA 链路）** | P0-7 yosys 预检、P0-8 CST 路径、P1-1 share 目录、P1-40 默认超时、P1-41 产物校验、P2-19/20 门级/占位符 | Linux 上完整跑通 `tang-nano-9k` 综合→布局→打包 |
| **第 7 批（工程化）** | P2-1 CMake DLL 拷贝、P1-60 config 身份统一、P2-13 日志路径、P2-25 HiDPI、其余 P2 | Windows 干净配置一次构建即可运行；HiDPI 150% 下无裁切 |

**建议同时补的护栏**：
1. `ctest` 必须能作为 Linux 门禁（先做 P0 的测试分支化）。
2. CI 增加"含中文路径 + 中文注释 + 中文模块名"的样例工程，专治 §6.2 这一类只在 Windows 暴露的问题。
3. CI 增加 `find -name '*\*'` 检查，禁止反斜杠路径回归。
4. `PlatformProcess` 增加"exec 失败回传通道"（`FD_CLOEXEC` errno 管道），否则 127 与真实编译器退出码无法区分。

---

## 8. 附录

### 8.1 关键证据速查

| 结论 | 证据 |
|---|---|
| UI 线程满载死循环 | `/proc/<pid>/stat`：`state=R`，`d≈0.69s/0.7s`，持续 28 s |
| 死循环在树重建 | `thread apply all bt`：`getdents64 → readdir64 → BuildTree → RefreshTree → OnFileSystemChanged → gtk_main_iteration → wxGenericProgressDialog::Update` |
| 是事件风暴不是慢操作 | 断点计数：`OnFileSystemChanged=24924`、`RefreshTree=24925`（25 s） |
| 事件来自"读目录自身" | `/proc/<pid>/fdinfo/<fd>`：`inotify wd:1/2/3 ... mask:fff`（含 `IN_ACCESS/IN_OPEN/IN_CLOSE_NOWRITE`） |
| 取消即退出 | `sigflow.log` 仅有 `Application started` + GTK 文件选择器告警；进程退出码 **255**（非信号） |
| 测试套件红 | `ctest`：`6 TEST(S) FAILED`，全部为 `cmd.exe`/`ping` Windows 用例 |
| 产物过期 | `build/sigflow` 16:31 vs 源码 18:52；旧产物报 `LoadCanvas: file not found!`，重建后消失 |

### 8.2 复现命令（最小集）

```bash
# 构建（增量）
cmake --build build -j"$(nproc)"

# 启动（含 X11 强制与日志落盘）
cd /path/to/CMake_SigFlow
HOME=$PWD/.debug/home GDK_BACKEND=x11 ./build/sigflow > .debug/run.out 2>&1 &

# 观察 UI 线程 CPU 与状态
pid=$(pgrep -n sigflow)
awk '{print "state="$3, "utime="$14, "stime="$15}' /proc/$pid/stat
ls -l /proc/$pid/fd | grep inotify            # 找到 inotify fd
cat /proc/$pid/fdinfo/<fd>                    # 看 watch mask

# 跑测试
cd build && ctest --output-on-failure
```

### 8.3 本报告的取证边界（诚实声明）

- **已由运行/工具直接确认**：F1（全黑+死循环+inotify 掩码+调用计数）、F2（exit 255）、F3（6/32 失败）、
  产物过期、`Arena`/`RemoveChildren`/tree-sitter 字节问题、`execv` 不走 PATH、`mask:fff` 与 `wxFSW_EVENT_ALL` 的对应。
- **纯源码精读确认（未逐条实机触发）**：其余 P0/P1/P2 条目。它们都给出了 `文件:行号` 与代码片段，
  修复前建议按第 7 节的分批验证方式逐条回归。
- 少数条目（如 inotify 掩码在 Windows 上的实际行为、`ConnectNamedPipe` 的具体 `GetLastError`、
  SetupAPI 的 `COM` 名解析）受限于本机无 Windows 环境，**未能实机验证**，报告中已就地标注。

---

# 9. 修复记录（第 1 轮）

> 状态：**已完成并验证**。`cmake --build build` 通过，`ctest` **32/32 全绿**（修复前 6 项失败）；
> 运行时复核：主窗口正常绘制、UI 线程不再满载、取消操作不再退出进程。

## 9.1 本轮已修复

| 编号 | 缺陷 | 修改位置 | 修法要点 |
|---|---|---|---|
| **P0-1** | 文件监视自激 → 窗口全黑 + UI 线程 100% CPU | `main/ProjectTreePanel.{h,cpp}` | ① 订阅掩码从 `wxFSW_EVENT_ALL` 收窄为 CREATE/DELETE/RENAME/MODIFY/ATTRIB/WARNING/ERROR（**去掉 ACCESS**）；② 事件处理里按类型早退（双保险）；③ 新增 250 ms 去抖定时器 `ScheduleRefresh()`，密集事件合并成一次重建；④ `BuildTree` 跳过 `.sigflow` 等生成目录并加 `wxDIR_NO_FOLLOW`（防符号链接成环） |
| **P0-2** | 取消"新建项目"→ 进程 exit(255) | `main/cMain.cpp` | `DoFileNew()` 的 `false` 不再向上传播为 `OnInit` 失败；同时把 `SetTopWindow(frame)` 提前到 `Show()` 之前 |
| **P0-3** | Arena 节点被 `delete` → 非法释放/double free | `main/SigTree.h`、`main/SigTree.cpp` | `Arena` 记录 `(指针, 析构 thunk)` 并在 `reset()` 里**逆序析构**；`ClearNode()` 只断链不再 `delete`；顺带修掉 Arena 不调用析构导致的对象内部资源泄漏 |
| **P0-4** | tree-sitter 收到"字符数"和 locale 缓冲 | `main/TreeSitterLinter.cpp` | 三处 `parse_string` 统一改为 `code.ToUTF8()` + `utf8.length()`，并把同一份字节缓冲传给 `TraverseNode/GetNodeName`；`LintFromPath` 改用 `FromUTF8`；`GetStructNode` 不再泄漏 TSTree；移除无副作用的 `DumpTree` 调用 |
| **P0-5** | POSIX `execv` 不走 PATH | `main/jobs/PlatformProcess.cpp` | `execv` → **`execvp`/`execvpe`**；测试用**裸命令名 `sh`** 作为回归用例 |
| **P0-6** | 读 `sigflow.project` 用反斜杠 | `main/MainFrame.cpp` | 全部改为 `JoinPath(...)`（本轮共机械替换 **82 处**） |
| **P0-7** | Yosys 预检硬编码 `yosys-abc.exe` | `main/FpgaYosysRuntime.cpp` | 改用 `WithExecutableSuffix("yosys-abc")`；`GetShareDirectory()` 用 `techmap.v` 探测 `share/` 与 `share/yosys/` 两种布局 |
| **P0-8** | CST/引脚约束路径硬编码反斜杠 | `main/fpga/FpgaPinBindingPanel.cpp` | 改用 `sigflow::platform::JoinPath(...)`，与 `CstValidator` 的查找路径一致 |
| **P0-9** | 串口枚举取 basename | `main/platform/SerialEnumerator.cpp` | 保留**绝对路径**；并补充 `ttyS/ttyAMA/ttyXRUSB/rfcomm` 前缀 |
| **P0-10** | `RemoveChildren()` 边遍历边 erase | `main/SigTree.cpp` | 直接 `children.clear()`（语义本就只是丢弃拷贝的子列表） |
| **P0-11** | 未初始化成员被当指针/枚举读 | `main/CanvasElement.h`、`main/SigTree.h`、`main/Wire.h` | `self`/`Definition` → `= nullptr`；`Port::direction` → `= PortDirection::InOut`；`Wire::status` → `= LogicSignal::ZERO`；`AlwaysStatement` 的 `is_blocking/delay` 加初始值 |
| **P0-12** | Windows 管道写端泄漏进并发子进程 | `main/jobs/PlatformProcess.cpp` | 管道建成**不可继承**，仅在全局互斥窗口内临时打开两个写端的继承位；补上 `hStdInput`（NUL） |
| **P1-8** | ZIP 导入把 `/` 换成 `\` | `main/debug/TraceBridgeWindow.cpp` | 删除该 `Replace` |
| **P1-1/2/3/4** | nextpnr share 目录、TraceBridge RTL、Yosys/nextpnr 工作目录、仿真 src/lib 扫描 | `main/MainFrame.cpp` | 随 82 处 `JoinPath` 一并修复 |
| **P1-9** | 作业日志/报告面板用反斜杠拼接 | `main/fpga/FpgaSynthesisJobsPanel.cpp`、`NextpnrJobsPanel.cpp`、`NextpnrJob.cpp`、`FpgaConstraint.cpp`、`FpgaToolWindow.cpp`、`main/debug/DebugOverlayBuilder.cpp` | 全部改 `JoinPath`（**与写入方一致**，否则读不到） |
| **P1-22/23/24/25** | fork 后 `setenv`/`ToUTF8`、fd 泄漏、`killpg` 竞态、`SetNonBlocking` 未检查 | `main/jobs/PlatformProcess.cpp` | 所有字符串与环境变量在 **fork 前**组装成 `argv/envp`；`FD_CLOEXEC`；父进程也 `setpgid`；`killpg` 失败退回 `kill`；错误路径补齐 `close()` |
| **P1-12 相关** | 标题永远停在 `[no project]` / 标题内容错乱 | `main/MainFrame.cpp` | 镜像失败不再跳过 `RefreshTitle()`；`RefreshTitle()` 不再对空 `m_currentFilePath` 调 `MakeRelativeTo` |
| **F3** | 测试套件 Windows-only，Linux 上 6/32 失败 | `tests/job_tests.cpp` | 按平台分支：Windows 用 `cmd.exe`/`ping`，POSIX 用 `sh`/`sleep`；`raw mode` 在 POSIX 断言"明确拒绝"；路径改用 `JoinPath` |

## 9.2 验证结果

```
$ cmake --build build -j
[100%] Built target sigflow                       # 无 error / warning

$ cd build && ctest --output-on-failure
1/1 Test #1: job_tests ..........   Passed  1.42 sec
100% tests passed out of 1
```

```
# 修复前（同一台机器，改动前实测）
OnFileSystemChanged = 24924 / 25s      → UI 线程 ~99% CPU，窗口全黑，标题不刷新
New Project → 取消文件夹对话框          → 进程 exit(255)，无任何提示
ctest                                  → 6 TEST(S) FAILED

# 修复后（同一脚本复测）
F1 VERDICT  PASS (no busy loop)        → 打开项目后主线程 6%~10%（含加载过程），窗口完整绘制
              标题 = "SigFlow [Linux2.0]"
F2 VERDICT  PASS (still alive)         → 取消后进程仍在
ctest                                  → ALL TESTS PASSED (32/32)
```

主窗口实测截图（`docs/` 同级 `.debug/err_dialog.png`）：菜单栏、工具栏、Project Manager 文件树、
画布、Side Panel、Console(Terminal/Waveform)、状态栏全部正常绘制 —— 修复前该区域是**整片纯黑**。

## 9.3 环境限制说明

- 测试机 `/home/nokna/Codes/SigFlowWorkspace` 在我的沙箱里是**只读**挂载，
  因此日志会出现 `MainFrame: project mirror failed; continuing without workspace copy.`。
  这是环境限制（并顺带验证了"镜像失败不再影响标题"这一修复），**不是新的缺陷**。
  该镜像本身没有消费者（报告 P2-3），建议后续直接移除。
- Windows 侧改动（`P0-12` 句柄继承、`P1-38` 窄字符路径等）**无法在本机实机验证**，
  已在代码中就地注明；建议在 MinGW 环境跑一次"两个不同 Job 类型并发"的用例。

## 9.4 尚未修复（下一轮建议顺序）

| 优先 | 条目 | 说明 |
|---|---|---|
| 1 | **P1-11 ~ P1-21**（编码/字节偏移） | `ToStdString()` 仍散落在 `MainFrame` / `VerilogManager` / `CanvasPanel` / `SFNPropertyPanel` / `SimulationEngine`；`CanvasPanel::Save()` 的 nlohmann `dump()` 仍未 `try/catch` |
| 2 | **P1-49 ~ P1-52**（画布 use-after-free / 越界 / 工具状态机） | 关标签页 UAF、拖拽索引越界、早退不重置状态 |
| 3 | **P1-26 / P1-27 / P1-28 / P1-29 / P1-30**（并发与生命周期） | manifest 丢更新、`RunJobHandle` 自 join、回调异常 `std::terminate`、`AsyncAnalysisCenter` 忙等与泄漏 |
| 4 | **P1-40 / P1-41**（FPGA 任务超时与产物校验） | `timeLimitSec` 默认 0、`ValidateArtifact` 只判存在 |
| 5 | **P1-31 ~ P1-35**（GL/波形） | GL 上下文未重新绑定、后端是死代码 |
| 6 | **P1-36 ~ P1-39**（大小写、命名管道、插件路径、串口波特率） | 单平台失效项 |
| 7 | **P1-48**（`CanvasPanel::OnPaint` → `Refresh()` 自激） | 与 P0-1 同类，静态确认，需实测确认 |
| 8 | **P2 全部（27 条）** | 含 `CMakeLists.txt` 的 wx DLL 拷贝、config 身份统一、HiDPI、启动日志路径等 |

> 第 7 节的分批路线图仍然有效；本轮完成的是**第 1~5 批中的 P0 部分 + 路径专项 + 进程专项**。

---

# 10. 修复记录（第 2 轮）

> 状态：**已完成并验证**。`cmake --build build` 通过；`ctest` **37/37 全绿**。
> 本轮全部为"读代码 + 无 GUI 测试"驱动，未对桌面发送任何合成输入。

## 10.1 本轮已修复

| 编号 | 缺陷 | 修改位置 | 修法要点 |
|---|---|---|---|
| **P1-48** | `OnPaint → LayoutScrollbars → Refresh()` 自激重绘（长期占满一核） | `CanvasPanel.cpp` | `LayoutScrollbars()` 先比较几何，**只在真的变化时** `Refresh(false)`；`OnPaint` 里几何已是最新 → 不再自激。顺带：滚动条宽度改用系统度量（HiDPI），`SetThumbSize` 钳到 ≥1（GTK 负数会断言） |
| **P1-49** | 关标签页后 `cvses` 留悬垂 `CanvasPanel*`（use-after-free） | `CanvasNoteBook.cpp` | `evt.Skip()`（默认处理会 delete 页面）**之前**先把 panel 从 `cvses` 摘除；补 `panel`/`tn` 空指针保护 |
| **P1-50** | `[&, i]` 按引用捕获**循环内局部**控件指针 | `SigFlowTreePanel.cpp` | 9 处 lambda 改为 `[&, i, <控件>]` 按值捕获（原先第二行会复用同一栈槽 → 编辑第一行写到第二行/读悬垂指针） |
| **P1-51** | 拖拽路径索引越界（OOB 写） | `CanvasEventHandler.cpp`、`CanvasPanel.h` | 三个"索引→位置/锚点"向量补齐越界检查与**占位**，保持与 `m_compntIdx` 一一对应；`StartElementDragging` 无论是否越界都 push；`SecondSetPos`/`TextSetPos`/`DeleteTextElement`/`WireGenerateCells`/`UpdateWire` 内联 setter 全部加边界检查 |
| **P1-52** | 提前 return 让工具状态机卡在 `DRAG_SELECT`（选择"粘"在光标上） | `CanvasEventHandler.cpp` | DRAG_SELECT 分支加 RAII 守卫，任何退出路径都复位 `IDLE`；另一处 `size()==1` 的越界 return 改为不提前返回，保证走到末尾的 `SetSelectState(IDLE)` 与 `evt.Skip()` |
| **P1-56** | 导线端点编辑 `pts[1]` / `pts[size()-2]` 越界 | `CanvasEventHandler.cpp` | `pts.size() < 2` 直接跳过（空 vector 的 `size()-2` 会回绕成巨大值） |
| **P1-28** | worker 线程里的用户回调无 `try/catch` → `std::terminate` | `trace/TraceQueryService.cpp` | `progress` / `completed` 两处回调就地兜住异常（对齐 `DebugAcquisition::FireProgress`） |
| **P1-40** | 执行器 `timeLimitSec` 默认 0 = **无超时**，工具卡死任务永不结束 | `FpgaYosysExecutor.cpp`、`fpga/NextpnrExecutor.cpp` | `<= 0` 时回落到 `kDefaultToolTimeoutSec = 600`（0 在平台层等价于 Windows `INFINITE` / POSIX 无超时轮询） |
| **P1-41** | `ValidateArtifact` 只判存在性，0 字节产物被当成功 | `fpga/NextpnrExecutor.cpp` | 额外要求文件非空，避免把截断的 `.pnr.json` 喂给 `gowin_pack` |
| **P1-36** | 工程目录包含性检查**无条件** `MakeLower()` | `FpgaSynthesisJob.cpp` | 大小写折叠仅在 `__WXMSW__` 下进行（Linux 大小写敏感，否则 `/proj` 会"包含" `/PROJ/evil.v`） |
| **P1-26** | manifest "读-改-写"丢更新；并发写入共用同一个 `.tmp` | `jobs/JobService.cpp` | 新增 `ManifestMutex()`（**recursive**，因为 `Start()` 会连续调用多次 `Transition()`）并锁住 `Create/Transition/Start/Cancel` 四个入口；临时文件名改为 `path.tmp.<pid>.<seq>` 唯一化 |
| **P1-27** | `JobRunHandle` 可能在 worker 线程上析构 → 自 `join` → `std::terminate` | `jobs/JobRunner.h` | 线程闭包**不再捕获 handle**，只捕获 `shared_ptr<atomic<bool>>` 完成标志；`Join()` 增加 `get_id() != this_thread` 守卫 |
| **P2-2** | `DoFileOpen()` 整个函数体被注释掉 → 文件菜单/工具栏/最近文件"打开"全是静默空操作 | `MainFrame.cpp` | 实现为：无路径时弹文件对话框，然后复用 `OnOpenFileFromTree` 的完整流程（保存画布 → 切编辑器/树/画布 → 刷新标题） |
| **P2-14** | 工具箱图标用 CWD 相对路径 + 白底不透明 | `ToolboxPanel.cpp` | 改用 `ResourcePath()`（相对 exe 目录）；图标改为在**全透明** `wxImage` 画布上叠加（原先 `*wxWHITE_BRUSH` 把注释声称的透明底画成白块） |
| **P2-23** | `JobOutputPump::Push` 在 `wxTheApp == nullptr` 时不清 `m_scheduled`，输出此后永不再刷 | `jobs/JobRunner.h` → 拆到新文件 `jobs/JobOutputPump.h` | 退出场景改为就地 `Flush()` 并复位标志；同时把 `JobOutputPump` 从 `JobRunner.h` 拆出，让 jobs 层头文件不再依赖 GUI，无 GUI 测试才能单独编译 |

## 10.2 新增回归用例

```
$ cd build && ./tests/job_tests
[PASS] utf8 producer exits 0
[PASS] utf8 output spanning read boundaries is not truncated or dropped
[PASS] invalid utf-8 output falls back instead of being dropped
[PASS] dropping the last JobRunHandle does not self-join / terminate
ALL TESTS PASSED            # 共 37 项
```

- 前两条对应 **P1-16**（跨 4096 读取边界的中文输出不丢块 / 非法 UTF-8 回退解码）。
- 第四条对应 **P1-27**：用例**故意丢弃最后一个 handle**；旧实现在 worker 线程析构 handle 会自连接并 `std::terminate`，进程根本走不到下一行 —— 因此"进程还活着"本身就是断言。

## 10.3 仍在待办（下一轮）

| 优先 | 条目 |
|---|---|
| 1 | **P1-11 尾项**：`CanvasModel`/`MainFrame` 中剩余的 `asString()`→`wxString` locale 转换点复查 |
| 2 | **P1-32 / P1-33 / P1-34 / P1-35**（波形 GL）：`OnPaint` 重新 `SetCurrent`、失败回调改 `wxWeakRef`、GL 上下文/属性表校验、后端是死代码 |
| 3 | **P1-47 / P1-43 / P1-44 / P1-45 / P1-46**（debug/trace 数值正确性）：中文子串判超时、内存预算、`1u<<32`、`uint16_t pos` 回绕、采集深度截断 |
| 4 | **P1-37 ~ P1-39**（平台单侧）：`LocalPipe` OVERLAPPED/句柄、插件窄字符路径、串口波特率 |
| 5 | **P2 其余**：`CMakeLists.txt` 的 wx DLL 拷贝（现为 configure 期 glob）、`wxConfig` 身份统一、启动日志路径、HiDPI、`NextpnrJob::GetPaths` 反斜杠等 |

---

# 11. 修复记录（第 3 轮）

> 状态：**已完成并验证**。`cmake --build build` 通过；`ctest` **37/37 全绿**。
> 本轮同样全程为"读代码 + 无 GUI 测试"，未对桌面发送任何合成输入。

## 11.1 本轮已修复

| 编号 | 缺陷 | 修改位置 | 修法要点 |
|---|---|---|---|
| **P1-44** | `(1u << width) - 1` 在 `width == 32` 时移位 UB | `debug/CaptureDecoder.cpp` | `width >= 32` 时直接用 `0xFFFFFFFFu`。本机 GCC 实测 `1u<<32` 得到 1 → mask=0，**32 位探针被解码成恒 0**（静默错误数据） |
| **P1-45** | `ReadCapture` 的 `pos` 是 `uint16_t`，`pos + chunk` 在 65535 处回绕 → **死循环 + 无界内存** | `debug/DebugProtocol.cpp` | 用 32 位游标推进（`end = uint32(start) + count`），只在调用 `ReadSamples` 时窄化 |
| **P1-46** | minimal 协议把 `depth` 静默窄化成 16 位 | `debug/DebugAcquisition.cpp` | `depth > 0xFFFF` 时显式拒绝并说明（契约允许到 10,000,000，旧实现 100000 → 34464 无任何提示） |
| **P1-47** | 用中文字符串 `"超时"` 判超时，而协议层报的是英文 `"response timeout"` | `debug/DebugAcquisition.cpp` | 同时匹配 `超时` / `timeout` / `Timeout` / `timed out` |
| **P1-43** | `TraceMemoryBudget` 的 `while (used > max && lru.size() > 1)` 在"单条就超限"时永不淘汰 | `trace/TraceMemoryBudget.cpp` | 条件改为 `!m_lru.empty()`，允许淘汰最后一条 |
| **P2-1** | CMake 用 `file(GLOB)` 收集**尚未构建**的本地 wx DLL → 全新配置时列表为空，一条拷贝命令都不注册 | `CMakeLists.txt` | 本地 wx 改用 `$<TARGET_FILE:wx::core>` 等生成器表达式逐 target 拷贝；预编译分支在列表为空时 `message(WARNING)`，不再静默漏拷 |
| **P2-13** | 启动日志用相对路径 `"sigflow.log"`，随 CWD 漂移；读/写结果全不检查 | `ProjectStartWindow.cpp` | 统一走 `ExecutableDir()/sigflow.log`；`IsOpened`/`ReadAll`/`Write` 全部检查并上报 |
| **P1-32** | GL 画布只在首次创建上下文时 `SetCurrent`，之后每次重绘都不再绑定 | `wave/WaveformGLCanvas.cpp` | `EnsureContext()` 每次都重新 `SetCurrent` 并**检查返回值**；析构时也先确认绑定成功再 `Shutdown()`（Compare 模式两个视图会互相抢上下文） |
| **P1-33** | GL 失败回调捕获裸 `this`，经 `CallAfter` 派发时视图可能已销毁 → use-after-free | `wave/WaveformView.cpp` | 改用 `wxWeakRef<WaveformView>`（与同仓 `MainFrame` 的写法一致） |
| **P1-38** | 插件加载在 Windows 上全程窄字符：`fs::path(std::string)` + `path::string()` + `LoadLibraryA` | `platform/PlatformPaths.h`、`platform/DynamicLibrary.cpp`、`PluginManager.cpp` | 新增 `PathToUtf8()` / `ExtensionEquals()`；`Utf8Path()` 构造路径、`PathToUtf8()` 取路径、`LoadLibraryW` + `GetLastError()` 上报；目录枚举改非抛出重载并在缺失/失败时记录日志 |
| **P1-39** | `IsSupportedBaud` 接受 110–3M 任意值，但 POSIX 只能通过 speed 表设置 | `platform/SerialPort.cpp` | POSIX 分支改为按 `ToSpeedConstant` 判定；`SetBaudRate` 不再把 `B0`（挂断）当哨兵。旧行为是 `SetBaudRate(250000)` 返回成功但端口仍停在 9600 |
| **P2-5** | 33 处含中文的**窄字符串字面量**被隐式转成 `wxString` | 6 个文件 | 在 `wxString` 上下文（`check.message`、`event.suggestion`、`wxMessageBox`、`wxTextEntryDialog`、`wxString::Printf`、`wxLogError`、`SetStatusText`）里统一包 `wxT(...)`。`std::string` 上下文的字面量本就按 UTF-8 约定，保持不动 |
| **P1-60** | 最近项目历史分散在**三套不同的 config 身份**里（`*wxConfig::Get()` / `wxConfig("MyLogisim")` / `wxConfig("Sigflow")`） | `cMain.cpp`、`MainMenuBar.cpp`、`ProjectStartWindow.cpp` | `OnInit` 里 `SetAppName/SetVendorName("Sigflow")`；三处一律改用 `wxConfig::Get()`。旧行为：启动窗口删掉的项目会在 File 菜单里复活，菜单列表永远保存不下来 |

## 11.2 本轮未做的判断说明

- 扫描出 225 处"含中文的窄字面量"，但只有 **33 处**真正落在 `wxString` 上下文里。
  其余是 `std::string`（`error = "..."`、`std::runtime_error("...")` 等），按本仓
  "std::string 一律视为 UTF-8 字节"的约定是**正确的**，不应盲改。
- `MainFrame.cpp` 里 `wxT("...") "窄字面量"` 的相邻拼接在 GCC 下由实现按源字符集
  (UTF-8) 正确转宽，属良性；本项目只支持 GCC(MinGW)，故未改动。

## 11.3 仍在待办（下一轮）

| 优先 | 条目 |
|---|---|
| 1 | **P1-37**（`LocalPipe` 对 OVERLAPPED 管道传 `nullptr` + `Close()` 无取消即 `join`）、**P1-42**（`VcdLazyTraceSource::ValueAt` 把稳定信号报成 `"x"`） |
| 2 | **P1-31**（`ReplayScenario` 在 worker 线程改全进程 CWD）、**P1-34/35**（GL core-profile / 后端是死代码） |
| 3 | **P2-3**（无消费者的工程镜像）、**P2-10**（`Maximize()` 早于 `Show()`）、**P2-25**（HiDPI 硬编码尺寸）、**P2-26 尾项** |
| 4 | 建议补的护栏：CI 增加 `find -name '*\*'` 与"中文路径/中文注释样例工程"两道检查 |
