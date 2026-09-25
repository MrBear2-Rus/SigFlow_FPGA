#include <wx/wx.h>
#include <clocale>
#include <cstring>
#ifdef __linux__
#include <langinfo.h>
#endif
#include "ProjectStartWindow.h"
#include "MainFrame.h"

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
        // 统一应用名/厂商名：wxConfig::Get()（默认 config）与
        // wxConfig("Sigflow") 必须指向同一个存储，否则最近项目历史会分裂成
        // 多份互不相通的配置（详见 MainMenuBar::LoadHistory 的说明）。
        SetAppName("Sigflow");
        SetVendorName("Sigflow");

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

