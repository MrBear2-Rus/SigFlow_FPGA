#include "VirtualUartPanel.h"
#include "eda-platform/Platform.h"
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/filedlg.h>
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
namespace sigflow::virtual_lab {
namespace {std::string Key(const eda::Json& frame){return frame.at("uart").get<std::string>()+":"+frame.at("direction").get<std::string>()+":"+frame.at("sequence").get<std::string>();}}
VirtualUartPanel::VirtualUartPanel(wxWindow* parent,std::shared_ptr<VirtualSessionController> controller):wxPanel(parent),controller_(std::move(controller)){
    auto* layout=new wxBoxSizer(wxVERTICAL);auto* row=new wxWrapSizer(wxHORIZONTAL);target_=new wxChoice(this,wxID_ANY);target_->Append("uart0");target_->SetSelection(0);format_=new wxChoice(this,wxID_ANY);format_->Append("ASCII");format_->Append("Hex");format_->SetSelection(0);
    line_end_=new wxChoice(this,wxID_ANY);for(auto label:{"none","LF","CR","CRLF"})line_end_->Append(label);line_end_->SetSelection(0);input_=new wxTextCtrl(this,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(270,-1)),wxTE_PROCESS_ENTER);send_=new wxButton(this,wxID_ANY,wxString::FromUTF8("发送（按仿真 Tick 排队）"));
    auto* clear=new wxButton(this,wxID_ANY,wxString::FromUTF8("清屏"));auto* exportButton=new wxButton(this,wxID_ANY,wxString::FromUTF8("导出全部帧"));for(auto* window:std::vector<wxWindow*>{target_,format_,line_end_,input_,send_,clear,exportButton})row->Add(window,0,wxRIGHT|wxBOTTOM,FromDIP(5));layout->Add(row,0,wxEXPAND|wxALL,FromDIP(6));
    status_=new wxStaticText(this,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_END|wxST_NO_AUTORESIZE);status_->SetMinSize(wxSize(0,FromDIP(22)));layout->Add(status_,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(6));
    log_=new wxListCtrl(this,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(-1,120)),wxLC_REPORT|wxLC_SINGLE_SEL);for(auto label:{"Tick (ps)","UART","方向","Hex","ASCII","帧状态"})log_->AppendColumn(wxString::FromUTF8(label),wxLIST_FORMAT_LEFT,FromDIP(110));log_->SetColumnWidth(5,FromDIP(270));layout->Add(log_,1,wxEXPAND|wxALL,FromDIP(6));SetSizer(layout);
    send_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){OnSend();});input_->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){OnSend();});clear->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Clear();});exportButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){wxFileDialog dialog(this,wxString::FromUTF8("导出带 Tick 与错误态的 UART 帧"),"","uart-frames.json","JSON (*.json)|*.json",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);if(dialog.ShowModal()==wxID_OK)try{Export(eda::platform::PathFromUtf8(dialog.GetPath().ToStdString(wxConvUTF8)));}catch(const std::exception& e){error_=e.what();Update();}});
    subscription_=controller_->Subscribe([this]{Update();});theme_=UiTheme::Subscribe([this]{ApplyTheme();});ApplyTheme();Update();
}
VirtualUartPanel::~VirtualUartPanel(){controller_->Unsubscribe(subscription_);}
void VirtualUartPanel::Send(const std::string& body,bool hex,const std::string& end){input_->ChangeValue(wxString::FromUTF8(body));format_->SetSelection(hex?1:0);auto choice=line_end_->FindString(wxString::FromUTF8(end));if(choice==wxNOT_FOUND)throw std::runtime_error("E_UART_LINE_END");line_end_->SetSelection(choice);wxCommandEvent event(wxEVT_BUTTON,send_->GetId());event.SetEventObject(send_);send_->GetEventHandler()->ProcessEvent(event);}
void VirtualUartPanel::OnSend(){try{
    error_.clear();if(!controller_->Frame().value("phase1",false) || !CanCommand(controller_->State(),"uart.send"))throw std::runtime_error("请先构建并启动 Phase 1 UART 实验");std::string text=input_->GetValue().ToStdString(wxConvUTF8);std::vector<uint8_t> bytes;
    if(format_->GetSelection()==1){std::string digits;for(unsigned char c:text)if(!std::isspace(c)){if(!std::isxdigit(c))throw std::runtime_error("E_UART_HEX: 请输入十六进制字节");digits+=char(c);}if(digits.empty() || digits.size()%2)throw std::runtime_error("E_UART_HEX: 每个字节需要两位");for(size_t i=0;i<digits.size();i+=2)bytes.push_back(uint8_t(std::stoul(digits.substr(i,2),nullptr,16)));}
    else for(unsigned char c:text){if(c>127)throw std::runtime_error("E_UART_ASCII: 非 ASCII 字节请使用 Hex 模式");bytes.push_back(c);}
    int ending=line_end_->GetSelection();if(ending==2 || ending==3)bytes.push_back(13);if(ending==1 || ending==3)bytes.push_back(10);if(bytes.empty())throw std::runtime_error("E_UART_EMPTY");
    controller_->Queue({{"op","uart.send"},{"target",target_->GetStringSelection().ToStdString(wxConvUTF8)},{"bytes",bytes}});status_->SetLabel(wxString::FromUTF8("pending：Step/Play 后从 TX 接点逐比特发送"));
    }catch(const std::exception& e){error_=e.what();status_->SetLabel(wxString::FromUTF8(error_));}}
