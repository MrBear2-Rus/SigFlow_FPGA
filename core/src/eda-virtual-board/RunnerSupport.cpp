#include "RunnerSupport.h"
#include "BoardTopology.h"
#include "ProjectValidation.h"
#include "DurableJournal.h"
#include "ElectricalValidator.h"
#include "ConstraintSheet.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"
#include <fstream>
#include <iostream>
#include <thread>
namespace eda::virtual_board {
namespace {
Json Read(const std::filesystem::path& path){std::ifstream f(path);Json j;f>>j;return j;}
uint64_t Number(const Json& j){auto s=j.get<std::string>();if(s.empty()||s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_RUNNER_NUMBER");return std::stoull(s);}
uint8_t StateByte(const Json& value){
    if(!value.is_number_integer() || value.get<int64_t>()<0 || value.get<uint64_t>()>255)throw std::runtime_error("E_RUNNER_STATE");
    return value.get<uint8_t>();
}
SessionContract Load(const Json& j,const PortManifest& m,const std::string& binary,bool synthetic){
    auto validation=ValidateVirtualProject(j,m,synthetic);if(!validation.at("ok").get<bool>()){
        const auto& d=validation.at("diagnostics").at(0);throw std::runtime_error(d.at("code").get<std::string>()+": "+d.at("message").get<std::string>());}
    if(j.at("schema")!="sigflow.virtual-session.v1")throw std::runtime_error("E_RUNNER_SCHEMA");
    SessionContract c;c.manifest=m;c.run_id=j.at("run_id");c.generation=Number(j.value("generation",Json("1")));
    const auto& id=j.at("identity");c.identity={id.at("device"),id.at("package"),id.at("board_revision")};
    auto electrical=platform::PathFromUtf8(j.at("electrical_profile_path"));std::vector<Diagnostic> diagnostics;
    if(!LoadElectricalProfile(electrical,c.electrical,diagnostics,synthetic))throw std::runtime_error("E_RUNNER_ELECTRICAL");
    c.electrical_sha256=platform::Sha256FileHex(electrical);c.adapter_sha256=binary;
    std::ifstream f(platform::PathFromUtf8(j.at("cst_path")));std::string text((std::istreambuf_iterator<char>(f)),{});
    auto imported=eda::cst::CstGenerator().ImportCst(text,m);if(!imported.ok())throw std::runtime_error("E_RUNNER_CST");c.bindings=imported.bindings;
    eda::cst::ConstraintSheetV2 sheet;sheet.manifest=m;sheet.bindings=c.bindings;sheet.topModule=m.top_module;
    sheet.device=c.identity.device;sheet.package=c.identity.package;sheet.boardRevision=c.identity.board_revision;
    c.allow_unbound_outputs=j.value("allow_unbound_outputs",false);
    auto errors=eda::cst::ConstraintValidator::Validate(sheet,&c.electrical);
    for(const auto& d:errors)if(d.code!="E_MISSING_BINDING" || !c.allow_unbound_outputs)throw std::runtime_error(d.code+": "+d.message);
    if(j.contains("board_path"))c.board=BoardTopology::Load(platform::PathFromUtf8(j.at("board_path")),c.electrical);
    for(const auto& e:j.at("inputs")){
        InputEndpoint input;input.target=e.at("target");input.package_pin=e.at("package_pin");input.initial_manual_state=StateByte(e.value("initial_manual_state",Json(int(State::FLT))));
        for(const auto& driver:e.at("fixed_drivers"))input.fixed_drivers.push_back({driver.at("driver_id"),StateByte(driver.at("state"))});c.inputs.push_back(input);
    }
    const auto& clock=j.at("clock");c.clock_pad=clock.at("package_pin");c.frequency_hz=Number(clock.at("frequency_hz"));
    if(clock.contains("port_ref")){
        for(const auto& p:m.ports)if(p.direction==Direction::Input)for(const auto& ref:p.bits)if(m.Label(ref)==clock.at("port_ref").get<std::string>())c.required_clock=ref;
        if(!c.required_clock)throw std::runtime_error("E_CLOCK_PORT_REF");
    }
    c.clock_enabled=clock.value("enabled",true);c.max_step_events=clock.value("max_step_events",10000u);
    c.max_step_ticks=Number(clock.value("max_step_ticks",Json("1000000000000")));
    c.max_delta=j.value("max_delta",256u);c.max_commits=j.value("max_commits",size_t(100000));
    c.durability=j.value("durability","memory");c.journal_path=platform::PathFromUtf8(j.value("journal_path",""));return c;
}
Json AdapterSelfTest(const PortManifest& m,const VirtualSession::Factory& factory){
    auto model=factory();size_t checks=0;
    auto expect=[&](auto fn,const std::string& code){bool failed=false;try{fn();}catch(const std::exception& e){failed=std::string(e.what()).find(code)!=std::string::npos;}if(!failed)throw std::runtime_error("E_ADAPTER_SELFTEST: "+code);++checks;};
    for(const auto& p:m.ports)for(const auto& ref:p.bits){
        if(p.direction==Direction::Input)model->WriteInput(ref,false);else model->ReadOutput(ref);++checks;
        auto bad=ref;bad.bit_slot=p.width;expect([&]{if(p.direction==Direction::Input)model->WriteInput(bad,true);else model->ReadOutput(bad);},"E_ADAPTER_REF");
        bad=ref;bad.hdl_index=999;expect([&]{if(p.direction==Direction::Input)model->WriteInput(bad,true);else model->ReadOutput(bad);},"E_ADAPTER_REF");
    }
    expect([&]{model->WriteInput({"unknown",{},0},false);},"E_ADAPTER_REF");
    bool rejected=false;std::thread other([&]{try{model->Evaluate({});}catch(const std::exception& e){rejected=std::string(e.what())=="E_MODEL_THREAD";}});other.join();
    if(!rejected)throw std::runtime_error("E_ADAPTER_SELFTEST_THREAD");++checks;
    model->Evaluate({});model->Finalize();expect([&]{model->Finalize();},"E_FINAL_TWICE");
    return {{"status","passed"},{"checks",checks}};
}
}
int SessionRunnerMain(int argc,char** argv,const PortManifest& manifest,VirtualSession::Factory factory){
    try{
        if(argc==2 && std::string(argv[1])=="--self-test-adapter"){std::cout<<AdapterSelfTest(manifest,factory).dump()<<'\n';return 0;}
        if(argc<2 || argc>3)throw std::runtime_error("usage: generated-runner config.json [--allow-synthetic-test-profile]");
        bool synthetic=argc==3 && std::string(argv[2])=="--allow-synthetic-test-profile";
        if(argc==3 && !synthetic)throw std::runtime_error("E_RUNNER_ARGUMENT");
        auto config=Read(platform::PathFromUtf8(argv[1]));auto contract=Load(config,manifest,platform::Sha256FileHex(platform::PathFromUtf8(argv[0])),synthetic);
        VirtualSession session(contract,factory);
        for(std::string line;std::getline(std::cin,line);){
            try{
                auto command=Json::parse(line);Json response;
                if(command.at("op")=="replay.current" || command.at("op")=="replay.file"){
                    auto state=session.Submit({{"op","get"}});if(state.at("state")=="Running")throw std::runtime_error("E_REPLAY_REQUIRES_PAUSE");
                    auto count=command.value("repetitions",3u);if(!count || count>10)throw std::runtime_error("E_REPLAY_REPETITIONS");
                    Json records;auto replayContract=contract;
                    if(command.at("op")=="replay.file"){
                        records=DurableJournal::Recover(platform::PathFromUtf8(command.at("path")),false).records;
                        if(!records.is_array() || records.empty())throw std::runtime_error("E_REPLAY_EMPTY");
                        // Run identity is the selected journal's identity. All design/tool/
                        // electrical fingerprints remain mandatory and are checked by replay.
                        replayContract.run_id=records.at(0).at("trace_run_id");replayContract.generation=Number(records.at(0).at("trace_generation"));
                    }else records=session.Submit({{"op","journal"}}).at("records");
                    response={{"status","passed"},{"repetitions",count},{"attempts",Json::array()}};
                    Json first;
                    for(unsigned i=0;i<count;++i){auto r=VirtualSession::ReplayCommitted(replayContract,factory,records);
                        if(i && (r.at("circuit_digest")!=first.at("circuit_digest") || r.at("control_digest")!=first.at("control_digest") || r.at("matched_commits")!=first.at("matched_commits")))throw std::runtime_error("E_REPLAY_DIVERGENCE");
                        if(!i)first=r;response["attempts"].push_back(r);}
                    for(auto key:{"matched_commits","circuit_digest","control_digest","board_trace_verified"})response[key]=first.at(key);
                }
                else if(command.at("op")=="replay")response=VirtualSession::ReplayCommitted(contract,factory,command.at("records"));
                else if(command.at("op")=="recover"){
                    auto recovered=DurableJournal::Recover(platform::PathFromUtf8(command.at("path")));
                    response=VirtualSession::ReplayCommitted(contract,factory,recovered.records);
                    auto restored=DurableJournal::RestoreTrace(recovered.records);auto water=restored->Watermark();
                    response["trace_revision"]=std::to_string(water.trace_revision);response["truncated_bytes"]=recovered.truncated_bytes;
                }else response=session.Submit(command);
                std::cout<<response.dump()<<std::endl;
            }catch(const std::exception& e){std::cout<<Json{{"status","rejected"},{"error",e.what()}}.dump()<<std::endl;}
        }return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
}
