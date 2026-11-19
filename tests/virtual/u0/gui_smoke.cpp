#include "virtual/VirtualSessionController.h"
#include "virtual/VirtualBoardPanel.h"
#include "virtual/FaultInjectionPanel.h"
#include "virtual/BoardRenderer.h"
#include "eda-platform/Platform.h"
#include <wx/app.h>
#include <wx/frame.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/evtloop.h>
#include <wx/log.h>
#include <wx/debug.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <thread>
using eda::Json;namespace lab=sigflow::virtual_lab;namespace fs=std::filesystem;
namespace {
class SmokeApp final:public wxApp {public:bool OnInit()override{return true;}};
std::ofstream progress;
int checks=0;void Check(bool yes,const std::string& msg){++checks;progress<<checks<<": "<<msg<<std::endl;if(!yes)throw std::runtime_error(msg);}
template<class F>void Until(F predicate,lab::VirtualSessionController& c){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(!predicate()){
        wxEventLoopBase::GetActive()->DispatchTimeout(5);wxTheApp->ProcessPendingEvents();
        if(c.State()==lab::SessionState::Error)throw std::runtime_error(c.Error());
        if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("GUI event-loop timeout");
    }wxTheApp->ProcessPendingEvents();
}
void Draw(wxPanel& w,const Json& frame,const fs::path& path,double scale){
    w.SetClientSize(w.FromDIP(wxSize(820,360)));wxBitmap bitmap(int(w.GetClientSize().x*scale),int(w.GetClientSize().y*scale));wxMemoryDC dc(bitmap);
    dc.SetBackground(wxBrush(UiTheme::Palette().canvas));dc.Clear();std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));g->Scale(scale,scale);
    lab::BoardRenderer::Paint(w,*g,frame,{{3,"key_p3"},{4,"reset_p4"}});g.reset();dc.SelectObject(wxNullBitmap);Check(bitmap.SaveFile(wxString::FromUTF8(eda::platform::PathToUtf8(path)),wxBITMAP_TYPE_PNG),"rendered board PNG");
}
}
int main(int argc,char** argv){
    if(argc!=5)return 2;auto root=eda::platform::PathFromUtf8(argv[1]),work=eda::platform::PathFromUtf8(argv[2]);fs::create_directories(work);
    progress.open(work/"progress.log");progress<<"wx init"<<std::endl;
    wxApp::SetInstance(new SmokeApp());progress<<"entry start"<<std::endl;if(!wxEntryStart(argc,argv))return 3;
    progress<<"call init"<<std::endl;wxTheApp->CallOnInit();progress<<"image handlers"<<std::endl;wxInitAllImageHandlers();
    wxLog::SetActiveTarget(new wxLogStderr());
    wxSetAssertHandler([](const wxString& file,int line,const wxString&,const wxString& condition,const wxString& message){
        std::cerr<<"wx ASSERT "<<file.ToStdString()<<':'<<line<<' '<<condition.ToStdString()<<' '<<message.ToStdString()<<std::endl;std::abort();});
    progress<<"event loop"<<std::endl;wxEventLoop loop;wxEventLoopActivator active(&loop);int result=0;wxFrame* frame=nullptr;
    try{
        progress<<"controller construct"<<std::endl;auto controller=std::make_shared<lab::VirtualSessionController>();
        progress<<"frame construct"<<std::endl;frame=new wxFrame(nullptr,wxID_ANY,"SigFlow U0 smoke");frame->SetClientSize(frame->FromDIP(wxSize(840,520)));
        progress<<"board panel construct"<<std::endl;auto* board=new lab::VirtualBoardPanel(frame,controller);
        progress<<"fault panel construct"<<std::endl;auto* faults=new lab::FaultInjectionPanel(frame,controller);faults->Hide();
        Check(controller->State()==lab::SessionState::Unbuilt && !lab::CanCommand(controller->State(),"play"),"Unbuilt controls disabled");
        controller->Open(wxString::FromUTF8(argv[3]),wxString::FromUTF8(argv[4]),wxString::FromUTF8(eda::platform::PathToUtf8(root)));
        Check(controller->State()==lab::SessionState::Building,"Building state emitted");Until([&]{return controller->State()==lab::SessionState::Ready && bool(controller->Source());},*controller);
        Check(lab::CanCommand(controller->State(),"step") && !lab::CanCommand(controller->State(),"pause"),"Ready control mapping");
        controller->Queue({{"op","set"},{"target","reset_p4"},{"state",2}});controller->Queue({{"op","set"},{"target","key_p3"},{"state",13}});
        Until([&]{return controller->Frame().value("pending_count",size_t(0))==2;},*controller);
        Check(controller->Frame().at("input_writes")=="3","paused GUI inputs pending without writes");
        std::thread caller([&]{controller->Queue({{"op","step"}});});caller.join();
        Until([&]{return controller->State()==lab::SessionState::Paused && controller->Frame().at("reason")=="StepComplete";},*controller);
        Check(controller->Frame().at("board").at("leds").at("led0").at("lit")==true,"GUI real RTL state reaches board renderer");
        auto held=controller->Source()->CurrentReadView();Draw(*board,controller->Frame(),work/"u0-board-light.png",1);
        controller->Queue({{"op","fault.apply"},{"fault_id","ui.led0.open"},{"edge_id","wire.led0"}});
        Until([&]{return controller->Frame().at("pending_faults").size()==1;},*controller);
        Check(controller->Frame().at("board").at("wires").at("wire.led0")==true,"GUI pending fault distinct from active");
        Draw(*board,controller->Frame(),work/"u0-board-pending.png",1);
        controller->Queue({{"op","step"}});Until([&]{return controller->Frame().at("pending_faults").empty() && controller->Frame().at("board").at("wires").at("wire.led0")==false;},*controller);
        auto revision=controller->Frame().at("trace").at("revision").get<std::string>();Until([&]{return std::to_string(controller->Source()->CurrentReadView().Revision())==revision;},*controller);
        Check(controller->Source()->CurrentReadView().Revision()>held.Revision() && held.Valid(),"UI mirror follows complete trace revisions and retains old view");
        UiTheme::SetMode(UiTheme::Mode::Dark);Draw(*board,controller->Frame(),work/"u0-board-open-dark-150.png",1.5);UiTheme::SetMode(UiTheme::Mode::Light);
        Check(lab::ClassifyNode({{"state_code",2}})==lab::NodeVisual::Floating && lab::ClassifyNode({{"state_code",44},{"contention",true}})==lab::NodeVisual::Contention,"FLT and CON distinct render classes");
        controller->Queue({{"op","fault.revoke"},{"fault_id","ui.led0.open"}});controller->Queue({{"op","step"}});
        Until([&]{return controller->Frame().at("pending_faults").empty() && controller->Frame().at("board").at("wires").at("wire.led0")==true;},*controller);
        controller->Queue({{"op","play"}});Until([&]{return controller->State()==lab::SessionState::Running;},*controller);
        controller->Queue({{"op","pause"}});Until([&]{return controller->State()==lab::SessionState::Paused && controller->Frame().at("reason")=="User";},*controller);
        Check(lab::CanCommand(controller->State(),"step"),"Play/Pause state round trip");
        auto stopStart=std::chrono::steady_clock::now();controller->Queue({{"op","stop"}});Until([&]{return controller->State()==lab::SessionState::Stopped;},*controller);
        Check(std::chrono::steady_clock::now()-stopStart<std::chrono::seconds(5),"Stop bypasses historical trace mirror backlog");
        Check(!lab::CanCommand(controller->State(),"play") && held.Valid(),"Stopped controller and retained view");
        for(const auto& name:{"BoardRenderer.cpp","VirtualBoardPanel.cpp","FaultInjectionPanel.cpp"}){
            std::ifstream input(root/"main/virtual"/name);std::string code((std::istreambuf_iterator<char>(input)),{});
            Check(code.find("UiTheme::Palette")!=std::string::npos && code.find("FromDIP")!=std::string::npos && !std::regex_search(code,std::regex("wxColour\\s*\\(\\s*[0-9]|#[0-9A-Fa-f]{6}")),"DIP/token guard");
        }
        std::cout<<"PASS "<<checks<<" U0 checks: real child runner, UI-thread callbacks, pending/applied, controls, fault mirror, token/DIP rendering\n";
        std::ofstream(work/"gui-results.json")<<Json{{"status","passed"},{"checks",checks},{"renderer","wxGraphicsContext / UiTheme tokens / FromDIP"},{"raster_scales",{1.0,1.5}},{"real_monitor_dpi_test",false}}.dump(2)<<'\n';
        controller->Shutdown();delete frame;frame=nullptr;controller.reset();wxTheApp->ProcessPendingEvents();
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';result=1;if(frame){delete frame;frame=nullptr;}}
    wxTheApp->OnExit();wxEntryCleanup();return result;
}
