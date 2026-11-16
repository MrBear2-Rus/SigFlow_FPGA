#pragma once
#include "VirtualSessionController.h"
#include "ui/UiTheme.h"
#include <wx/panel.h>
class wxCheckBox;class wxStaticText;class wxTextCtrl;class wxChoice;class wxButton;
namespace sigflow::virtual_lab {
class FaultInjectionPanel:public wxPanel {
public:
    FaultInjectionPanel(wxWindow*,std::shared_ptr<VirtualSessionController>);
    ~FaultInjectionPanel();
    void SetOpen(const std::string& edge,bool open);
    std::string DetailText()const;
    void DriveInput(int pad,int state);
private:
    void Update();void ApplyTheme();
    std::shared_ptr<VirtualSessionController> controller_;
    wxCheckBox* open_;wxStaticText* status_;size_t subscription_;
    std::map<std::string,wxCheckBox*> edges_;
    wxTextCtrl* detail_;
    wxChoice *input_,*state_;wxButton* drive_;
    UiTheme::Subscription theme_;
};
}
