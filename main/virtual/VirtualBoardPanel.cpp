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
#include <wx/choice.h>
namespace sigflow::virtual_lab {
namespace {
std::string ResolveBoardNode(const eda::Json& frame,const std::string& hit){
    if(hit.empty() || !frame.contains("board") || !frame.at("board").contains("nodes"))return {};
    const auto& nodes=frame.at("board").at("nodes");
    if(nodes.contains(hit))return hit;
    constexpr int pads[]={10,11,13,14,15,16};
    for(int i=0;i<6;++i){
        if(hit=="led"+std::to_string(i)+".K"){
            std::string pad="pad."+std::to_string(pads[i]);
            if(nodes.contains(pad))return pad;
        }
    }
    return {};
}
}
const std::vector<VirtualBoardPanel::ExampleEntry>& VirtualBoardPanel::Examples(){
    static const std::vector<ExampleEntry> items={
        {"m0-counter","M0 · 参考计数器","examples/virtual/counter/virtual.project.json"},
        {"p1-peripherals","P1 · PWM/数码管/DIP/UART","examples/virtual/phase1/virtual.project.json"},
        {"p2-debug","P2 · 13态争用与调试","examples/virtual/phase2/virtual.project.json"},
        {"p3-keypad-scan","P3 · 4×4 矩阵键盘扫描","examples/virtual/phase3/keypad-scan/virtual.project.json"},
        {"p4-spi-oled","P4 · SPI OLED 点阵屏","examples/virtual/phase4/spi-oled/virtual.project.json"},
        {"p4-crc-cdc","P4 · CRC8 篡改与 CDC 采样","examples/virtual/phase4/crc-cdc/virtual.project.json"},
        {"p5-i2c-burst","P5 · I2C 多字节连续读传感器","examples/virtual/phase5/i2c-burst/virtual.project.json"}
    };
    return items;
}
VirtualBoardPanel::VirtualBoardPanel(wxWindow* parent,std::shared_ptr<VirtualSessionController> controller):wxPanel(parent),controller_(std::move(controller)){
    auto* layout=new wxBoxSizer(wxVERTICAL);auto* toolbar=new wxWrapSizer(wxHORIZONTAL);
    auto button=[&](const std::string& op,const char* label,auto callback){auto* b=new wxButton(this,wxID_ANY,wxString::FromUTF8(label));controls_[op]=b;toolbar->Add(b,0,wxRIGHT|wxBOTTOM,FromDIP(5));b->Bind(wxEVT_BUTTON,callback);};
    button("current","构建并启动当前工程",[this](wxCommandEvent&){if(project_provider_){try{controller_->BuildProject(project_provider_());}catch(const std::exception& e){detail_->SetValue(wxString::FromUTF8(e.what()));}}else Load();});
    button("reference","加载 M0 参考计数器",[this](wxCommandEvent&){controller_->BuildReference();});
    button("reference-p1","加载 P1 外设实验",[this](wxCommandEvent&){try{controller_->BuildProject(VirtualProjectSessionBuilder::ReadProject(VirtualProjectSessionBuilder::FindRepository()/"examples/virtual/phase1/virtual.project.json"));}catch(const std::exception& e){detail_->SetValue(wxString::FromUTF8(e.what()));}});
    button("reference-p2","加载 P2 调试实验",[this](wxCommandEvent&){try{controller_->BuildProject(VirtualProjectSessionBuilder::ReadProject(VirtualProjectSessionBuilder::FindRepository()/"examples/virtual/phase2/virtual.project.json"));}catch(const std::exception& e){detail_->SetValue(wxString::FromUTF8(e.what()));}});
    example_picker_=new wxChoice(this,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(210,-1)));
    example_picker_->Append(wxString::FromUTF8("实验示例 ▾"));
    for(const auto& item:Examples())example_picker_->Append(wxString::FromUTF8(item.label));
    example_picker_->SetSelection(0);
    example_picker_->SetToolTip(wxString::FromUTF8("一键加载 M0–P5 任意阶段参考实验并启动虚拟实验箱"));
    example_picker_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){
        int sel=example_picker_->GetSelection();
        if(sel>=1 && size_t(sel-1)<Examples().size())LoadExample(Examples().at(size_t(sel-1)).id);
    });
    toolbar->Add(example_picker_,0,wxRIGHT|wxBOTTOM,FromDIP(5));
    button("load","加载实验配置",[this](wxCommandEvent&){Load();});
    for(const auto& op:{"play","step","pause","stop"})button(op,op,[this,op](wxCommandEvent&){if(std::string(op)=="step")Step();else controller_->Queue({{"op",op}});});
    button("replay","重放当前日志",[this](wxCommandEvent&){controller_->Replay(replay_count_->GetValue());});
    button("replay-file","重放指定 Journal",[this](wxCommandEvent&){wxFileDialog d(this,wxString::FromUTF8("选择已关闭的 Journal"),"","","Journal (*.jsonl;*.journal)|*.jsonl;*.journal|All files|*",wxFD_OPEN|wxFD_FILE_MUST_EXIST);if(d.ShowModal()==wxID_OK)controller_->Replay(replay_count_->GetValue(),d.GetPath().ToStdString(wxConvUTF8));});
    // 拖出独立窗口按钮由宿主安装回调时懒创建（MainFrame 接线后出现）。
    toolbar_=toolbar;
    replay_count_=new wxSpinCtrl(this,wxID_ANY,"3",wxDefaultPosition,FromDIP(wxSize(60,-1)),wxSP_ARROW_KEYS,1,10,3);replay_count_->SetToolTip(wxString::FromUTF8("从零重放次数"));toolbar->Add(replay_count_,0,wxBOTTOM,FromDIP(5));
    layout->Add(toolbar,0,wxEXPAND|wxALL,FromDIP(8));
    phase1_controls_=new wxPanel(this);auto* p1row=new wxWrapSizer(wxHORIZONTAL);step_mode_=new wxChoice(phase1_controls_,wxID_ANY);for(auto label:{"下一主时钟上升沿","下一事件 Next Event","指定 Tick 数"})step_mode_->Append(wxString::FromUTF8(label));step_mode_->SetSelection(0);p1row->Add(step_mode_,0,wxRIGHT|wxBOTTOM,FromDIP(6));
    step_ticks_=new wxTextCtrl(phase1_controls_,wxID_ANY,"1000000",wxDefaultPosition,FromDIP(wxSize(135,-1)));step_ticks_->SetToolTip(wxString::FromUTF8("整数 ps tick 数"));p1row->Add(step_ticks_,0,wxRIGHT|wxBOTTOM,FromDIP(6));
    view_mode_=new wxChoice(phase1_controls_,wxID_ANY);for(auto label:{"自动：运行积分 / 暂停瞬时","电气瞬时态","视觉积分态"})view_mode_->Append(wxString::FromUTF8(label));view_mode_->SetSelection(0);p1row->Add(view_mode_,0,wxBOTTOM,FromDIP(6));view_mode_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){drawing_->Refresh();});
    phase1_controls_->SetSizer(p1row);layout->Add(phase1_controls_,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(8));phase1_controls_->Hide();
    drawing_=new wxPanel(this);drawing_->SetBackgroundStyle(wxBG_STYLE_PAINT);drawing_->SetMinSize(FromDIP(wxSize(320,165)));
    drawing_->Bind(wxEVT_PAINT,[this](wxPaintEvent&){wxAutoBufferedPaintDC dc(drawing_);dc.SetBackground(wxBrush(UiTheme::Palette().canvas));dc.Clear();std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));if(g)BoardRenderer::Paint(*drawing_,*g,controller_->Frame(),controller_->InputTargets(),IntegratedView());});
    drawing_->SetToolTip(wxString::FromUTF8("暂停时只暂存输入；按住按键并按空格单步，松开后再次单步提交释放。"));
    drawing_->Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent& e){if(auto dip=BoardRenderer::HitDip(*drawing_,e.GetPosition(),controller_->Frame())){const auto& component=controller_->Frame().at("board").at("peripherals").at(dip->first);SetDip(dip->first,component.at("value").get<unsigned>()^(1u<<dip->second));return;}
        // Phase 4: clicking a matrix keypad cell toggles that key closure.
        if(auto cell=BoardRenderer::HitKeypad(*drawing_,e.GetPosition(),controller_->Frame())){ToggleKeypadKey(cell->component,cell->row,cell->col);return;}
        int pad=BoardRenderer::HitButton(*drawing_,e.GetPosition(),controller_->Frame().value("phase1",false));if(pad>0 && BoardRenderer::KeySlotStatus(controller_->Frame(),pad).mounted && CanCommand(controller_->State(),"set")){held_pad_=pad;drawing_->SetFocus();drawing_->CaptureMouse();Contact(pad,true);}});
    drawing_->Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&){ReleaseContact();});drawing_->Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){ReleaseContact();});drawing_->Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){ReleaseContact();e.Skip();});
    drawing_->Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& e){if(e.GetKeyCode()==WXK_SPACE && CanCommand(controller_->State(),"step"))Step();else if(e.GetKeyCode()==WXK_ESCAPE)ReleaseContact();else e.Skip();});
    drawing_->Bind(wxEVT_RIGHT_UP,[this](wxMouseEvent& e){auto node=ResolveBoardNode(controller_->Frame(),BoardRenderer::HitNode(*drawing_,e.GetPosition(),controller_->Frame().value("phase1",false)));if(!node.empty())InspectNode(node);});
    drawing_->Bind(wxEVT_MOTION,[this](wxMouseEvent& e){auto node=ResolveBoardNode(controller_->Frame(),BoardRenderer::HitNode(*drawing_,e.GetPosition(),controller_->Frame().value("phase1",false)));if(!node.empty()){const auto& state=controller_->Frame().at("board").at("nodes").at(node);drawing_->SetToolTip(wxString::FromUTF8(node+" / STATE="+state.at("state_code").dump()+" / 右键查看逐步折叠与来源"));}e.Skip();});
    layout->Add(drawing_,1,wxEXPAND);status_=new wxStaticText(this,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_END|wxST_NO_AUTORESIZE);status_->SetMinSize(wxSize(0,FromDIP(20)));layout->Add(status_,0,wxEXPAND|wxALL,FromDIP(6));
    diagnostics_=new wxListCtrl(this,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(-1,88)),wxLC_REPORT|wxLC_SINGLE_SEL);int col=0;for(auto title:{"错误码","PortBitRef","物理脚","Bank / VCCIO","修复建议"})diagnostics_->InsertColumn(col++,wxString::FromUTF8(title));
    diagnostics_->Bind(wxEVT_LIST_ITEM_ACTIVATED,[this](wxListEvent& e){controller_->LocateDiagnostic(size_t(e.GetIndex()));});layout->Add(diagnostics_,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(6));diagnostics_->Hide();
    detail_=new wxTextCtrl(this,wxID_ANY,"",wxDefaultPosition,FromDIP(wxSize(-1,90)),wxTE_MULTILINE|wxTE_READONLY|wxTE_DONTWRAP);layout->Add(detail_,0,wxEXPAND|wxALL,FromDIP(6));SetSizer(layout);
    subscription_=controller_->Subscribe([this]{Update();});theme_=UiTheme::Subscribe([this]{ApplyTheme();});ApplyTheme();Update();
}
VirtualBoardPanel::~VirtualBoardPanel(){controller_->Unsubscribe(subscription_);}
void VirtualBoardPanel::LoadExample(const std::string& id){
    const auto& items=Examples();
    for(size_t i=0;i<items.size();++i){
        if(items[i].id==id){
            if(example_picker_)example_picker_->SetSelection(int(i+1));
            try{
                auto repo=VirtualProjectSessionBuilder::FindRepository();
                auto project=VirtualProjectSessionBuilder::ReadProject(repo/items[i].relative_project_path);
                project["repo_root"]=eda::platform::PathToUtf8(repo);
                if(!build_dir_override_.empty())project["build_dir"]=eda::platform::PathToUtf8(build_dir_override_);
                controller_->BuildProject(std::move(project));
            }catch(const std::exception& e){detail_->SetValue(wxString::FromUTF8(e.what()));}
            return;
        }
    }
    throw std::runtime_error("E_UNKNOWN_EXAMPLE: "+id);
}
std::string VirtualBoardPanel::SelectedExampleId()const{
    if(!example_picker_)return {};
    int sel=example_picker_->GetSelection();
    if(sel>=1 && size_t(sel-1)<Examples().size())return Examples().at(size_t(sel-1)).id;
    return {};
}
void VirtualBoardPanel::SetFloatOutHandler(std::function<void()> f){
    float_out_handler_=std::move(f);
    if(float_out_handler_ && !float_out_ && toolbar_){
        float_out_=new wxButton(this,wxID_ANY,wxString::FromUTF8("拖出独立窗口"));
        float_out_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){if(float_out_handler_)float_out_handler_();});
        const auto& p=UiTheme::Palette();float_out_->SetForegroundColour(p.text1);float_out_->SetBackgroundColour(p.surfaceOnPanel);float_out_->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));
        toolbar_->Add(float_out_,0,wxRIGHT|wxBOTTOM,FromDIP(5));Layout();
    } else if(!float_out_handler_ && float_out_)float_out_->Hide();
}
void VirtualBoardPanel::InvokeControl(const std::string& op){auto* b=controls_.at(op);if(!b->IsEnabled())throw std::runtime_error("E_DISABLED_CONTROL: "+op);wxCommandEvent e(wxEVT_BUTTON,b->GetId());e.SetEventObject(b);b->GetEventHandler()->ProcessEvent(e);}
std::string VirtualBoardPanel::DetailText()const{return detail_->GetValue().ToStdString(wxConvUTF8);}
void VirtualBoardPanel::SetStepMode(const std::string& mode,const std::string& ticks){if(mode!="rising" && mode!="next_event" && mode!="ticks")throw std::runtime_error("E_STEP_MODE");step_mode_->SetSelection(mode=="rising"?0:mode=="next_event"?1:2);step_ticks_->ChangeValue(wxString::FromUTF8(ticks));}
void VirtualBoardPanel::Step(){eda::Json command={{"op","step"}};if(controller_->Frame().value("phase1",false)){static const char* modes[]={"rising","next_event","ticks"};command["mode"]=modes[step_mode_->GetSelection()];if(step_mode_->GetSelection()==2)command["ticks"]=step_ticks_->GetValue().ToStdString(wxConvUTF8);}controller_->Queue(std::move(command));}
void VirtualBoardPanel::SetIntegratedView(bool value){view_mode_->SetSelection(value?2:1);drawing_->Refresh();}
bool VirtualBoardPanel::IntegratedView()const{return view_mode_ && (view_mode_->GetSelection()==2 || (view_mode_->GetSelection()==0 && controller_->State()==SessionState::Running));}
void VirtualBoardPanel::SetDip(const std::string& id,unsigned value){controller_->Queue({{"op","dip.set"},{"target",id},{"value",value}});}
eda::Json VirtualBoardPanel::KeypadKeys(const std::string& id)const{
    eda::Json keys=eda::Json::array();
    if(!controller_->Frame().contains("board") || !controller_->Frame().at("board").contains("peripherals"))return keys;
    const auto& peripherals=controller_->Frame().at("board").at("peripherals");
    if(!peripherals.contains(id) || !peripherals.at(id).contains("closed"))return keys;
    for(const auto& cell:peripherals.at(id).at("closed"))if(cell.is_array() && cell.size()==2)
        keys.push_back(eda::Json::array({std::stoi(cell.at(0).get<std::string>()),std::stoi(cell.at(1).get<std::string>())}));
    return keys;
}
void VirtualBoardPanel::SetKeys(const std::string& id,const eda::Json& keys){controller_->Queue({{"op","keypad.set"},{"target",id},{"keys",keys}});}
void VirtualBoardPanel::ToggleKeypadKey(const std::string& id,unsigned row,unsigned col){
    auto keys=KeypadKeys(id);eda::Json next=eda::Json::array();bool present=false;
    for(const auto& key:keys){if(key.at(0)==int(row) && key.at(1)==int(col)){present=true;continue;}next.push_back(key);}
    if(!present)next.push_back(eda::Json::array({int(row),int(col)}));
    SetKeys(id,next);drawing_->Refresh();
}
eda::Json VirtualBoardPanel::InspectNode(const std::string& node,bool popup){const auto& state=controller_->Frame().at("board").at("nodes").at(node);if(popup){auto* dialog=new wxDialog(this,wxID_ANY,wxString::FromUTF8("STATE 逐步解释 / "+node),wxDefaultPosition,FromDIP(wxSize(720,500)),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);auto* layout=new wxBoxSizer(wxVERTICAL);auto* text=new wxTextCtrl(dialog,wxID_ANY,wxString::FromUTF8(state.dump(2)),wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY|wxTE_DONTWRAP);text->SetForegroundColour(UiTheme::Palette().text1);text->SetBackgroundColour(UiTheme::Palette().canvas);layout->Add(text,1,wxEXPAND|wxALL,FromDIP(8));auto* close=new wxButton(dialog,wxID_CLOSE,wxString::FromUTF8("关闭"));layout->Add(close,0,wxALIGN_RIGHT|wxALL,FromDIP(8));close->Bind(wxEVT_BUTTON,[dialog](wxCommandEvent&){dialog->Destroy();});dialog->SetSizer(layout);inspections_.push_back(wxWeakRef<wxDialog>(dialog));dialog->Show();}return state;}
void VirtualBoardPanel::Contact(int pad,bool pressed){auto it=controller_->InputTargets().find(pad);if(it!=controller_->InputTargets().end())controller_->Queue({{"op","set"},{"target",it->second},{"state",pressed?13:2}});}
void VirtualBoardPanel::ReleaseContact(){int pad=held_pad_;held_pad_=-1;if(drawing_->HasCapture())drawing_->ReleaseMouse();if(pad>0)Contact(pad,false);}
void VirtualBoardPanel::ApplyTheme(){auto& p=UiTheme::Palette();SetBackgroundColour(p.bgPanel);for(auto* w:std::vector<wxWindow*>{status_,detail_,diagnostics_}){w->SetForegroundColour(p.text2);w->SetBackgroundColour(p.surfaceOnPanel);w->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}
    for(const auto& [op,b]:controls_){b->SetForegroundColour(p.text1);b->SetBackgroundColour(p.surfaceOnPanel);b->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}
    if(example_picker_){example_picker_->SetForegroundColour(p.text1);example_picker_->SetBackgroundColour(p.surfaceOnPanel);example_picker_->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}
    if(float_out_){float_out_->SetForegroundColour(p.text1);float_out_->SetBackgroundColour(p.surfaceOnPanel);float_out_->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}
    phase1_controls_->SetBackgroundColour(p.bgPanel);for(auto* window:std::vector<wxWindow*>{step_mode_,view_mode_,step_ticks_}){window->SetForegroundColour(p.text1);window->SetBackgroundColour(p.surfaceOnPanel);window->SetFont(UiTheme::UiFont(this,UiTheme::TextSize::P12));}drawing_->Refresh();}
void VirtualBoardPanel::Update(){
    auto currentRun=controller_->Frame().value("run_id",std::string());if(currentRun!=inspection_run_){for(auto& dialog:inspections_)if(dialog)dialog->Destroy();inspections_.clear();inspection_run_=std::move(currentRun);}
    phase1_controls_->Show(controller_->Frame().value("phase1",false));
    bool canLoad=controller_->State()!=SessionState::Running && controller_->State()!=SessionState::Building;
    for(const auto& [op,b]:controls_){bool enabled=CanCommand(controller_->State(),op);if(op=="load" || op=="current" || op=="reference" || op=="reference-p1" || op=="reference-p2")enabled=canLoad;
        if(op=="replay" || op=="replay-file")enabled=CanCommand(controller_->State(),"replay.current") && controller_->ReplayResult().value("status","")!="running";b->Enable(enabled);}
    if(example_picker_)example_picker_->Enable(canLoad);
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
