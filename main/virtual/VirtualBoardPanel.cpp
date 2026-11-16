#include "VirtualBoardPanel.h"
#include "BoardRenderer.h"
#include "eda-platform/Platform.h"
#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
namespace sigflow::virtual_lab {
VirtualBoardPanel::VirtualBoardPanel(wxWindow* parent,std::shared_ptr<VirtualSessionController> controller):wxPanel(parent),controller_(std::move(controller)){
    auto* layout=new wxBoxSizer(wxVERTICAL);auto* toolbar=new wxWrapSizer(wxHORIZONTAL);
    auto button=[&](const std::string& op,const char* label,auto callback){auto* b=new wxButton(this,wxID_ANY,wxString::FromUTF8(label));controls_[op]=b;toolbar->Add(b,0,wxRIGHT|wxBOTTOM,FromDIP(5));b->Bind(wxEVT_BUTTON,callback);};
    button("current","构建并启动当前工程",[this](wxCommandEvent&){if(project_provider_){try{controller_->BuildProject(project_provider_());}catch(const std::exception& e){detail_->SetValue(wxString::FromUTF8(e.what()));}}else Load();});
    button("reference","加载 M0 参考计数器",[this](wxCommandEvent&){controller_->BuildReference();});
    button("load","加载实验配置",[this](wxCommandEvent&){Load();});
    for(const auto& op:{"play","step","pause","stop"})button(op,op,[this,op](wxCommandEvent&){controller_->Queue({{"op",op}});});
    button("replay","重放当前日志",[this](wxCommandEvent&){controller_->Replay(replay_count_->GetValue());});
    button("replay-file","重放指定 Journal",[this](wxCommandEvent&){wxFileDialog d(this,wxString::FromUTF8("选择已关闭的 Journal"),"","","Journal (*.jsonl;*.journal)|*.jsonl;*.journal|All files|*",wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(d.ShowModal()==wxID_OK)controller_->Replay(replay_count_->GetValue(),d.GetPath().ToStdString(wxConvUTF8));});
    replay_count_=new wxSpinCtrl(this,wxID_ANY,"3",wxDefaultPosition,FromDIP(wxSize(60,-1)),wxSP_ARROW_KEYS,1,10,3);replay_count_->SetToolTip(wxString::FromUTF8("从零重放次数"));toolbar->Add(replay_count_,0,wxBOTTOM,FromDIP(5));
    layout->Add(toolbar,0,wxEXPAND|wxALL,FromDIP(8));
    drawing_=new wxPanel(this);drawing_->SetBackgroundStyle(wxBG_STYLE_PAINT);drawing_->SetMinSize(FromDIP(wxSize(320,165)));
    drawing_->Bind(wxEVT_PAINT,[this](wxPaintEvent&){wxAutoBufferedPaintDC dc(drawing_);dc.SetBackground(wxBrush(UiTheme::Palette().canvas));dc.Clear();std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));if(g)BoardRenderer::Paint(*drawing_,*g,controller_->Frame(),controller_->InputTargets());});
    drawing_->SetToolTip(wxString::FromUTF8("暂停时只暂存输入；按住按键并按空格单步，松开后再次单步提交释放。"));
    drawing_->Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent& e){int pad=BoardRenderer::HitButton(*drawing_,e.GetPosition());if(pad>0 && CanCommand(controller_->State(),"set")){held_pad_=pad;drawing_->SetFocus();drawing_->CaptureMouse();Contact(pad,true);}});
    drawing_->Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&){ReleaseContact();});drawing_->Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){ReleaseContact();});drawing_->Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){ReleaseContact();e.Skip();});
    drawing_->Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& e){if(e.GetKeyCode()==WXK_SPACE && CanCommand(controller_->State(),"step"))controller_->Queue({{"op","step"}});else if(e.GetKeyCode()==WXK_ESCAPE)ReleaseContact();else e.Skip();});
    layout->Add(drawing_,1,wxEXPAND);status_=new wxStaticText(this,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_END|wxST_NO_AUTORESIZE);status_->SetMinSize(wxSize(0,FromDIP(20)));layout->Add(status_,0,wxEXPAND|wxALL,FromDIP(6));
    diagnostics_=new wxListCtrl(this,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(-1,88)),wxLC_REPORT|wxLC_SINGLE_SEL);int col=0;for(auto title:{"错误码","PortBitRef","物理脚","Bank / VCCIO","修复建议"})diagnostics_->InsertColumn(col++,wxString::FromUTF8(title));
    diagnostics_->Bind(wxEVT_LIST_ITEM_ACTIVATED,[this](wxListEvent& e){controller_->LocateDiagnostic(size_t(e.GetIndex()));});layout->Add(diagnostics_,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(6));diagnostics_->Hide();
    detail_=new wxTextCtrl(this,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(-1,90)),wxTE_MULTILINE|wxTE_READONLY|wxTE_DONTWRAP);layout->Add(detail_,0,wxEXPAND|wxALL,FromDIP(6));SetSizer(layout);
    subscription_=controller_->Subscribe([this]{Update();});theme_=UiTheme::Subscribe([this]{ApplyTheme();});ApplyTheme();Update();
}
VirtualBoardPanel::~VirtualBoardPanel(){controller_->Unsubscribe(subscription_);}
void VirtualBoardPanel::InvokeControl(const std::string& op){auto* b=controls_.at(op);if(!b->IsEnabled())throw std::runtime_error("E_DISABLED_CONTROL: "+op);wxCommandEvent e(wxEVT_BUTTON,b->GetId());e.SetEventObject(b);b->GetEventHandler()->ProcessEvent(e);}
std::string VirtualBoardPanel::DetailText()const{return detail_->GetValue().ToStdString(wxConvUTF8);}
void VirtualBoardPanel::Contact(int pad,bool pressed){auto it=controller_->InputTargets().find(pad);if(it!=controller_->InputTargets().end())controller_->Queue({{"op","set"},{"target",it->second},{"state",pressed?13:2}});}
void VirtualBoardPanel::ReleaseContact(){int pad=held_pad_;held_pad_=-1;if(drawing_->HasCapture())drawing_->ReleaseMouse();if(pad>0)Contact(pad,false);}
void VirtualBoardPanel::ApplyTheme(){auto& p=UiTheme::Palette();SetBackgroundColour(p.bgPanel);for(auto* w:std::vector<wxWindow*>{status_,detail_,diagnostics_}){w->SetForegroundColour(p.text2);w->SetBackgroundColour(p.surfaceOnPanel);w->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}
    for(const auto& [op,b]:controls_){b->SetForegroundColour(p.text1);b->SetBackgroundColour(p.surfaceOnPanel);b->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}drawing_->Refresh();}
