#include "../p3/offscreen_capture.hpp"
#include <wx/display.h>
int RunPhase4Gui(int argc,char** argv){
    repo=eda::platform::PathFromUtf8(argv[1]);build=eda::platform::PathFromUtf8(argv[2]);work=repo/".work/p4/gui";fs::create_directories(work);progress.open(work/"progress.log");report={{"status","running"},{"checks",Json::array()}};
    wxApp::SetInstance(new AcceptanceApp());if(!wxEntryStart(argc,argv))return 3;wxTheApp->CallOnInit();wxInitAllImageHandlers();wxLog::SetActiveTarget(new wxLogStderr());wxSetAssertHandler([](const wxString& f,int line,const wxString&,const wxString& condition,const wxString& message){std::cerr<<f<<':'<<line<<' '<<condition<<' '<<message<<std::endl;std::abort();});
    wxEventLoop loop;wxEventLoopActivator active(&loop);wxFrame* frame=nullptr;std::shared_ptr<lab::VirtualSessionController> c;int result=0;
    try{c=std::make_shared<lab::VirtualSessionController>();frame=new wxFrame(nullptr,wxID_ANY,"SigFlow Phase 4 real GUI acceptance");auto* layout=new wxBoxSizer(wxVERTICAL);auto* tabs=new wxNotebook(frame,wxID_ANY);auto* board=new lab::VirtualBoardPanel(tabs,c);auto* faults=new lab::FaultInjectionPanel(tabs,c);auto* waves=new wave::TraceViewPanel(tabs);auto* teaching=new lab::VirtualTeachingPanel(tabs,c,board,waves);
        tabs->AddPage(board,wxString::FromUTF8("虚拟实验箱"));tabs->AddPage(faults,wxString::FromUTF8("实验故障"));tabs->AddPage(teaching,wxString::FromUTF8("教学与虚拟 TraceBridge"));tabs->AddPage(waves,"Waveform");layout->Add(tabs,1,wxEXPAND);frame->SetSizer(layout);frame->SetClientSize({1500,980});frame->Show();frame->Layout();
        auto subscription=c->Subscribe([&]{if(auto source=c->Source())waves->FollowLiveTrace(source);});
        auto project=lab::VirtualProjectSessionBuilder::ReadProject(repo/"examples/virtual/phase4/i2c-temp/virtual.project.json");project["repo_root"]=Path(repo);project["build_dir"]=Path(build);board->SetProjectProvider([&]{return project;});board->InvokeControl("current");
        Until([&]{return c->State()!=lab::SessionState::Building;},*c);Until([&]{return c->State()==lab::SessionState::Ready && bool(c->Source());},*c);
Check(c->Frame().value("phase1",false) && c->Frame().at("board").at("peripherals").contains("i2c0") && c->Frame().at("board").at("peripherals").contains("oled0") && c->Frame().at("board").at("peripherals").contains("keypad0"),"P4","Phase 4 board hosts the I2C sensor, SPI OLED and matrix keypad components");
        // ---- matrix keypad interaction through the real renderer hit test
        board->SetContact(4,false);
        auto stepTicks=[&](const std::string& ticks){auto seq=c->ReplySequence("step");board->SetStepMode("ticks",ticks);board->InvokeControl("step");Until([&]{return c->ReplySequence("step")>seq;},*c);};
        stepTicks("20000000");
        Check(c->State()==lab::SessionState::Paused,"P4","long stepped run completes in the paused state");
        auto keypadKeys=[&]{auto keys=Json::array();for(const auto& cell:c->Frame().at("board").at("peripherals").at("keypad0").at("closed"))keys.push_back(cell);return keys;};
        Check(keypadKeys().empty(),"P4","keypad starts with every key released");
        // Keypad edits while Paused stay pending until the next committed step.
        auto toggleAndApply=[&](unsigned row,unsigned col){board->ToggleKeypadKey("keypad0",row,col);Until([&]{return c->Frame().value("pending_count",size_t(0))>=1;},*c);Check(c->Frame().value("input_writes",std::string())!="" ,"P4","paused keypad edit is staged without DUT writes");stepTicks("1000000");stepTicks("1000000");};
        toggleAndApply(0,0);
        Check(keypadKeys().size()==1 && keypadKeys().at(0)==Json::array({"0","0"}),"P4","renderer click path closes key (0,0) through keypad.set");
        toggleAndApply(2,3);
        Check(keypadKeys().size()==2,"P4","a second click closes key (2,3) alongside the first");
        toggleAndApply(0,0);
        Check(keypadKeys().size()==1 && keypadKeys().at(0)==Json::array({"2","3"}),"P4","clicking a closed key releases it and keeps the other key");
        // ---- I2C badge + OLED dot matrix are non-empty in the rendered tray
        project=lab::VirtualProjectSessionBuilder::ReadProject(repo/"examples/virtual/phase4/spi-oled/virtual.project.json");project["repo_root"]=Path(repo);project["build_dir"]=Path(build);board->InvokeControl("current");
        Until([&]{return c->State()!=lab::SessionState::Building;},*c);Until([&]{return c->State()==lab::SessionState::Ready && bool(c->Source());},*c);
        board->SetContact(4,false);stepTicks("34000000");
        const auto& lit=c->Frame().at("board").at("peripherals").at("oled0");
        Check(lit.value("display_on",false) && lit.value("lit_pixels",0u)==28 && lit.value("frame_sha256",std::string()).size()==64,"P4","OLED dot matrix snapshot carries display state, lit pixel count and VRAM digest");
        Check(c->Frame().at("board").at("peripherals").contains("i2c0"),"P4","I2C sensor badge state stays available on the Phase 4 board");
        // ---- fixed offscreen export of the workbench with OLED + keypad
        for(auto mode:{UiTheme::Mode::Light,UiTheme::Mode::Dark}){UiTheme::SetMode(mode);tabs->SetSelection(0);board->Layout();frame->Layout();for(int n=0;n<6;++n)Pump();
            wxBitmap snapshot;Check(sigflow_acceptance::CaptureWindow(board,snapshot),"P4","PW_RENDERFULLCONTENT renders the workbench including the external tray");
            Check(sigflow_acceptance::DistinctColors(snapshot)>=10,"P4","workbench snapshot carries real themed pixels");
            auto name=mode==UiTheme::Mode::Light?"board-oled-keypad-light.png":"board-oled-keypad-dark.png";
            Check(snapshot.SaveFile(wxString::FromUTF8(Path(work/name)),wxBITMAP_TYPE_PNG),"P4","exports "+std::string(name));}
        // ---- module D: course 05 blind closed loop
        teaching->StartCourse("course-05-i2c-nak-troubleshooting");Until([&]{return c->State()!=lab::SessionState::Building;},*c,false,300);Until([&]{return c->State()==lab::SessionState::Ready && c->Frame().value("mode",std::string())=="student_blind_test";},*c);
        Check(faults->IsBlindLocked() && !c->Frame().contains("faults") && !c->Frame().contains("p1"),"P4","course 05 locks private controls and never ships the runtime checkpoint");
        teaching->MeasurePair("pad.35","pad.36");Until([&]{return c->LatestReply("virtual.evidence.query").value("measurements",Json::array()).size()>=2;},*c);
        auto measurements=c->LatestReply("virtual.evidence.query").at("measurements");
        Check(measurements.at(0).at("target")=="pad.35" && measurements.at(1).at("target")=="pad.36","P4","blind dual probe measures SCL and SDA on the Phase 4 pins");
        auto seq=c->ReplySequence("teaching.hint.request");teaching->RequestHint("L4");Until([&]{return c->ReplySequence("teaching.hint.request")>seq;},*c);
        Check(c->LatestReply("teaching.hint.request").at("status")=="rejected","P4","course 05 cannot skip straight to L4");
        for(auto level:{"L1","L2","L3"}){seq=c->ReplySequence("teaching.hint.request");teaching->RequestHint(level);Until([&]{return c->ReplySequence("teaching.hint.request")>seq;},*c);Check(c->LatestReply("teaching.hint.request").at("level")==level,"P4",std::string("course 05 unlocks ")+level);}
        teaching->MeasurePair("pad.35","pad.36");Until([&]{return c->LatestReply("virtual.evidence.query").value("measurements",Json::array()).size()>=4;},*c);
        seq=c->ReplySequence("teaching.hint.request");teaching->RequestHint("L4");Until([&]{return c->ReplySequence("teaching.hint.request")>seq;},*c);
        Check(c->LatestReply("teaching.hint.request").at("level")=="L4","P4","course 05 authorized L4 uses the prior evidence ledger");
        auto submit=[&](Json cause,Json repair){auto before=c->ReplySequence("teaching.grade.submit");teaching->SubmitGrade(cause,repair);Until([&]{return c->ReplySequence("teaching.grade.submit")>before;},*c,false,600);return c->LatestReply("teaching.grade.submit");};
        auto wrong=submit({{"location","pad.99"},{"type","i2c_nak"}},{{"restore","i2c-sda-pullup"}});
        Check(wrong.at("status")=="fail","P4","course 05 wrong location fails despite a correct repair claim");
        auto unrepaired=submit({{"location","i2c0"},{"type","i2c_nak"}},Json::object());
        Check(unrepaired.at("status")=="fail","P4","course 05 correct location without restoration still fails");
        auto passed=submit({{"location","i2c0"},{"type","i2c_nak"}},{{"restore","i2c-sda-pullup"}});
        Check(passed.at("status")=="pass" && passed.at("score")==100 && passed.at("seeds_tested")==3,"P4","course 05 genuine repair runs three hidden seeds and passes deterministically");
        report["grade"]=passed;
        Json captured;bool bridged=false;
        teaching->SetAgentBridge([&](const Json& payload){captured=payload;bridged=true;return Json{{"status","submitted"},{"channel","test-host"}};});
        teaching->SendToAgent();
        Check(bridged && captured.at("kind")=="virtual.evidence.snapshot" && captured.at("measurements").size()>=2,"P4","course 05 evidence snapshot reaches the AI assistant bridge");
        auto bridgeText=captured.dump()+teaching->OutputText()+c->Frame().dump();
        for(auto secret:{"secret-nak-7f4","fault_id","rubric","hidden_seeds","9500123"})
            Check(bridgeText.find(secret)==std::string::npos,"P4","course 05 leaks no "+std::string(secret));
        teaching->SetAgentBridge(nullptr);
        Write(work/"course05-evidence.json",captured);Write(work/"course05-blind-frame.json",c->Frame());
        c->Unsubscribe(subscription);c->Shutdown();delete frame;frame=nullptr;c.reset();for(int n=0;n<5;++n)Pump();report["status"]="passed";report["host"]="real wxApp + Phase 4 board + keypad/OLED/I2C rendering + course 05 trusted blind loop";
    }catch(const std::exception& e){report["status"]="failed";report["error"]=e.what();std::cerr<<e.what()<<std::endl;result=1;if(c)c->Shutdown();if(frame)delete frame;c.reset();}
    report["check_count"]=report["checks"].size();Write(work/"gui-results.json",report);wxTheApp->OnExit();wxEntryCleanup();return result;
}
