#include "FaultInjectionPanel.h"
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/wrapsizer.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/filedlg.h>
#include "eda-platform/Platform.h"
#include <fstream>
namespace sigflow::virtual_lab {
FaultInjectionPanel::FaultInjectionPanel(wxWindow* parent,std::shared_ptr<VirtualSessionController> c):wxScrolledWindow(parent),controller_(std::move(c)){
    auto* layout=new wxBoxSizer(wxVERTICAL);open_=new wxCheckBox(this,wxID_ANY,wxString::FromUTF8("断开 LED0 走线（wire.led0）"));
    status_=new wxStaticText(this,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_END|wxST_NO_AUTORESIZE);status_->SetMinSize(wxSize(0,FromDIP(20)));
    edges_["wire.led0"]=open_;auto* row=new wxWrapSizer(wxHORIZONTAL);row->Add(open_,0,wxALL,FromDIP(8));
    for(auto edge:{"wire.key3","wire.key4","wire.clock"}){auto* box=new wxCheckBox(this,wxID_ANY,wxString::FromUTF8(std::string("断开 ")+edge));edges_[edge]=box;row->Add(box,0,wxALL,FromDIP(8));}
    layout->Add(row,0,wxEXPAND);layout->Add(status_,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
    auto* inputrow=new wxWrapSizer(wxHORIZONTAL);input_=new wxChoice(this,wxID_ANY);input_->Append("P3 / key");input_->Append("P4 / reset");input_->SetSelection(0);
    state_=new wxChoice(this,wxID_ANY);state_->Append("FLT / release (2)");state_->Append("SLO / press (13)");state_->Append("CON / contention (44)");state_->SetSelection(0);
    drive_=new wxButton(this,wxID_ANY,wxString::FromUTF8("提交输入驱动（暂停时 pending）"));inputrow->Add(input_,0,wxRIGHT,FromDIP(6));inputrow->Add(state_,0,wxRIGHT,FromDIP(6));inputrow->Add(drive_);layout->Add(inputrow,0,wxALL,FromDIP(8));
    drive_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){int pad=input_->GetSelection()==0?3:4;auto target=controller_->InputTargets().find(pad);if(target==controller_->InputTargets().end())return;static const int states[]={2,13,44};controller_->Queue({{"op","set"},{"target",target->second},{"state",states[state_->GetSelection()]}});});
    phase1_=new wxPanel(this);auto* p1layout=new wxBoxSizer(wxVERTICAL);auto* profilebar=new wxWrapSizer(wxHORIZONTAL);
    auto* load=new wxButton(phase1_,wxID_ANY,wxString::FromUTF8("加载 fault-profile.json"));apply_profile_=new wxButton(phase1_,wxID_ANY,wxString::FromUTF8("应用编辑配置"));auto* save=new wxButton(phase1_,wxID_ANY,wxString::FromUTF8("保存配置"));
    stuck_mode_=new wxChoice(phase1_,wxID_ANY);stuck_mode_->Append("replace_source");stuck_mode_->Append("short_to_rail");stuck_mode_->SetSelection(0);stuck_mode_->SetToolTip(wxString::FromUTF8("选中 Stuck-at 规则后切换物理模式，生成新配置 revision"));
    for(auto* window:std::vector<wxWindow*>{load,apply_profile_,save,stuck_mode_})profilebar->Add(window,0,wxRIGHT|wxBOTTOM,FromDIP(5));p1layout->Add(profilebar,0,wxEXPAND|wxALL,FromDIP(5));
    auto* notebook=new wxNotebook(phase1_,wxID_ANY);rules_=new wxListCtrl(notebook,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(-1,145)),wxLC_REPORT|wxLC_SINGLE_SEL);for(auto label:{"故障 ID","类型 / 模式","目标","状态","触发次数","剩余抖动边沿"})rules_->AppendColumn(wxString::FromUTF8(label),wxLIST_FORMAT_LEFT,FromDIP(155));
    editor_=new wxTextCtrl(notebook,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(-1,145)),wxTE_MULTILINE|wxTE_DONTWRAP);notebook->AddPage(rules_,wxString::FromUTF8("规则状态（双击启停）"));notebook->AddPage(editor_,wxString::FromUTF8("编辑 JSON / 四类参数"));p1layout->Add(notebook,1,wxEXPAND);
    profile_status_=new wxStaticText(phase1_,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_END|wxST_NO_AUTORESIZE);profile_status_->SetMinSize(wxSize(0,FromDIP(24)));p1layout->Add(profile_status_,0,wxEXPAND|wxALL,FromDIP(5));phase1_->SetSizer(p1layout);layout->Add(phase1_,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(8));phase1_->Hide();
    load->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){wxFileDialog dialog(this,wxString::FromUTF8("加载教学故障配置"),"","fault-profile.json","JSON (*.json)|*.json",wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(dialog.ShowModal()!=wxID_OK)return;try{auto path=eda::platform::PathFromUtf8(dialog.GetPath().ToStdString(wxConvUTF8));if(std::filesystem::file_size(path)>1024*1024)throw std::runtime_error("E_FAULT_PROFILE_SIZE");std::ifstream in(path);eda::Json profile;in>>profile;LoadProfile(profile);}catch(const std::exception& e){profile_error_=e.what();Update();}});
    apply_profile_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){ApplyProfile();});save->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){wxFileDialog dialog(this,wxString::FromUTF8("保存故障配置"),"","fault-profile.json","JSON (*.json)|*.json",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);if(dialog.ShowModal()!=wxID_OK)return;try{auto profile=eda::Json::parse(editor_->GetValue().ToStdString(wxConvUTF8));std::ofstream out(eda::platform::PathFromUtf8(dialog.GetPath().ToStdString(wxConvUTF8)));out<<profile.dump(2)<<'\n';if(!out)throw std::runtime_error("E_FAULT_PROFILE_SAVE");}catch(const std::exception& e){profile_error_=e.what();Update();}});
    rules_->Bind(wxEVT_LIST_ITEM_ACTIVATED,[this](wxListEvent& event){auto index=size_t(event.GetIndex());if(index<fault_rows_.size())EnableFault(fault_rows_[index].at("id"),!fault_rows_[index].at("enabled").get<bool>());});
    stuck_mode_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){auto selected=rules_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED);if(selected>=0 && size_t(selected)<fault_rows_.size())try{SetStuckMode(fault_rows_[selected].at("id"),stuck_mode_->GetStringSelection().ToStdString(wxConvUTF8));}catch(const std::exception& e){profile_error_=e.what();Update();}});
    detail_=new wxTextCtrl(this,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(-1,80)),wxTE_MULTILINE|wxTE_READONLY|wxTE_DONTWRAP);layout->Add(detail_,1,wxEXPAND|wxALL,FromDIP(8));SetSizer(layout);SetScrollRate(0,FromDIP(12));
    for(auto [edge,box]:edges_)box->Bind(wxEVT_CHECKBOX,[this,edge,box](wxCommandEvent&){eda::Json command={{"op",box->GetValue()?"fault.apply":"fault.revoke"},{"fault_id",edge=="wire.led0"?"ui.led0.open":"ui."+edge+".open"}};if(box->GetValue())command["edge_id"]=edge;controller_->Queue(std::move(command));});
    subscription_=controller_->Subscribe([this]{Update();});theme_=UiTheme::Subscribe([this]{ApplyTheme();});ApplyTheme();Update();
}
FaultInjectionPanel::~FaultInjectionPanel(){controller_->Unsubscribe(subscription_);}
void FaultInjectionPanel::SetOpen(const std::string& edge,bool value){auto* box=edges_.at(edge);box->SetValue(value);wxCommandEvent event(wxEVT_CHECKBOX,box->GetId());event.SetEventObject(box);box->GetEventHandler()->ProcessEvent(event);}
std::string FaultInjectionPanel::DetailText()const{return detail_->GetValue().ToStdString(wxConvUTF8);}
void FaultInjectionPanel::DriveInput(int pad,int state){input_->SetSelection(pad==3?0:1);state_->SetSelection(state==2?0:state==13?1:2);wxCommandEvent event(wxEVT_BUTTON,drive_->GetId());event.SetEventObject(drive_);drive_->GetEventHandler()->ProcessEvent(event);}
void FaultInjectionPanel::LoadProfile(const eda::Json& profile){editor_->ChangeValue(wxString::FromUTF8(profile.dump(2)));wxCommandEvent event(wxEVT_BUTTON,apply_profile_->GetId());event.SetEventObject(apply_profile_);apply_profile_->GetEventHandler()->ProcessEvent(event);}
void FaultInjectionPanel::ApplyProfile(){try{auto profile=eda::Json::parse(editor_->GetValue().ToStdString(wxConvUTF8));const auto& frame=controller_->Frame();if(!frame.value("phase1",false))throw std::runtime_error("E_PHASE1_REQUIRED");
    if(frame.contains("faults") && frame.at("faults").value("profile_id",std::string())==profile.value("id",std::string()) && profile.value("revision",0)<=frame.at("faults").value("revision",0))profile["revision"]=frame.at("faults").at("revision").get<int>()+1;
    profile_=profile;editor_->ChangeValue(wxString::FromUTF8(profile.dump(2)));profile_error_.clear();controller_->Queue({{"op","profile.load"},{"profile",profile}});
    }catch(const std::exception& e){profile_error_=e.what();}Update();}