void VirtualBoardPanel::Update(){
    for(const auto& [op,b]:controls_){bool enabled=CanCommand(controller_->State(),op);if(op=="load" || op=="current" || op=="reference")enabled=controller_->State()!=SessionState::Running && controller_->State()!=SessionState::Building;
        if(op=="replay" || op=="replay-file")enabled=CanCommand(controller_->State(),"replay.current") && controller_->ReplayResult().value("status","")!="running";b->Enable(enabled);}
    const auto& f=controller_->Frame();std::string tick=f.contains("position")?f.at("position").value("tick",std::string("0")):"0";
    auto message=std::string(StateName(controller_->State()))+" | "+f.value("reason",std::string())+" | tick="+tick+" ps | CST="+f.value("binding_status",std::string("未加载"))+" | pending="+std::to_string(f.value("pending_count",size_t(0)));
    if(f.contains("trace"))message+=" | trace="+std::to_string(controller_->Source()?controller_->Source()->CurrentReadView().Revision():0)+"/"+f.at("trace").at("revision").get<std::string>();
    if(!controller_->Error().empty())message+=" | "+controller_->Error();status_->SetLabel(wxString::FromUTF8(message));status_->SetToolTip(wxString::FromUTF8(message));
    std::string detail=controller_->Error().empty()?"":"error: "+controller_->Error()+"\n";for(auto key:{"attempted_position","position","guard"})if(f.contains(key))detail+=std::string(std::string(key)=="position"?"last_committed_position":key)+": "+f.at(key).dump()+"\n";
    if(f.contains("error") && !f.at("error").get<std::string>().empty())detail+="error: "+f.at("error").dump()+"\n";
    if(!controller_->ReplayResult().empty())detail+="Replay: "+controller_->ReplayResult().dump(2)+"\n";
    const auto& build=controller_->BuildResult();if(build.contains("cache_key"))detail+="build cache="+build.at("cache_key").get<std::string>()+(build.value("cache_hit",false)?" (hit)":" (compiled)")+"\n";
    auto ds=build.value("diagnostics",eda::Json::array());auto signature=ds.dump();if(signature!=diagnostic_signature_){diagnostic_signature_=signature;diagnostics_->DeleteAllItems();for(const auto& d:ds){long row=diagnostics_->InsertItem(diagnostics_->GetItemCount(),wxString::FromUTF8(d.value("code","")));diagnostics_->SetItem(row,1,wxString::FromUTF8(d.value("port_ref","")));diagnostics_->SetItem(row,2,wxString::FromUTF8(d.value("package_pin",eda::Json(nullptr)).dump()));diagnostics_->SetItem(row,3,wxString::FromUTF8(d.value("bank_id","")+" / "+d.value("vccio_mv",eda::Json(nullptr)).dump()));diagnostics_->SetItem(row,4,wxString::FromUTF8(d.value("suggestion",d.value("message",""))));}for(int c=0;c<5;++c)diagnostics_->SetColumnWidth(c,c==4?FromDIP(390):wxLIST_AUTOSIZE_USEHEADER);diagnostics_->Show(!ds.empty());}
    for(const auto& d:ds)detail+=d.dump()+"\n";detail_->ChangeValue(wxString::FromUTF8(detail));drawing_->Refresh();Layout();
}
void VirtualBoardPanel::Load(){wxFileDialog d(this,wxString::FromUTF8("选择 virtual.project.json"),"","virtual.project.json","JSON (*.json)|*.json",wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(d.ShowModal()!=wxID_OK)return;try{controller_->BuildProject(VirtualProjectSessionBuilder::ReadProject(eda::platform::PathFromUtf8(d.GetPath().ToStdString(wxConvUTF8))));}catch(const std::exception& e){detail_->SetValue(wxString::FromUTF8(e.what()));}}
}
