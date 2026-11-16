#pragma once
#include "VirtualSessionController.h"
#include "ui/UiTheme.h"
#include <wx/panel.h>
class wxStaticText;class wxButton;class wxTextCtrl;class wxListCtrl;class wxSpinCtrl;
namespace sigflow::virtual_lab {
class VirtualBoardPanel:public wxPanel {
public:
    VirtualBoardPanel(wxWindow*,std::shared_ptr<VirtualSessionController>);
    ~VirtualBoardPanel();
    void SetProjectProvider(std::function<eda::Json()> f){project_provider_=std::move(f);}
    void InvokeControl(const std::string&);
    void SetContact(int pad,bool pressed){Contact(pad,pressed);}
    std::string DetailText()const;
private:
    void Update();
    void Load();
    void Contact(int pad,bool pressed);
    void ReleaseContact();
    void ApplyTheme();
    std::shared_ptr<VirtualSessionController> controller_;
    wxPanel* drawing_=nullptr;wxStaticText* status_=nullptr;
    wxTextCtrl* detail_=nullptr;wxListCtrl* diagnostics_=nullptr;wxSpinCtrl* replay_count_=nullptr;
    std::function<eda::Json()> project_provider_;
    std::string diagnostic_signature_;
    std::map<std::string,wxButton*> controls_;
    size_t subscription_=0;
    int held_pad_=-1;
    UiTheme::Subscription theme_;
};
}
