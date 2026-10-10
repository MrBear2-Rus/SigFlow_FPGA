#include <wx/wx.h>
#include <wx/config.h>
#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/memconf.h>
#include <wx/stdpaths.h>
#include <clocale>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __linux__
#include <langinfo.h>
#endif
#include "ProjectStartWindow.h"
#include "MainFrame.h"

namespace {

#ifdef _WIN32
void MigrateRegistryHistory(wxConfigBase& config)
{
    bool migrated = false;
    if (config.Read("/Migration/RegistryHistoryV1", &migrated) && migrated) return;

    // The previous Windows wxConfig backend used this key.  Open it with
    // read-only access: wxRegConfig's constructor requests write access and
    // would reproduce the startup error on restricted accounts.
    HKEY legacy = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Sigflow\\Sigflow", 0,
                      KEY_QUERY_VALUE, &legacy) != ERROR_SUCCESS) {
        return;
    }

    std::vector<wxString> merged;
    const auto appendUnique = [&merged](const wxString& path) {
        if (path.empty()) return;
        for (const wxString& existing : merged) {
            if (existing.CmpNoCase(path) == 0) return;
        }
        if (merged.size() < 9) merged.push_back(path);
    };
    // Keep projects opened since the file backend was introduced first.
    for (int index = 1; index <= 9; ++index) {
        wxString value;
        if (config.Read(wxString::Format("/file%d", index), &value)) appendUnique(value);
    }
    const std::size_t currentCount = merged.size();
    bool foundOldHistory = false;
    for (int index = 1; index <= 9; ++index) {
        const wxString name = wxString::Format("file%d", index);
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(legacy, name.wc_str(), nullptr, &type, nullptr, &bytes) !=
                ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) ||
            bytes > 64 * 1024) {
            continue;
        }
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(legacy, name.wc_str(), nullptr, &type,
                             reinterpret_cast<BYTE*>(buffer.data()), &bytes) == ERROR_SUCCESS) {
            const wxString path(buffer.data());
            if (!path.empty()) {
                foundOldHistory = true;
                appendUnique(path);
            }
        }
    }
    RegCloseKey(legacy);

    if (!foundOldHistory) return;
    if (merged.size() > currentCount) {
        for (int index = 1; index <= 9; ++index) {
            config.Write(wxString::Format("/file%d", index),
                         index <= static_cast<int>(merged.size()) ? merged[index - 1] : wxString());
        }
    }
    config.Write("/Migration/RegistryHistoryV1", true);
    wxLogNull suppressExpectedFileErrors;
    config.Flush();
}
#endif

// wxConfig defaults to the registry on Windows.  Some managed Windows
// accounts cannot create HKCU\Software\Sigflow\Sigflow, which otherwise
// produces a wxWidgets error dialog as soon as recent projects are loaded.
// Keep all wxConfig::Get() callers on the same per-user file instead.
void InstallUserConfig()
{
#ifdef _WIN32
    const wxString directory = wxStandardPaths::Get().GetUserLocalDataDir();
    bool writable = !directory.empty();
    {
        // A failed permission probe must not create another startup dialog.
        wxLogNull suppressExpectedFileErrors;
        if (writable && !wxDirExists(directory)) {
            writable = wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }
        if (writable) {
            const wxString probePath = wxFileName(
                directory, wxString::Format(".config-probe-%lu.tmp",
                                            static_cast<unsigned long>(wxGetProcessId())))
                                           .GetFullPath();
            wxFile probe;
            writable = probe.Create(probePath, false);
            if (writable) {
                probe.Close();
                writable = wxRemoveFile(probePath);
            }
        }

        const wxString configPath = wxFileName(directory, "settings.ini").GetFullPath();
        if (writable && wxFileExists(configPath)) {
            wxFile existing(configPath, wxFile::read_write);
            writable = existing.IsOpened();
        }
        if (writable) {
            wxConfigBase::Set(new wxFileConfig(
                "Sigflow", "Sigflow", configPath, wxEmptyString, wxCONFIG_USE_LOCAL_FILE));
        }
    }
    if (!writable) {
        // The IDE remains usable when the profile is read-only.  Settings
        // edited in this session simply cannot be persisted.
        wxConfigBase::Set(new wxMemoryConfig());
    }
    MigrateRegistryHistory(*wxConfigBase::Get(false));
#endif
}

} // namespace

// Linux 下强制进程 locale 为 UTF-8（若当前不是）。
// 背景：wxString::ToStdString() 按当前 locale 转码，项目里历史代码大量用它
// 序列化路径。项目路径常含非 ASCII（如 ~/下载/），在 LANG=C 的终端启动
// （典型：openKylin 的 KARE 沙箱终端）时中文会静默转成空串：
//   * Job manifest 的 project_path/source_files 变空；
//   * 运行期拼出 "/.sigflow/fpga/runs/..."（从根开始），
//     报 "can't open file ... (error 2: No such file or directory)"。
// Windows 不受影响（走宽字符 API），故仅 Linux 生效。
static void EnsureUtf8LocaleOnLinux()
{
#ifdef __linux__
    setlocale(LC_ALL, "");
    const char* codeset = nl_langinfo(CODESET);
    if (codeset && std::strcmp(codeset, "UTF-8") == 0) return;
    // C.UTF-8 在 glibc ≥ 2.35 上保证存在；老系统退化为仅改 LC_CTYPE
    if (!setlocale(LC_ALL, "C.UTF-8")) {
        setlocale(LC_ALL, "C");
        setlocale(LC_CTYPE, "C.UTF-8");
    }
#else
    (void)0;
#endif
}

class MyApp : public wxApp
{
public:
    bool OnInit() override
    {
        EnsureUtf8LocaleOnLinux();
        // All recent-project and preference callers use this same identity.
        SetAppName("Sigflow");
        SetVendorName("Sigflow");
        InstallUserConfig();

        // 注意：wxApp 初始化时框架已自动调用 wxInitAllImageHandlers()，
        // GUI 应用中不要再手动调用，否则会产生 "Adding duplicate image handler" 警告。
        ProjectStartWindow startWindow;

        int ret = startWindow.ShowModal();

        if (ret == wxID_CANCEL)
        {
            return false;
        }

        wxString projectDir = startWindow.GetProjectDir();

        MainFrame* frame = new MainFrame();
        // 先确立顶层窗口：Show() 与后面的模态对话框都需要正确的属主窗口，
        // 否则对话框可能出现在主窗口之后/之外，或模态范围异常。
        SetTopWindow(frame);
        frame->Centre(wxBOTH);
        frame->Show(true);

        if (!projectDir.IsEmpty())
        {
            frame->SetProjectDir(projectDir);
        }
        else
        {
            // 关键：DoFileNew() 返回 false 只代表"本次新建没有完成"——
            // 用户在选择父目录/输入项目名时按下"取消"、对覆盖提示选"No"、
            // 取消保存确认等，都会走这条路径。
            // 绝不能因为用户的一次"取消"就让 OnInit() 返回 false：
            // 那会让 wxEntry 直接返回 -1，进程以退出码 255 静默消失
            //（没有任何窗口、没有任何提示，用户只会看到程序凭空关闭）。
            // 正确做法是留在"无项目的空工程"界面，用户可以再新建或打开项目。
            frame->DoFileNew();
        }

        return true;
    }
};

wxIMPLEMENT_APP(MyApp);

