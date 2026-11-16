#include "FaultInjectionPanel.h"
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/wrapsizer.h>
namespace sigflow::virtual_lab {
FaultInjectionPanel::FaultInjectionPanel(wxWindow* parent,std::shared_ptr<VirtualSessionController> c):wxPanel(parent),controller_(std::move(c)){
    auto* layout=new wxBoxSizer(wxVERTICAL);open_=new wxCheckBox(this,wxID_ANY,wxString::FromUTF8("断开 LED0 走线（wire.led0）"));
    status_=new wxStaticText(this,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_END|wxST_NO_AUTORESIZE);status_->SetMinSize(wxSize(0,FromDIP(20)));
    edges_["wire.led0"]=open_;auto* row=new wxWrapSizer(wxHORIZONTAL);row->Add(open_,0,wxALL,FromDIP(8));
    for(auto edge:{"wire.key3","wire.key4","wire.clock"}){auto* box=new wxCheckBox(this,wxID_ANY,wxString::FromUTF8(std::string("断开 ")+edge));edges_[edge]=box;row->Add(box,0,wxALL,FromDIP(8));}
    layout->Add(row,0,wxEXPAND);layout->Add(status_,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
    auto* inputrow=new wxWrapSizer(wxHORIZONTAL);input_=new wxChoice(this,wxID_ANY);input_->Append("P3 / key");input_->Append("P4 / reset");input_->SetSelection(0);
    state_=new wxChoice(this,wxID_ANY);state_->Append("FLT / release (2)");state_->Append("SLO / press (13)");state_->Append("CON / contention (44)");state_->SetSelection(0);
    drive_=new wxButton(this,wxID_ANY,wxString::FromUTF8("提交输入驱动（暂停时 pending）"));inputrow->Add(input_,0,wxRIGHT,FromDIP(6));inputrow->Add(state_,0,wxRIGHT,FromDIP(6));inputrow->Add(drive_);layout->Add(inputrow,0,wxALL,FromDIP(8));
    drive_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){int pad=input_->GetSelection()==0?3:4;auto target=controller_->InputTargets().find(pad);if(target==controller_->InputTargets().end())return;static const int states[]={2,13,44};controller_->Queue({{"op","set"},{"target",target->second},{"state",states[state_->GetSelection()]}});});
    detail_=new wxTextCtrl(this,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(-1,80)),wxTE_MULTILINE|wxTE_READONLY|wxTE_DONTWRAP);layout->Add(detail_,1,wxEXPAND|wxALL,FromDIP(8));SetSizer(layout);
    for(auto [edge,box]:edges_)box->Bind(wxEVT_CHECKBOX,[this,edge,box](wxCommandEvent&){eda::Json command={{"op",box->GetValue()?"fault.apply":"fault.revoke"},{"fault_id",edge=="wire.led0"?"ui.led0.open":"ui."+edge+".open"}};if(box->GetValue())command["edge_id"]=edge;controller_->Queue(std::move(command));});
    subscription_=controller_->Subscribe([this]{Update();});theme_=UiTheme::Subscribe([this]{ApplyTheme();});ApplyTheme();Update();
}
FaultInjectionPanel::~FaultInjectionPanel(){controller_->Unsubscribe(subscription_);}
void FaultInjectionPanel::SetOpen(const std::string& edge,bool value){auto* box=edges_.at(edge);box->SetValue(value);wxCommandEvent event(wxEVT_CHECKBOX,box->GetId());event.SetEventObject(box);box->GetEventHandler()->ProcessEvent(event);}
std::string FaultInjectionPanel::DetailText()const{return detail_->GetValue().ToStdString(wxConvUTF8);}
void FaultInjectionPanel::DriveInput(int pad,int state){input_->SetSelection(pad==3?0:1);state_->SetSelection(state==2?0:state==13?1:2);wxCommandEvent event(wxEVT_BUTTON,drive_->GetId());event.SetEventObject(drive_);drive_->GetEventHandler()->ProcessEvent(event);}
void FaultInjectionPanel::ApplyTheme(){auto& p=UiTheme::Palette();SetBackgroundColour(p.bgPanel);open_->SetForegroundColour(p.text1);status_->SetForegroundColour(p.text2);
    for(auto [edge,box]:edges_){box->SetForegroundColour(p.text1);box->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}
    for(auto* w:std::vector<wxWindow*>{input_,state_,drive_,detail_}){w->SetForegroundColour(p.text1);w->SetBackgroundColour(p.surfaceOnPanel);w->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}status_->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));Refresh();}
void FaultInjectionPanel::Update(){
    const auto& f=controller_->Frame();bool active=false,pending=false,desired=false;
    drive_->Enable(CanCommand(controller_->State(),"set"));
    if(f.contains("board") && f.at("board").contains("wires"))active=!f.at("board").at("wires").value("wire.led0",true);desired=active;
    if(f.contains("pending_faults"))for(const auto& p:f.at("pending_faults"))if(p.at("fault_id")=="ui.led0.open"){pending=true;desired=p.at("op")=="apply";}
    open_->SetValue(desired);for(auto [edge,box]:edges_){bool wanted=f.contains("board") && f.at("board").contains("wires")?!f.at("board").at("wires").value(edge,true):false;
        if(f.contains("pending_faults"))for(const auto& p:f.at("pending_faults"))if(p.value("edge_id",std::string())==edge)wanted=p.at("op")=="apply";
        box->SetValue(wanted);box->Enable(CanCommand(controller_->State(),"fault.apply"));}
    std::string tick="0";if(f.contains("topology_position"))tick=f.at("topology_position").at("tick");
    auto message=std::string(pending?"pending：Step/Play 后生效":(active?"active：走线开路，LED 端悬空":"正常：走线接通"))+" | 生效 tick="+tick+" ps";
    status_->SetLabel(wxString::FromUTF8(message));std::string detail=f.value("reason",std::string());if(!controller_->Error().empty())detail+="\nerror: "+controller_->Error();if(f.contains("error"))detail+="\nerror: "+f.at("error").dump();for(auto key:{"attempted_position","position","guard","pending_faults"})if(f.contains(key))detail+="\n"+std::string(std::string(key)=="position"?"last_committed_position":key)+": "+f.at(key).dump();detail_->ChangeValue(wxString::FromUTF8(detail));Layout();
}
}
