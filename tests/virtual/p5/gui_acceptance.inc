#include "../p3/offscreen_capture.hpp"
#include <wx/display.h>
int RunPhase5Gui(int argc,char** argv){
    repo=eda::platform::PathFromUtf8(argv[1]);build=eda::platform::PathFromUtf8(argv[2]);work=repo/".work/p5/gui";fs::create_directories(work);progress.open(work/"progress.log");report={{"status","running"},{"checks",Json::array()}};
    wxApp::SetInstance(new AcceptanceApp());if(!wxEntryStart(argc,argv))return 3;wxTheApp->CallOnInit();wxInitAllImageHandlers();wxLog::SetActiveTarget(new wxLogStderr());wxSetAssertHandler([](const wxString& f,int line,const wxString&,const wxString& condition,const wxString& message){std::cerr<<f<<':'<<line<<' '<<condition<<' '<<message<<std::endl;std::abort();});
    wxEventLoop loop;wxEventLoopActivator active(&loop);wxFrame* frame=nullptr;std::shared_ptr<lab::VirtualSessionController> c;int result=0;
    try{
        c=std::make_shared<lab::VirtualSessionController>();
        frame=new wxFrame(nullptr,wxID_ANY,"SigFlow Phase 5 real GUI acceptance");
        auto* layout=new wxBoxSizer(wxVERTICAL);auto* tabs=new wxNotebook(frame,wxID_ANY);
        auto* board=new lab::VirtualBoardPanel(tabs,c);
        auto* faults=new lab::FaultInjectionPanel(tabs,c);
        auto* waves=new wave::TraceViewPanel(tabs);
        auto* teaching=new lab::VirtualTeachingPanel(tabs,c,board,waves);
        auto* uart=new lab::VirtualUartPanel(tabs,c);
        tabs->AddPage(board,wxString::FromUTF8("虚拟实验箱"));
        tabs->AddPage(faults,wxString::FromUTF8("实验故障"));
        tabs->AddPage(teaching,wxString::FromUTF8("教学与虚拟 TraceBridge"));
        tabs->AddPage(waves,"Waveform");
        tabs->AddPage(uart,wxString::FromUTF8("虚拟串口"));
        layout->Add(tabs,1,wxEXPAND);frame->SetSizer(layout);frame->SetClientSize({1500,980});frame->Show();frame->Layout();

        auto subscription=c->Subscribe([&]{if(auto source=c->Source())waves->FollowLiveTrace(source);});
        board->SetBuildDirOverride(build);

        lab::VirtualBoardFloatFrame* floatFrame=nullptr;
        auto popOut=[&]{
            if(floatFrame){floatFrame->Raise();return;}
            floatFrame=lab::VirtualBoardFloatFrame::PopOut(tabs,board,wxString::FromUTF8("SigFlow · 虚拟实验箱"));
            if(floatFrame)floatFrame->SetReturnHandler([&]{floatFrame=nullptr;});
        };
        board->SetFloatOutHandler(popOut);

        auto waitReady=[&]{
            Until([&]{return c->State()!=lab::SessionState::Building;},*c,false,300);
            Until([&]{return c->State()==lab::SessionState::Ready && bool(c->Source());},*c,false,120);
        };
        auto stepTicks=[&](const std::string& ticks){
            auto seq=c->ReplySequence("step");
            board->SetStepMode("ticks",ticks);
            board->InvokeControl("step");
            Until([&]{return c->ReplySequence("step")>seq;},*c,false,240);
        };

#ifdef _WIN32
        {
            struct RestorePath {wxString value;~RestorePath(){wxSetEnv("PATH",value);}} restore{wxGetenv("PATH")};
            wxString hostile="C:\\mingw64\\bin";
            for(const auto& part:wxSplit(restore.value,';')){
                auto normalized=part.Lower();normalized.Replace("\\","/");
                if(normalized.Find("e:/sigflow_fpga")==wxNOT_FOUND)hostile+=";"+part;
            }
            wxSetEnv("PATH",hostile);
            board->InvokeControl("reference");waitReady();
            Check(c->Frame().value("reason",std::string())=="Ready","P5.2","M0 reference builds and starts with host GCC first and locked toolchain removed from parent PATH");
            auto seq=c->ReplySequence("step");board->SetStepMode("rising");board->InvokeControl("step");
            Until([&]{return c->ReplySequence("step")>seq;},*c,false,120);
            Check(c->Frame().value("reason",std::string())=="StepComplete","P5.2","runner starts and steps using the GUI child environment despite hostile parent PATH");
            report["explorer_toolchain"]={{"status","passed"},{"host_compiler_first",true},{"parent_locked_toolchain_removed",true},{"build",c->BuildResult()},{"runner_frame",c->Frame()}};
        }
#endif

        // 1. Full-stage quick launcher covers all 7 experiments M0-P5.
        Check(lab::VirtualBoardPanel::Examples().size()==7,"P5","full-stage quick launcher lists all 7 M0-P5 experiments");

        board->LoadExample("p3-keypad-scan");waitReady();
        Check(board->SelectedExampleId()=="p3-keypad-scan","P5","quick launcher loads P3 keypad scan and updates selection");
        Check(c->Frame().at("board").at("peripherals").contains("keypad0"),"P5","P3 keypad experiment mounts matrix keypad peripheral");
        Check(lab::BoardRenderer::LedSlotStatus(c->Frame(),5).mounted && lab::BoardRenderer::KeySlotStatus(c->Frame(),3).mounted,"P5","P3 board keeps LED5 and KEY P3 mounted");

        // 2. Load P4 SPI OLED and verify honest multiplexed-pin & unmounted-key rendering.
        board->LoadExample("p4-spi-oled");waitReady();
        board->SetContact(4,false);
        Until([&]{return c->Frame().value("pending_count",size_t(0))>=1;},*c);
        stepTicks("34000000");
        const auto& oled=c->Frame().at("board").at("peripherals").at("oled0");
        Check(oled.value("display_on",false) && oled.value("lit_pixels",0u)==28
            && oled.value("frame_sha256",std::string())=="307a5bc25d1638e7d825a90ae9acc7ebf9b1db1f19ab0a47e0f94dfe93733180","P5","quick-loaded P4 OLED reproduces exact 28 lit pixels and VRAM digest");

        auto led5=lab::BoardRenderer::LedSlotStatus(c->Frame(),5);
        Check(!led5.mounted && led5.multiplexed && led5.label.find("P16")!=std::string::npos
            && led5.state_text!="UND" && led5.state_text!="INVALID","P5","multiplexed P16 reports honest auxiliary state instead of LED5 UND: "+led5.label+" / "+led5.state_text);
        auto key3=lab::BoardRenderer::KeySlotStatus(c->Frame(),3);
        Check(!key3.mounted && key3.state_text=="未挂载","P5","unmounted KEY P3 reports honest 未挂载 status");
        auto key4=lab::BoardRenderer::KeySlotStatus(c->Frame(),4);
        Check(key4.mounted,"P5","mounted KEY P4 stays active");
        auto pad16Inspect=board->InspectNode("pad.16",false);
        Check(pad16Inspect.contains("state_code") && pad16Inspect.contains("explanation"),"P5","inspecting multiplexed pad.16 reveals 13-state explanation without error");

        // 3. Load P5 I2C multibyte burst read experiment via quick launcher.
        board->LoadExample("p5-i2c-burst");waitReady();
        Check(board->SelectedExampleId()=="p5-i2c-burst","P5","quick launcher switches directly to P5 I2C burst experiment");
        board->SetContact(4,false);
        Until([&]{return c->Frame().value("pending_count",size_t(0))>=1;},*c);
        stepTicks("45000000");
        const auto& i2c=c->Frame().at("board").at("peripherals").at("i2c0");
        Check(i2c.value("transactions",0u)==2 && i2c.value("ack_count",0u)==3 && i2c.value("nak_count",0u)==0
            && i2c.value("reg_pointer",0u)==2 && i2c.value("last_read_byte",std::string())=="5c"
            && i2c.value("bus_state",std::string())=="idle","P5","P5 I2C burst read advances pointer across 3 registers and ends on 0x5c");
        const auto& outs=c->Frame().at("outputs");
        Check(outs.at("led_n[0]")==false && outs.at("led_n[1]")==true && outs.at("led_n[2]")==false
            && outs.at("led_n[3]")==false && outs.at("led_n[4]")==true,"P5","P5 I2C burst master outputs exact XOR parity 0x1A^0x2B^0x5C on LED bank");

        // 4. Switch back to P4 SPI OLED, pop out floating window, interact with keypad, capture and return.
        board->LoadExample("p4-spi-oled");waitReady();
        board->SetContact(4,false);
        Until([&]{return c->Frame().value("pending_count",size_t(0))>=1;},*c);
        stepTicks("34000000");
        popOut();for(int n=0;n<6;++n)Pump();
        Check(bool(floatFrame) && board->GetParent()==floatFrame,"P5","workbench pops out cleanly into independent VirtualBoardFloatFrame");
        auto toggleAndStep=[&](unsigned row,unsigned col){
            board->ToggleKeypadKey("keypad0",row,col);
            Until([&]{return c->Frame().value("pending_count",size_t(0))>=1;},*c);
            stepTicks("1000000");
        };
        toggleAndStep(1,2);
        toggleAndStep(3,0);
        const auto& closed=c->Frame().at("board").at("peripherals").at("keypad0").at("closed");
        Check(closed.size()==2,"P5","keypad toggles (1,2) and (3,0) work seamlessly inside popped-out floating window");
        const auto& oledAfter=c->Frame().at("board").at("peripherals").at("oled0");
        Check(oledAfter.value("lit_pixels",0u)==28
            && oledAfter.value("frame_sha256",std::string())=="307a5bc25d1638e7d825a90ae9acc7ebf9b1db1f19ab0a47e0f94dfe93733180","P5","OLED dot matrix remains intact inside floating window");
        {
            for(int n=0;n<4;++n)Pump();
            wxBitmap floatShot;
            Check(sigflow_acceptance::CaptureWindow(floatFrame,floatShot),"P5","captures popped-out VirtualBoardFloatFrame via PW_RENDERFULLCONTENT");
            Check(sigflow_acceptance::DistinctColors(floatShot)>=12,"P5","floating window snapshot renders diverse themed pixels");
            const int headerOffset=floatFrame->FromDIP(40);
            wxRect clientBody(0,headerOffset,floatShot.GetWidth(),std::max(0,floatShot.GetHeight()-headerOffset));
            Check(sigflow_acceptance::DistinctColorsInRegion(floatShot,clientBody)>=10,"P5","floating window body below 34 DIP header renders live workbench canvas and peripherals (not blank gray)");
            Check(floatShot.SaveFile(wxString::FromUTF8(Path(work/"float-showcase.png")),wxBITMAP_TYPE_PNG),"P5","exports float-showcase.png");
        }
        floatFrame->ReturnToMain();for(int n=0;n<6;++n)Pump();
        Check(!floatFrame && board->GetParent()==tabs,"P5","workbench returns to main notebook without losing session state");

        // 5. Export final Light & Dark full-platform offscreen showcase PNGs.
        for(auto mode:{UiTheme::Mode::Light,UiTheme::Mode::Dark}){
            UiTheme::SetMode(mode);
            tabs->SetSelection(0);board->Layout();frame->Layout();
            for(int n=0;n<6;++n)Pump();
            wxBitmap snapshot;
            Check(sigflow_acceptance::CaptureWindow(board,snapshot),"P5","PW_RENDERFULLCONTENT renders full-platform showcase");
            Check(sigflow_acceptance::DistinctColors(snapshot)>=12,"P5","full-platform showcase snapshot exhibits rich themed pixel diversity");
            auto name=mode==UiTheme::Mode::Light?"full-showcase-light.png":"full-showcase-dark.png";
            Check(snapshot.SaveFile(wxString::FromUTF8(Path(work/name)),wxBITMAP_TYPE_PNG),"P5","exports "+std::string(name));
        }

        UiTheme::SetMode(UiTheme::Mode::Light);
        c->Unsubscribe(subscription);c->Shutdown();delete frame;frame=nullptr;c.reset();for(int n=0;n<5;++n)Pump();
        report["status"]="passed";
        report["host"]="real wxApp + full-stage quick launcher + honest pin multiplexing + P5 I2C burst + VirtualBoardFloatFrame";
    }catch(const std::exception& e){
        report["status"]="failed";report["error"]=e.what();std::cerr<<e.what()<<std::endl;result=1;
        if(c)c->Shutdown();if(frame){delete frame;frame=nullptr;}c.reset();
    }
    report["check_count"]=report["checks"].size();
    Write(work/"gui-results.json",report);
    wxTheApp->OnExit();wxEntryCleanup();return result;
}
