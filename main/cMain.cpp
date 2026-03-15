#include <wx/wx.h>
#include "ProjectStartWindow.h"
#include "MainFrame.h"

class MyApp : public wxApp
{
public:
    bool OnInit() override
    {
        ProjectStartWindow startWindow;

        int ret = startWindow.ShowModal();

        if (ret == wxID_CANCEL)
        {
            return false;
        }

        wxString projectDir = startWindow.GetProjectDir();

        MainFrame* frame = new MainFrame();
        frame->Centre(wxBOTH);
        frame->Show(true);

        if (!projectDir.IsEmpty())
        {
            frame->SetProjectDir(projectDir);
        }
        else
        {
            frame->DoFileNew();
        }

        SetTopWindow(frame);

        return true;
    }
};

wxIMPLEMENT_APP(MyApp);
