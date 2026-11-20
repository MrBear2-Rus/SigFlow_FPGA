#include "offscreen_capture.hpp"
#include <wx/display.h>
int RunPhase3Gui(int argc,char** argv){
    repo=eda::platform::PathFromUtf8(argv[1]);build=eda::platform::PathFromUtf8(argv[2]);work=repo/".work/p3/gui";fs::create_directories(work);progress.open(work/"progress.log");report={{"status","running"},{"checks",Json::array()}};
    wxApp::SetInstance(new AcceptanceApp());if(!wxEntryStart(argc,argv))return 3;wxTheApp->CallOnInit();wxInitAllImageHandlers();wxLog::SetActiveTarget(new wxLogStderr());wxSetAssertHandler([](const wxString& f,int line,const wxString&,const wxString& condition,const wxString& message){std::cerr<<f<<':'<<line<<' '<<condition<<' '<<message<<std::endl;std::abort();});
    wxEventLoop loop;wxEventLoopActivator active(&loop);wxFrame* frame=nullptr;std::shared_ptr<lab::VirtualSessionController> c;int result=0;
    try{c=std::make_shared<lab::VirtualSessionController>();frame=new wxFrame(nullptr,wxID_ANY,"SigFlow Phase 3 real GUI acceptance");auto* layout=new wxBoxSizer(wxVERTICAL);auto* tabs=new wxNotebook(frame,wxID_ANY);auto* board=new lab::VirtualBoardPanel(tabs,c);auto* faults=new lab::FaultInjectionPanel(tabs,c);auto* waves=new wave::TraceViewPanel(tabs);auto* teaching=new lab::VirtualTeachingPanel(tabs,c,board,waves);auto* uart=new lab::VirtualUartPanel(tabs,c);
        tabs->AddPage(board,wxString::FromUTF8("虚拟实验箱"));tabs->AddPage(faults,wxString::FromUTF8("实验故障"));tabs->AddPage(teaching,wxString::FromUTF8("教学与虚拟 TraceBridge"));tabs->AddPage(waves,"Waveform");tabs->AddPage(uart,wxString::FromUTF8("虚拟串口"));layout->Add(tabs,1,wxEXPAND);frame->SetSizer(layout);frame->SetClientSize({1440,960});frame->Show();frame->Layout();
        auto subscription=c->Subscribe([&]{if(auto source=c->Source())waves->FollowLiveTrace(source);});
        auto project=lab::VirtualProjectSessionBuilder::ReadProject(repo/"examples/virtual/courses/course-01-open-wire/virtual.project.json");project["repo_root"]=Path(repo);project["build_dir"]=Path(build);
        auto profile=Read(repo/"examples/virtual/courses/course-01-open-wire/faults.private.json");profile["faults"]=Json::array();project["fault_profile"]=profile;board->SetProjectProvider([&]{return project;});board->InvokeControl("current");
        Until([&]{return c->State()!=lab::SessionState::Building;},*c);Until([&]{return c->State()==lab::SessionState::Ready && bool(c->Source());},*c);
        // ---- Module A1: independent floating window, 20 pop-out/return cycles
        // on a live session. Only the view host migrates; the persistent
        // session controller, its LiveTraceSource and every subscription survive.
        lab::VirtualBoardFloatFrame* floatFrame=nullptr;
        auto popOut=[&]{if(floatFrame){floatFrame->Raise();return;}floatFrame=lab::VirtualBoardFloatFrame::PopOut(tabs,board,wxString::FromUTF8("SigFlow · 虚拟实验箱"));if(floatFrame)floatFrame->SetReturnHandler([&]{floatFrame=nullptr;});};
        board->SetFloatOutHandler(popOut);
        auto stepRising=[&]{auto seq=c->ReplySequence("step");auto tickBefore=c->Frame().at("position").at("tick");board->SetStepMode("rising");board->InvokeControl("step");Until([&]{return c->ReplySequence("step")>seq && c->Frame().at("position").at("tick")!=tickBefore;},*c);};
        board->SetContact(4,false);board->SetContact(3,true);stepRising();
        auto runBefore=c->Frame().value("run_id",std::string());auto sourceBefore=c->Source();
        auto writesBefore=c->Frame().value("input_writes",std::string());bool monotonic=true;
        auto findPage=[&](const wxString& needle){for(size_t i=0;i<tabs->GetPageCount();++i)if(tabs->GetPageText(i).Contains(needle))return int(i);return -1;};
        for(int cycle=0;cycle<20;++cycle){
            popOut();for(int n=0;n<4;++n)Pump();
            Check(bool(floatFrame) && board->GetParent()==floatFrame,"P3","cycle "+std::to_string(cycle)+" pops the live panel into the independent frame");
            int slot=findPage(wxString::FromUTF8("已拖出"));
            Check(slot>=0 && tabs->GetPage(size_t(slot))!=board && tabs->FindPage(board)==wxNOT_FOUND,"P3","main notebook shows the spec placeholder page while popped out");
            auto requested=floatFrame->FromDIP(wxSize(880,620));
            Check(floatFrame->GetClientSize().x>=requested.x-2 && floatFrame->GetClientSize().y>=requested.y-2,"P3","floating frame keeps the 880x620 DIP workspace");
            int display=wxDisplay::GetFromPoint(floatFrame->GetScreenPosition());
            Check(display>=0 && wxDisplay(display).GetClientArea().Contains(floatFrame->GetScreenRect()),"P3","floating frame lands fully inside one display");
            stepRising();
            Check(c->State()==lab::SessionState::Paused && c->Frame().value("run_id",std::string())==runBefore && c->Source()==sourceBefore,"P3","stepping inside the floating window keeps the persistent session and live trace source");
            monotonic&=c->Frame().at("position").at("tick")!=Json();
            floatFrame->ReturnToMain();for(int n=0;n<4;++n)Pump();
            int returned=tabs->FindPage(board);
            Check(!floatFrame && returned!=wxNOT_FOUND && board->GetParent()==tabs && findPage(wxString::FromUTF8("已拖出"))<0,"P3","cycle "+std::to_string(cycle)+" returns the panel to its original notebook page");
            stepRising();
        }
        Check(c->State()==lab::SessionState::Paused && c->Frame().value("reason",std::string())=="StepComplete" && c->Frame().value("input_writes",std::string())!=writesBefore && c->Source()==sourceBefore,"P3","20 pop-out/return cycles leave no dangling callbacks and keep the simulation alive");
        Check(std::stoull(c->Frame().at("position").at("tick").get<std::string>())>0 && monotonic,"P3","committed tick strictly advances across every migration");
        // Duplicate pop-out attempts only raise the existing frame.
        popOut();auto* firstFrame=floatFrame;popOut();Check(floatFrame==firstFrame,"P3","a second pop-out request raises the existing floating window instead of forking the view");
        firstFrame->ReturnToMain();for(int n=0;n<3;++n)Pump();
        report["float_cycles"]=20;report["float_frame"]="VirtualBoardFloatFrame + placeholder page + shared controller";
        // ---- Module A3: teaching agent bridge over the desensitized evidence.
        teaching->StartCourse("course-01-open-wire");Until([&]{return c->State()!=lab::SessionState::Building;},*c,false,300);Until([&]{return c->State()==lab::SessionState::Ready && c->Frame().value("mode",std::string())=="student_blind_test";},*c);
        Check(faults->IsBlindLocked() && !c->Frame().contains("faults") && !c->Frame().contains("p1"),"P3","blind course locks private controls before any agent traffic");
        teaching->MeasurePair("pad.10","led0.K");Until([&]{return c->LatestReply("virtual.evidence.query").value("measurements",Json::array()).size()>=2;},*c);
        auto seq=c->ReplySequence("teaching.hint.request");teaching->RequestHint("L4");Until([&]{return c->ReplySequence("teaching.hint.request")>seq;},*c);
        Check(c->LatestReply("teaching.hint.request").at("status")=="rejected","P3","L4 remains gated before lower levels");
        for(auto level:{"L1","L2","L3"}){seq=c->ReplySequence("teaching.hint.request");teaching->RequestHint(level);Until([&]{return c->ReplySequence("teaching.hint.request")>seq;},*c);}
        teaching->MeasurePair("pad.10","led0.K");Until([&]{return c->LatestReply("virtual.evidence.query").value("measurements",Json::array()).size()>=4;},*c);
        auto submit=[&](Json cause,Json repair){auto before=c->ReplySequence("teaching.grade.submit");teaching->SubmitGrade(cause,repair);Until([&]{return c->ReplySequence("teaching.grade.submit")>before;},*c,false,300);return c->LatestReply("teaching.grade.submit");};
        auto passed=submit({{"location","wire.led0"},{"type","open"}},{{"restore","wire.led0"}});
        Check(passed.at("status")=="pass" && passed.at("score")==100 && passed.at("seeds_tested")==3,"P3","real repair passes the deterministic three-seed grading");
        Json captured;bool bridged=false;
        teaching->SetAgentBridge([&](const Json& payload){captured=payload;bridged=true;return Json{{"status","submitted"},{"channel","test-host"}};});
        teaching->SendToAgent();
        Check(bridged && captured.at("kind")=="virtual.evidence.snapshot" && captured.at("measurements").size()>=2,"P3","agent bridge receives the live desensitized evidence snapshot with dual probe readings");
        Check(captured.contains("wave_first_divergence") && captured.contains("session") && captured.at("session").at("mode")=="student_blind_test","P3","bridge snapshot carries session facts and capture provenance");
        auto bridgeText=captured.dump()+teaching->OutputText();
        for(auto secret:{"secret-open-4a7","fault_id","rubric","hidden-","faults"})
            Check(bridgeText.find(secret)==std::string::npos,"P3","agent evidence snapshot leaks no "+std::string(secret));
        Check(teaching->OutputText().find("assistant_reply")!=std::string::npos,"P3","assistant channel reply is shown transparently next to the evidence");
        teaching->SetAgentBridge(nullptr);
        bool graceful=false;try{teaching->SendToAgent();}catch(const std::exception& e){graceful=std::string(e.what()).find("E_AGENT_BRIDGE_UNAVAILABLE")!=std::string::npos;}
        Check(graceful,"P3","missing agent bridge fails gracefully with an explicit error");
        auto publicJson=c->Frame().dump()+teaching->OutputText()+faults->DetailText()+c->LatestReply("virtual.evidence.query").dump();
        for(auto secret:{"secret-open-4a7","9500123","fault_id","rubric"})Check(publicJson.find(secret)==std::string::npos,"P3","controller and visible text contain no secret rule, seed or private rubric");
        Write(work/"agent-evidence.json",captured);
        // ---- Module A2: fixed offscreen capture of the native teaching panel.
        for(auto mode:{UiTheme::Mode::Light,UiTheme::Mode::Dark}){UiTheme::SetMode(mode);tabs->SetSelection(2);frame->Layout();for(int n=0;n<6;++n)Pump();
            wxBitmap snapshot;Check(sigflow_acceptance::CaptureWindow(teaching,snapshot),"P3","PrintWindow(PW_RENDERFULLCONTENT) renders the whole teaching client");
            Check(sigflow_acceptance::DistinctColors(snapshot)>=8,"P3","native child controls paint real content instead of blank boxes");
            auto region=teaching->ReadoutRegion();
            Check(sigflow_acceptance::DistinctColorsInRegion(snapshot,region)>=6,"P3","readout region shows hint levels, probe readings and grading text pixels");
            auto name=mode==UiTheme::Mode::Light?"teaching-light.png":"teaching-dark.png";
            Check(snapshot.SaveFile(wxString::FromUTF8(Path(work/name)),wxBITMAP_TYPE_PNG),"P3","exports the fixed native teaching snapshot "+std::string(name));}
        c->Unsubscribe(subscription);c->Shutdown();delete frame;frame=nullptr;c.reset();for(int n=0;n<5;++n)Pump();report["status"]="passed";report["host"]="real wxApp + VirtualBoardFloatFrame + VirtualTeachingPanel agent bridge + trusted blind course + fixed offscreen capture";
    }catch(const std::exception& e){report["status"]="failed";report["error"]=e.what();std::cerr<<e.what()<<std::endl;result=1;if(c)c->Shutdown();if(frame)delete frame;c.reset();}
    report["check_count"]=report["checks"].size();Write(work/"gui-results.json",report);wxTheApp->OnExit();wxEntryCleanup();return result;
}
