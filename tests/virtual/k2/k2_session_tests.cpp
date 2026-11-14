#include "fixture_support.hpp"
#include "eda-platform/Platform.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
namespace vb=eda::virtual_board;
using eda::Json;
namespace {
int checks=0;
void Check(bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);}
struct Counters {std::atomic<int> made{0},finalized{0},destroyed{0};};
// A test double checks concurrency/command semantics; real RTL is tested by the
// separate k2_real_verilated_session target, never counted as this model.
class Model final:public vb::IDutAdapter {
    Counters& counters;std::thread::id owner=std::this_thread::get_id();
    bool clock=false,reset=false,key=true,last_clock=false;unsigned value=0;
    void Owner()const{if(std::this_thread::get_id()!=owner)throw std::runtime_error("owner thread");}
public:
    explicit Model(Counters& c):counters(c){++c.made;}
    ~Model(){Owner();++counters.destroyed;}
    void WriteInput(const vb::PortBitRef& r,bool v)override{
        Owner();if(r.port_id=="k2_counter::clock_in")clock=v;else if(r.port_id=="k2_counter::reset_n")reset=v;
        else if(r.port_id=="k2_counter::key_n")key=v;else throw std::runtime_error("ref");
    }
    void Evaluate(const vb::EventPosition&)override{Owner();if(!reset)value=0;else if(clock&&!last_clock&&!key)value=(value+1)&63;last_clock=clock;}
    bool ReadOutput(const vb::PortBitRef& r)const override{Owner();return !(value&(1u<<r.bit_slot));}
    void Finalize()override{Owner();++counters.finalized;}
};
vb::PortManifest Manifest(){
    vb::PortManifest m;m.top_module="k2_counter";m.source_sha256=std::string(64,'a');
    for(const std::string name:{"clock_in","reset_n","key_n","led_n"}){
        vb::PortDecl p;p.rtl_name=name;p.port_id=m.top_module+"::"+name;p.source_ref="unit-test";
        p.direction=name=="led_n"?vb::Direction::Output:vb::Direction::Input;
        if(name=="led_n"){p.shape=vb::PortShape::PackedVector;p.width=6;p.decl_left=5;p.decl_right=0;}
        for(uint32_t i=0;i<p.width;++i)p.bits.push_back({p.port_id,name=="led_n"?std::optional<int64_t>(i):std::nullopt,i});m.ports.push_back(p);
    }return m;
}
Json Set(const std::string& id,const std::string& target,uint8_t value){return {{"op","set"},{"command_id",id},{"target",target},{"state",value}};}
Json Control(const std::string& id,const std::string& op){return {{"op",op},{"command_id",id}};}
Json Read(const std::filesystem::path& p){std::ifstream f(p);Json j;f>>j;return j;}
void Fixtures(const std::filesystem::path& root){
    auto fixture=Read(root/"tests/virtual/k2/fixtures/pause-replay-cases.json");
    for(const auto& test:fixture.at("cases")){
        Counters counts;auto c=k2fixture::Contract(root,Manifest());vb::VirtualSession session(c,[&]{return std::make_unique<Model>(counts);});
        for(const auto& command:test.at("requests")){
            auto id=command.at("id").get<std::string>();auto type=command.at("type").get<std::string>();
            auto response=type=="input.set"?session.Submit(Set(id,command.at("target"),uint8_t(command.at("action")=="press"?vb::State::SLO:vb::State::FLT))):
                session.Submit(Control(id,type=="session.step"?"step":"stop"));
            Check(response.at("status")!="rejected","fixture command accepted");
        }
        auto audit=session.Submit({{"op","journal"}});
        const auto& acks=audit.at("acks");
        for(const auto& expected:test.at("expected_applied"))Check(acks.at(expected.at("id").get<std::string>()).at("status")=="applied","expected applied");
        for(const auto& id:test.at("expected_superseded"))Check(acks.at(id.get<std::string>()).at("status")=="superseded","expected superseded");
        if(test.contains("expected_cancelled"))for(const auto& id:test.at("expected_cancelled"))Check(acks.at(id.get<std::string>()).at("status")=="cancelled","expected cancelled");
        Check(counts.made==1,"one persistent model per session");
        session.Submit(Control("end","stop"));std::cout<<"PASS "<<test.at("id")<<'\n';
    }
    for(const auto& test:fixture.at("unknown_guard_cases")){
        auto name=test.at("state").get<std::string>();uint8_t raw=name=="FLT"?uint8_t(vb::State::FLT):name=="CON"?uint8_t(vb::State::SUD)|vb::ContentionFlag:uint8_t(vb::State::WHI);
        auto r=vb::ResolveM0({{"fixture",raw}});auto direction=test.at("direction")=="output"?vb::Direction::Output:vb::Direction::Input;
        auto result=vb::GuardInput(direction,r);auto expected=test.at("expected").get<std::string>();
        Check(result==(expected=="ObserveOnly"?vb::ImportDecision::ObserveOnly:expected=="Import1"?vb::ImportDecision::Import1:vb::ImportDecision::PauseUnknownInput),"guard reference "+test.at("id").get<std::string>());
    }
}
void Stateful(const std::filesystem::path& root){
    Counters counts;auto c=k2fixture::Contract(root,Manifest());
    auto factory=[&]{return std::make_unique<Model>(counts);};
    vb::VirtualSession s(c,factory);
    auto before=s.Submit({{"op","get"}});auto set=Set("release-reset","reset_p4",uint8_t(vb::State::FLT));
    auto pending=s.Submit(set);Check(pending.at("status")=="pending","paused intent pending");
    auto after=s.Submit({{"op","get"}});Check(before.at("eval_calls")==after.at("eval_calls") && before.at("input_writes")==after.at("input_writes"),"paused intent writes/evals frozen");
    Check(s.Submit(set)==pending,"idempotent pending resend");
    s.Submit(Control("first-edge","step"));
    auto invalid=Set("fault-reset","reset_p4",uint8_t(vb::State::SUD)|vb::ContentionFlag);
    s.Submit(invalid);before=s.Submit({{"op","get"}});auto blocked=s.Submit(Control("blocked-step","step"));
    Check(blocked.at("status")=="blocked" && blocked.at("reason")=="UnknownInput","async reset contention blocks before import");
    Check(blocked.at("eval_calls")==before.at("eval_calls") && blocked.at("input_writes")==before.at("input_writes"),"guard writes zero DUT bits");
    Check(blocked.at("position")==before.at("position"),"guard keeps last committed position");
    s.Submit(Set("repair-reset","reset_p4",uint8_t(vb::State::FLT)));Check(s.Submit(Control("resume-edge","step")).at("status")=="applied","repair resumes retained boundary");
    auto audit=s.Submit({{"op","journal"}});
    auto replay=vb::VirtualSession::ReplayCommitted(c,factory,audit.at("records"));Check(replay.at("status")=="passed","committed-only replay");
    auto corrupt=audit.at("records");corrupt[0]["source_sha256"]=std::string(64,'0');bool rejected=false;
    try{vb::VirtualSession::ReplayCommitted(c,factory,corrupt);}catch(...){rejected=true;}Check(rejected,"replay fingerprint mismatch rejected");
    s.Submit(Control("go","play"));std::this_thread::sleep_for(std::chrono::milliseconds(10));auto paused=s.Submit(Control("hold","pause"));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));after=s.Submit({{"op","get"}});
    Check(after.at("position")==paused.at("position") && after.at("eval_calls")==paused.at("eval_calls"),"Pause ACK freezes time and eval");
    auto stop=s.Submit(Control("stop","stop"));Check(stop.at("final_calls")==1,"final exactly once");
    Check(s.Submit(Control("stop","stop"))==stop,"Stop resend idempotent");
    auto unpowered=k2fixture::Contract(root,Manifest());unpowered.electrical.power.vccio_mv["3"]=std::nullopt;rejected=false;
    try{vb::VirtualSession bad(unpowered,factory);}catch(...){rejected=true;}Check(rejected,"K1 electrical gate prevents model creation");
    auto floating=k2fixture::Contract(root,Manifest());floating.inputs[0].fixed_drivers.clear();Counters unknown;
    vb::VirtualSession guard(floating,[&]{return std::make_unique<Model>(unknown);});auto snapshot=guard.Submit({{"op","get"}});
    Check(snapshot.at("reason")=="UnknownInput" && snapshot.at("input_writes")=="0" && snapshot.at("eval_calls")=="0","initial unknown blocks all DUT imports");
    auto limited=k2fixture::Contract(root,Manifest());limited.max_commits=1;Counters limitedCounts;
    vb::VirtualSession quota(limited,[&]{return std::make_unique<Model>(limitedCounts);});
    auto full=quota.Submit(Control("limit-step","step"));
    Check(full.at("status")=="blocked" && full.at("reason")=="ResourceLimit" && full.at("eval_calls")=="1","commit quota pauses without an unrecorded eval");
}
void Resolver(){
    Check(vb::ResolveM0({}).state==uint8_t(vb::State::FLT),"no drivers floating");
    Check(vb::ResolveM0({{"weak",uint8_t(vb::State::WHI)},{"strong",uint8_t(vb::State::SLO)}}).state==uint8_t(vb::State::SLO),"weak pull yields to strong low");
    auto conflict=vb::ResolveM0({{"a",uint8_t(vb::State::SHI)},{"b",uint8_t(vb::State::SLO)}});
    Check(!conflict.known() && (conflict.state&vb::ContentionFlag),"strong contention flag");
    Check(!vb::ResolveM0({{"ideal",uint8_t(vb::State::IHI)}}).supported,"unimplemented Ideal semantics rejected");
    Check(!vb::ResolveM0({{"a",7},{"a",5}}).supported,"duplicate driver ownership rejected");
}
}
int main(int argc,char** argv){try{if(argc!=2)return 2;auto root=eda::platform::PathFromUtf8(argv[1]);Resolver();Fixtures(root);Stateful(root);std::cout<<"PASS "<<checks<<" K2 core checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
