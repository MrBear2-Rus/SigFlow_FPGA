#pragma once
#include "VirtualSessionController.h"
#include "ui/UiTheme.h"
#include <wx/panel.h>
#include <wx/scrolwin.h>
class wxCheckBox;class wxStaticText;class wxTextCtrl;class wxChoice;class wxButton;class wxListCtrl;
namespace sigflow::virtual_lab {
class FaultInjectionPanel:public wxScrolledWindow {
public:
    FaultInjectionPanel(wxWindow*,std::shared_ptr<VirtualSessionController>);
    ~FaultInjectionPanel();
    void SetOpen(const std::string& edge,bool open);
    std::string DetailText()const;
    void DriveInput(int pad,int state);
    void LoadProfile(const eda::Json&);
    void EnableFault(const std::string&,bool);
    void SetStuckMode(const std::string&,const std::string&);
    const eda::Json& FaultRows()const{return fault_rows_;}
private:
    void Update();void ApplyTheme();
    std::shared_ptr<VirtualSessionController> controller_;
    wxCheckBox* open_;wxStaticText* status_;size_t subscription_;
    std::map<std::string,wxCheckBox*> edges_;
    wxTextCtrl* detail_;
    wxChoice *input_,*state_;wxButton* drive_;
    wxPanel* phase1_=nullptr;wxListCtrl* rules_=nullptr;wxTextCtrl* editor_=nullptr;wxStaticText* profile_status_=nullptr;wxButton* apply_profile_=nullptr;wxChoice* stuck_mode_=nullptr;
    eda::Json profile_=eda::Json::object(),fault_rows_=eda::Json::array();std::string profile_error_,rules_signature_;
    void ApplyProfile();
    UiTheme::Subscription theme_;
};
}