void FaultInjectionPanel::EnableFault(const std::string& id,bool enabled){controller_->Queue({{"op",enabled?"fault.enable":"fault.revoke"},{"fault_id",id},{"enabled",enabled}});}
void FaultInjectionPanel::SetStuckMode(const std::string& id,const std::string& mode){if(mode!="replace_source" && mode!="short_to_rail")throw std::runtime_error("E_STUCK_MODE");auto profile=eda::Json::parse(editor_->GetValue().ToStdString(wxConvUTF8));bool found=false;for(auto& fault:profile.at("faults"))if(fault.at("id")==id && fault.at("effect").at("type")=="stuck_at"){fault["effect"]["mode"]=mode;found=true;}if(!found)throw std::runtime_error("E_STUCK_RULE_REQUIRED");LoadProfile(profile);}
void FaultInjectionPanel::ApplyTheme(){auto& p=UiTheme::Palette();SetBackgroundColour(p.bgPanel);open_->SetForegroundColour(p.text1);status_->SetForegroundColour(p.text2);
    for(auto [edge,box]:edges_){box->SetForegroundColour(p.text1);box->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}
    for(auto* w:std::vector<wxWindow*>{input_,state_,drive_,detail_,phase1_,editor_,rules_,profile_status_,apply_profile_,stuck_mode_}){w->SetForegroundColour(p.text1);w->SetBackgroundColour(p.surfaceOnPanel);w->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}status_->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));Refresh();}
