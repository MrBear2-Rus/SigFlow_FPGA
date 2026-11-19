#include "TestCounter.h"
#include "eda-platform/Platform.h"
#include "eda-virtual-board/DurableJournal.h"
#include "eda-virtual-board/LiveTraceStore.h"
#include <iostream>
using namespace m0test;namespace fs=std::filesystem;
namespace {
int checks=0;void Check(bool v,const std::string& m){++checks;if(!v)throw std::runtime_error(m);}
template<class F>void Error(F f,const std::string& code){bool rejected=false;try{f();}catch(const std::exception& e){rejected=std::string(e.what()).find(code)!=std::string::npos;}Check(rejected,code);}
const Json& Node(const Json& frame,const std::string& name){return frame.at("board").at("nodes").at(name);}
void Wiring(const fs::path& root){auto c=Contract(root);vb::VirtualSession correct(c,Factory());auto a=Start(correct);
    constexpr int ledPads[]={10,11,13,14,15,16};
    for(int i=0;i<6;++i){auto graph=c.board->Resolve({{"pad."+std::to_string(ledPads[i]),{{"test-driver",13}}}},{},0);
        bool exact=true;for(int led=0;led<6;++led)if(graph.at("leds").at("led"+std::to_string(led)).at("lit").get<bool>()!=(led==i))exact=false;
        Check(exact,"all six reviewed LED edges route independently");}
    Check(a.at("board").at("leds").at("led0").at("lit")==true,"CST low bit drives LED0");
    Check(Node(a,"pad.10").at("state_code")==13 && Node(a,"led0.K").at("state_code")==13,"connected endpoints share strong low");
    auto swapped=c;for(auto& b:swapped.bindings)if(b.package_pin==10)b.package_pin=11;else if(b.package_pin==11)b.package_pin=10;
    vb::VirtualSession wrong(swapped,Factory());auto b=Start(wrong);
    Check(b.at("board").at("leds").at("led1").at("lit")==true && b.at("board").at("leds").at("led0").at("lit")==false,"wrong CST migrates phenomenon to LED1");
    auto missing=c;std::erase_if(missing.bindings,[](const auto& b){return b.package_pin==10;});
    Error([&]{vb::VirtualSession rejected(missing,Factory());},"E_MISSING_BINDING");missing.allow_unbound_outputs=true;
    vb::VirtualSession unbound(missing,Factory());auto u=Start(unbound);
    Check(Node(u,"pad.10").at("state_code")==2 && Node(u,"led0.K").at("state_code")==2 && u.at("board").at("leds").at("led0").at("lit")==false,"unbound output never bypasses CST");
    Check(u.at("binding_status")=="teaching-unbound" && u.at("binding_diagnostics").size()==1,"unbound output explicitly diagnosed");
    auto noClock=c;std::erase_if(noClock.bindings,[](const auto& b){return b.package_pin==52;});Error([&]{vb::VirtualSession rejected(noClock,Factory());},"E_MISSING_BINDING");
    auto wrongClock=c;for(auto& b:wrongClock.bindings)if(b.package_pin==52)b.package_pin=3;else if(b.package_pin==3)b.package_pin=52;
    Error([&]{vb::VirtualSession rejected(wrongClock,Factory());},"E_CLOCK_BINDING");
    auto descriptor=c.board->Descriptor();descriptor["components"][0]["model"]="not-implemented@1";Error([&]{vb::BoardTopology rejected(descriptor,c.electrical);},"E_BOARD_MODEL_UNSUPPORTED");
    auto solved=c.board->Resolve({{"pad.10",{{"a",13},{"b",15}}}},{},0);
    Check(solved.at("leds").at("led0").at("lit")==false && solved.at("nodes").at("led0.K").at("contention")==true,"contention never lights passive LED");
    std::cout<<"PASS A4 correct/swapped/unbound CST, required clock role, declarative board and passive LED validity\n";
}
void Replay3(const vb::SessionContract& c,const Json& records){auto digests=vb::DurableJournal::Digests(records);
    for(int attempt=0;attempt<3;++attempt){auto r=vb::VirtualSession::ReplayCommitted(c,Factory(),records);
        Check(r.at("board_trace_verified")==true && r.at("circuit_digest").get<std::string>()==digests.circuit && r.at("control_digest").get<std::string>()==digests.control,"three from-zero board/trace/digest replays");}
    auto trace=vb::DurableJournal::RestoreTrace(records);auto view=trace->AcquireReadView();
    for(const auto& signal:view.Signals()){size_t expected=0;for(const auto& r:records)if(r.at("kind")=="commit")for(const auto& e:r.at("trace_events"))if(e.at("signal_id")==signal.id)++expected;
        Check(trace->Query(view,signal.id,0,view.Tail().tick).records.size()==expected,"restored trace retains every endpoint event");}
}
void Faults(const fs::path& root,const fs::path& work){auto c=Contract(root);vb::VirtualSession normal(c,Factory());Start(normal);normal.Submit(Cmd("step","normal-2"));normal.Submit(Cmd("stop","stop"));
    auto normalAudit=normal.Submit({{"op","journal"}});Replay3(c,normalAudit.at("records"));
    auto disk=c;disk.durability="durable";disk.journal_path=work/"faults.journal";fs::remove(disk.journal_path);
    vb::VirtualSession session(disk,Factory());auto initial=Start(session);auto pending=session.Submit(Open("wire.led0"));
    Check(pending.at("status")=="pending" && pending.at("trace")==initial.at("trace") && pending.at("input_writes")==initial.at("input_writes"),"paused fault has no circuit side effects");
    auto open=session.Submit(Cmd("step","step-open"));
    Check(open.at("state")=="Paused" && open.at("reason")=="StepComplete" && Node(open,"led0.K").at("state_code")==2,"output open observes FLT without guard pause");
    auto toggled=session.Submit(Cmd("step","toggle-open"));Check(Node(toggled,"pad.10").at("state_code")!=Node(open,"pad.10").at("state_code") && Node(toggled,"led0.K").at("state_code")==2,"Pad toggles independently of disconnected LED");
    auto revoke=Cmd("fault.revoke","revoke");revoke["fault_id"]="open";Check(session.Submit(revoke).at("status")=="pending","paused revoke staged");
    session.Submit(Cmd("step","restore-step"));session.Submit(Cmd("stop","stop"));auto audit=session.Submit({{"op","journal"}});
    bool immediate=false,independent=false;for(const auto& r:audit.at("records")){
        if(r.at("kind")=="fault_commit" && r.at("op")=="revoke")immediate=r.at("board").at("leds").at("led0").at("lit")==true;
        if(r.at("kind")=="commit" && r.at("board").at("wires").at("wire.led0")==false)independent=r.at("board").at("nodes").at("led0.K").at("state_code")==2;
    }Check(immediate && independent,"restoration applies at topology boundary before next clock");
    auto recovery=vb::DurableJournal::Recover(disk.journal_path);Check(recovery.records==audit.at("records"),"fault journal disk recovery exact");Replay3(disk,recovery.records);
    auto tampered=recovery.records;for(auto& r:tampered)if(r.at("kind")=="commit"){r["board"]["nodes"]["led0.K"]["state_code"]=2;break;}
    Error([&]{vb::VirtualSession::ReplayCommitted(disk,Factory(),tampered);},"E_REPLAY_BOARD_DIVERGENCE");
    vb::VirtualSession input(c,Factory());auto before=Start(input);input.Submit(Open("wire.clock"));auto blocked=input.Submit(Cmd("step","blocked"));
    Check(blocked.at("reason")=="UnknownInput" && blocked.at("input_writes")==before.at("input_writes") && blocked.at("eval_calls")==before.at("eval_calls"),"clock open guards before all DUT writes");
    Check(blocked.at("guard").at("last_committed_position")==blocked.at("position"),"unknown guard retains exact topology commit fence");
    vb::VirtualSession pull(c,Factory());Start(pull);pull.Submit(Open("wire.key3"));auto known=pull.Submit(Cmd("step","pull-up"));
    Check(known.at("reason")=="StepComplete" && Node(known,"pad.3").at("state_code")==7,"Pad-side pullup survives contact open");
    auto noPull=c;auto desc=c.board->Descriptor();desc["fixed_drivers"]=Json::array();noPull.board=std::make_shared<const vb::BoardTopology>(desc,c.electrical);
    vb::VirtualSession unknown(noPull,Factory());auto uninitialized=unknown.Submit({{"op","get"}});
    Check(uninitialized.at("reason")=="UnknownInput" && uninitialized.at("input_writes")=="0" && uninitialized.at("eval_calls")=="0","initial unknown blocks with zero imports");
    noPull.inputs.at(0).initial_manual_state=13;vb::VirtualSession dynamic(noPull,Factory());dynamic.Submit(Open("wire.key3"));auto lost=dynamic.Submit(Cmd("step","lost"));
    Check(lost.at("reason")=="UnknownInput" && lost.at("input_writes")=="3","unpulled input open blocks next import");
    vb::VirtualSession transient(c,Factory());Start(transient);auto timed=Open("wire.led0","window");timed["trigger"]={{"type","transient"},{"start_tick","55556"},{"end_tick","92593"}};
    transient.Submit(timed);auto start=transient.Submit(Cmd("step","start-window")),end=transient.Submit(Cmd("step","end-window"));
    Check(start.at("board").at("wires").at("wire.led0")==false && end.at("board").at("wires").at("wire.led0")==true,"transient half-open interval activates and restores exactly");
    auto stream=session.Submit({{"op","trace.commits"},{"after_revision","0"},{"limit",2}});Check(stream.at("batches").size()==2 && stream.at("complete")==false,"bounded committed trace transport after Stop");
    std::cout<<"PASS A7/A9/A10 staged topology, two-sided open traces, pullup counterexamples, transient deadlines and 3x normal/fault replay\n";
}
}
int main(int argc,char** argv){try{if(argc!=4)return 2;auto root=eda::platform::PathFromUtf8(argv[1]),work=eda::platform::PathFromUtf8(argv[2]);fs::create_directories(work);
    if(std::string(argv[3])=="wiring")Wiring(root);else Faults(root,work);std::cout<<"PASS "<<checks<<" K4/K5 core checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