eda::Json VirtualUartPanel::AllRecords()const{
    eda::Json result=eda::Json::array();const auto& frame=controller_->Frame();if(!frame.contains("p1"))return result;const auto& uarts=frame.at("p1").at("peripherals").at("uart");
    for(const auto& [id,uart]:uarts.items())for(auto pair:{std::make_pair("received","RX"),std::make_pair("transmitted","TX")})for(const auto& event:uart.at(pair.first)){auto record=event;record["uart"]=id;record["direction"]=pair.second;result.push_back(std::move(record));}
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){auto at=std::stoull(a.at("tick").template get<std::string>()),bt=std::stoull(b.at("tick").template get<std::string>());if(at!=bt)return at<bt;return Key(a)<Key(b);});return result;
}
void VirtualUartPanel::Clear(){for(const auto& record:AllRecords())hidden_.insert(Key(record));signature_.clear();Update();}
void VirtualUartPanel::Export(const std::filesystem::path& path)const{std::ofstream out(path,std::ios::binary);if(!out)throw std::runtime_error("E_UART_EXPORT");out<<eda::Json{{"schema","sigflow.uart-frames.v1"},{"run_id",controller_->Frame().value("run_id",std::string())},{"tick_ps","1"},{"frames",AllRecords()}}.dump(2)<<'\n';if(!out)throw std::runtime_error("E_UART_EXPORT");}
void VirtualUartPanel::Update(){const auto& frame=controller_->Frame();bool enabled=frame.value("phase1",false) && CanCommand(controller_->State(),"uart.send");send_->Enable(enabled);
    std::string run=frame.value("run_id",std::string());if(run!=run_){run_=run;hidden_.clear();signature_.clear();error_.clear();}
    if(frame.contains("p1")){auto selection=target_->GetStringSelection();target_->Clear();for(const auto& [id,uart]:frame.at("p1").at("peripherals").at("uart").items())target_->Append(wxString::FromUTF8(id));if(!target_->SetStringSelection(selection) && target_->GetCount())target_->SetSelection(0);send_->Enable(enabled && target_->GetCount());}
    auto all=AllRecords();std::string signature=all.dump();if(signature!=signature_){signature_=std::move(signature);records_=eda::Json::array();log_->Freeze();log_->DeleteAllItems();for(const auto& record:all){if(hidden_.count(Key(record)))continue;records_.push_back(record);auto row=log_->InsertItem(log_->GetItemCount(),wxString::FromUTF8(record.at("tick").get<std::string>()));unsigned byte=record.at("byte");log_->SetItem(row,1,wxString::FromUTF8(record.at("uart").get<std::string>()));log_->SetItem(row,2,wxString::FromUTF8(record.at("direction").get<std::string>()));log_->SetItem(row,3,wxString::Format("%02X",byte));log_->SetItem(row,4,byte>=32 && byte<127?wxString::Format("%c",int(byte)):".");log_->SetItem(row,5,record.value("valid",false)?"valid":record.value("framing_error",false)?"framing_error / FLT / CON":"parity_error");if(!record.value("valid",false))log_->SetItemTextColour(row,UiTheme::Palette().danger);}log_->Thaw();}
    std::string status=error_.empty()?"RX/TX 帧="+std::to_string(records_.size())+" | tick="+(frame.contains("position")?frame.at("position").value("tick",std::string("0")):"0")+" ps | pending="+std::to_string(frame.value("pending_count",size_t(0))):error_;status_->SetLabel(wxString::FromUTF8(status));status_->SetToolTip(wxString::FromUTF8(status));
}
void VirtualUartPanel::ApplyTheme(){const auto& p=UiTheme::Palette();SetBackgroundColour(p.bgPanel);for(auto* child:GetChildren()){child->SetForegroundColour(p.text1);child->SetBackgroundColour(p.surfaceOnPanel);child->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}status_->SetForegroundColour(p.text2);signature_.clear();Update();Refresh();}
}