void FaultInjectionPanel::Update(){
    bool blind=controller_->Frame().value("mode",std::string())=="student_blind_test";if(blind_locked_ && !blind)for(auto* child:GetChildren())child->Enable();blind_locked_=blind;
    const auto& f=controller_->Frame();bool active=false,pending=false,desired=false;
    phase1_->Show(f.value("phase1",false));apply_profile_->Enable(f.value("phase1",false) && CanCommand(controller_->State(),"profile.load"));
    if(f.value("phase1",false) && f.contains("faults")){fault_rows_=f.at("faults").at("rules");auto signature=fault_rows_.dump()+f.value("pending_mutations",eda::Json::array()).dump();if(signature!=rules_signature_){rules_signature_=std::move(signature);rules_->DeleteAllItems();for(const auto& rule:fault_rows_){auto row=rules_->InsertItem(rules_->GetItemCount(),wxString::FromUTF8(rule.at("id").get<std::string>()));auto type=rule.at("type").get<std::string>();if(rule.contains("effect") && rule.at("effect").contains("mode"))type+=" / "+rule.at("effect").at("mode").get<std::string>();rules_->SetItem(row,1,wxString::FromUTF8(type));rules_->SetItem(row,2,wxString::FromUTF8(rule.at("target").at("id").get<std::string>()));std::string status=rule.at("active").get<bool>()?"active":rule.at("enabled").get<bool>()?"armed":"disabled";
        for(const auto& action:f.value("pending_mutations",eda::Json::array()))if(action.value("op","")=="profile.load" || action.value("fault_id",std::string())==rule.at("id").get<std::string>())status="pending";rules_->SetItem(row,3,wxString::FromUTF8(status));rules_->SetItem(row,4,wxString::FromUTF8(rule.at("activations").get<std::string>()));rules_->SetItem(row,5,std::to_string(rule.at("remaining_edges").get<size_t>()));}}
        auto text=profile_error_.empty()?"profile="+f.at("faults").at("profile_id").get<std::string>()+" revision="+std::to_string(f.at("faults").at("revision").get<int>()):profile_error_;profile_status_->SetLabel(wxString::FromUTF8(text));profile_status_->SetForegroundColour(profile_error_.empty()?UiTheme::Palette().text2:UiTheme::Palette().danger);}
    drive_->Enable(CanCommand(controller_->State(),"set"));
    if(f.contains("board") && f.at("board").contains("wires"))active=!f.at("board").at("wires").value("wire.led0",true);desired=active;
    if(f.contains("pending_faults"))for(const auto& p:f.at("pending_faults"))if(p.at("fault_id")=="ui.led0.open"){pending=true;desired=p.at("op")=="apply";}
    open_->SetValue(desired);for(auto [edge,box]:edges_){bool wanted=f.contains("board") && f.at("board").contains("wires")?!f.at("board").at("wires").value(edge,true):false;
        if(f.contains("pending_faults"))for(const auto& p:f.at("pending_faults"))if(p.value("edge_id",std::string())==edge)wanted=p.at("op")=="apply";
        box->SetValue(wanted);box->Enable(CanCommand(controller_->State(),"fault.apply"));}
    std::string tick="0";if(f.contains("topology_position"))tick=f.at("topology_position").at("tick");
    auto message=std::string(pending?"pending：Step/Play 后生效":(active?"active：走线开路，LED 端悬空":"正常：走线接通"))+" | 生效 tick="+tick+" ps";
    status_->SetLabel(wxString::FromUTF8(message));std::string detail=f.value("reason",std::string());if(!controller_->Error().empty())detail+="\nerror: "+controller_->Error();if(f.contains("error"))detail+="\nerror: "+f.at("error").dump();for(auto key:{"attempted_position","position","guard","pending_faults"})if(f.contains(key))detail+="\n"+std::string(std::string(key)=="position"?"last_committed_position":key)+": "+f.at(key).dump();detail_->ChangeValue(wxString::FromUTF8(detail));if(blind){fault_rows_=eda::Json::array();profile_=eda::Json::object();rules_->DeleteAllItems();editor_->ChangeValue("");phase1_->Hide();status_->SetLabel(wxString::FromUTF8("盲测模式：故障控制已锁定。请使用双端探针、波形、分级提示与修复判题。"));for(auto* child:GetChildren())child->Enable(false);}Layout();FitInside();
}
}
