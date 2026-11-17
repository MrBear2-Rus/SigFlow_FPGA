#pragma once
#include "VirtualSessionController.h"
#include "ui/UiTheme.h"
#include <wx/panel.h>
#include <set>
class wxChoice;class wxTextCtrl;class wxButton;class wxListCtrl;class wxStaticText;
namespace sigflow::virtual_lab {
class VirtualUartPanel:public wxPanel {
public:
    VirtualUartPanel(wxWindow*,std::shared_ptr<VirtualSessionController>);
    ~VirtualUartPanel();
    void Send(const std::string&,bool hex=false,const std::string& lineEnd="none");
    void Clear();
    void Export(const std::filesystem::path&)const;
    const eda::Json& VisibleRecords()const{return records_;}
    const std::string& Error()const{return error_;}
private:
    void Update();void ApplyTheme();void OnSend();
    eda::Json AllRecords()const;
    std::shared_ptr<VirtualSessionController> controller_;
    wxChoice *target_,*format_,*line_end_;wxTextCtrl* input_;wxButton* send_;wxListCtrl* log_;wxStaticText* status_;
    std::string run_,signature_,error_;
    eda::Json records_=eda::Json::array();std::set<std::string> hidden_;
    size_t subscription_;UiTheme::Subscription theme_;
};
}
