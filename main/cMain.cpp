#include <wx/wx.h>
#include "ProjectStartWindow.h"
#include "MainFrame.h"

class MyApp : public wxApp
{
public:
    bool OnInit() override
    {
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

