#include "VirtualSession.h"
#include "BoardTopology.h"
#include "DurableJournal.h"
#include "LiveTraceStore.h"
#include "ElectricalValidator.h"
#include "PortManifestBuilder.h"
#include "Phase1Session.h"
#include "eda-platform/Sha256.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace eda::virtual_board {
namespace {
Json Position(const EventPosition& p){return {{"tick",std::to_string(p.tick)},{"delta",p.delta},{"event_seq",std::to_string(p.event_seq)}};}
uint64_t Number(const Json& j){
    auto text=j.get<std::string>();
    if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_DURATION_TICKS");
    return std::stoull(text);
}
std::vector<LiveSignal> Signals(const SessionContract& c){
    std::vector<LiveSignal> signals;
    for(const auto& p:c.manifest.ports)for(const auto& bit:p.bits)
        signals.push_back({uint32_t(signals.size()),"port:"+c.manifest.Label(bit),1,"port"});
    std::set<int> pads;for(const auto& b:c.bindings)pads.insert(b.package_pin);
    for(auto pad:pads)signals.push_back({uint32_t(signals.size()),"pad:"+std::to_string(pad),1,"pad"});
    if(c.board){
        for(const auto& node:c.board->Nodes())signals.push_back({uint32_t(signals.size()),node,1,"endpoint"});
        for(const auto& edge:c.board->Edges())signals.push_back({uint32_t(signals.size()),edge.id,1,"connection"});
        for(const auto& component:c.board->Descriptor().at("components"))if(component.at("model")=="sigflow.led@1")
            signals.push_back({uint32_t(signals.size()),component.at("id").get<std::string>()+".lit",1,"led"});
    }
    return signals;
}
using TraceValues=std::map<std::string,std::pair<std::string,int>>;
Json BoardFor(const SessionContract& c,const std::map<std::string,uint8_t>& manual,bool high,const Json& outputs,const FaultSet& faults,uint64_t tick){
    if(!c.board)return Json::object();std::map<std::string,std::vector<Driver>> drivers;
    drivers[c.board->ClockEndpoint()].push_back({"oscillator",uint8_t(high?State::SHI:State::SLO)});
    for(const auto& input:c.inputs){
        auto pad="pad."+std::to_string(input.package_pin);drivers[pad]=input.fixed_drivers;
        if(input.package_pin!=c.clock_pad)drivers[c.board->ControlEndpoint(input.package_pin)].push_back({"contact",manual.at(input.target)});
    }
    for(const auto& binding:c.bindings){
        auto pad="pad."+std::to_string(binding.package_pin);auto label=c.manifest.Label(binding.ref);
        if(binding.pull_up)drivers[pad].push_back({"cst.pullup",uint8_t(State::WHI)});
        if(binding.pull_down)drivers[pad].push_back({"cst.pulldown",uint8_t(State::WLO)});
        if(c.manifest.Find(binding.ref.port_id)->direction==Direction::Output && outputs.contains(label) && outputs.at(label).is_boolean())
            drivers[pad].push_back({"dut:"+label,uint8_t(outputs.at(label).get<bool>()?State::SHI:State::SLO)});
    }return c.board->Resolve(drivers,faults,tick);
}
void AddBoardValues(const Json& board,TraceValues& values,const std::map<std::string,uint32_t>& ids){
    if(board.empty())return;
    for(const auto& [name,node]:board.at("nodes").items()){
        auto value=std::make_pair(node.at("logic").get<std::string>(),node.at("state_code").get<int>());values[name]=value;
        if(name.starts_with("pad.") && ids.count("pad:"+name.substr(4)))values["pad:"+name.substr(4)]=value;
    }
    for(const auto& [name,closed]:board.at("wires").items())values[name]={closed.get<bool>()?"1":"0",-1};
    for(const auto& [name,led]:board.at("leds").items())values[name+".lit"]={led.at("lit").get<bool>()?"1":"0",-1};
}
Json Header(const SessionContract& c){
    Json bs=Json::array(),signals=Json::array(),endpoints=Json::array();
    for(const auto& b:c.bindings)bs.push_back({{"port_ref",c.manifest.Label(b.ref)},{"package_pin",b.package_pin},
        {"io_type",b.io_type},{"drive",b.drive},{"pull_up",b.pull_up},{"pull_down",b.pull_down}});
    for(const auto& e:c.inputs){Json drivers=Json::array();for(const auto& d:e.fixed_drivers)drivers.push_back({{"id",d.driver_id},{"state",d.state}});
        endpoints.push_back({{"target",e.target},{"pad",e.package_pin},{"initial_state",e.initial_manual_state},{"drivers",drivers}});}
    for(const auto& signal:Signals(c))signals.push_back({{"id",signal.id},{"name",signal.name},{"width",signal.width},{"kind",signal.kind}});
    auto text=bs.dump()+endpoints.dump();
    return {{"kind","run_header"},{"schema","sigflow.session-journal.m0.v3"},
        {"source_sha256",c.manifest.source_sha256},{"adapter_sha256",c.adapter_sha256},
        {"electrical_sha256",c.electrical_sha256},{"bindings_sha256",platform::Sha256Hex(text.data(),text.size())},
        {"resolver_version",ResolverVersion},{"scheduler_version","sigflow.scheduler.m0.v3"},
        {"board_sha256",c.board?c.board->Fingerprint():""},{"allow_unbound_outputs",c.allow_unbound_outputs},
        {"required_clock",c.required_clock?c.manifest.Label(*c.required_clock):""},
        {"digest_version",JournalDigestVersion},
        {"tick_ps","1"},{"frequency_hz",std::to_string(c.frequency_hz)},{"clock_enabled",c.clock_enabled},
        {"max_delta",c.max_delta},{"trace_signals",signals},{"trace_run_id",c.run_id},{"trace_generation",std::to_string(c.generation)}};
}
class Clock {
public:
    explicit Clock(uint64_t hz):denominator_(2*hz){
        if(!hz || hz>500000000000ULL)throw std::runtime_error("E_CLOCK_RESOLUTION");
    }
    uint64_t Next(){
        constexpr uint64_t scale=1000000000000ULL; // 1 ps tick
        auto sum=remainder_+scale%denominator_;
        auto advance=scale/denominator_+sum/denominator_;
        if(floor_>std::numeric_limits<uint64_t>::max()-advance-1)throw std::runtime_error("E_TIME_OVERFLOW");
        floor_+=advance;remainder_=sum%denominator_;return floor_+(remainder_>=denominator_/2?1:0);
    }
private:
    uint64_t denominator_,floor_=0,remainder_=0;
};
}
struct VirtualSession::Impl {
    struct Request {Json value;std::promise<Json> reply;};
    SessionContract contract;
    Factory factory;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::shared_ptr<Request>> queue;
    std::promise<void> ready;
    std::atomic<bool> finished{false};
    // Worker-owned state only from here down.
    std::unique_ptr<IDutAdapter> model;
    std::string state="Paused",reason="Ready";
    EventPosition committed,attempted,topology_position;
    Clock clock;
    bool clock_high=false;
    uint64_t evaluations=0,writes=0,final_calls=0,edges=0;
    bool initialized=false;
    struct Pending {std::string id;uint8_t value;bool pulse=false;uint8_t released=0;uint64_t duration=0;};
    struct Pulse {std::string id;uint8_t released;uint64_t due;};
    std::map<std::string,Pulse> pulses;
    std::map<std::string,Pending> pending;
    struct PendingFault {std::string command_id;std::optional<EdgeFault> fault;};
    FaultSet faults;
    std::map<std::string,PendingFault> pending_faults;
    Json last_board=Json::object(),binding_diagnostics=Json::array();
    std::map<std::string,Json> acknowledgements,requests;
    std::map<std::string,uint8_t> manual;
    std::map<int,PinBindingV2> pad_map;
    std::map<int,InputEndpoint> input_map;
    std::map<std::string,int> target_map;
    std::unique_ptr<DurableJournal> log;
    std::shared_ptr<LiveTraceStore> trace;
    std::map<std::string,uint32_t> signal_ids;
    std::map<uint32_t,std::pair<std::string,int>> trace_values;
    Json last_outputs=Json::object(),last_guard=Json::object();
    std::string last_error;
    Json terminal_snapshot;
    size_t commits=0;

    Impl(SessionContract c,Factory f):contract(std::move(c)),factory(std::move(f)),clock(contract.frequency_hz){
        auto valid=PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(contract.manifest));
        if(!valid.ok())throw std::runtime_error("E_SESSION_MANIFEST");
        if(contract.run_id.empty() || !contract.max_commits || !contract.max_commands || !contract.max_queue)
            throw std::runtime_error("E_SESSION_CONTRACT");
        if(contract.adapter_sha256.size()!=64 || contract.electrical_sha256.size()!=64)
            throw std::runtime_error("E_SESSION_FINGERPRINT");
        std::set<std::string> refs;
        for(const auto& b:contract.bindings){
            if(!contract.manifest.Contains(b.ref) || b.package_pin<1 || !pad_map.emplace(b.package_pin,b).second ||
               !refs.insert(contract.manifest.Label(b.ref)).second)throw std::runtime_error("E_SESSION_BINDING");
        }
        for(const auto& p:contract.manifest.ports)for(const auto& b:p.bits)
            if(!refs.count(contract.manifest.Label(b))){
                if(p.direction!=Direction::Output || !contract.allow_unbound_outputs)throw std::runtime_error("E_MISSING_BINDING: "+contract.manifest.Label(b));
                binding_diagnostics.push_back({{"code","W_UNBOUND_OUTPUT"},{"port_ref",contract.manifest.Label(b)}});
            }
        if(contract.required_clock){auto found=pad_map.find(contract.clock_pad);
            if(!contract.manifest.Contains(*contract.required_clock) || found==pad_map.end() || !(found->second.ref==*contract.required_clock))throw std::runtime_error("E_CLOCK_BINDING");}
        auto ds=ValidateElectrical(contract.electrical,contract.identity,contract.manifest,contract.bindings);
        if(!ds.empty())throw std::runtime_error(ds[0].code+": "+ds[0].message);
        for(const auto& e:contract.inputs){
            if(e.target.empty() || !input_map.emplace(e.package_pin,e).second || !target_map.emplace(e.target,e.package_pin).second)
                throw std::runtime_error("E_ENDPOINT_DUPLICATE");
            auto binding=pad_map.find(e.package_pin);
            if(binding==pad_map.end() || contract.manifest.Find(binding->second.ref.port_id)->direction!=Direction::Input)
                throw std::runtime_error("E_ENDPOINT_BINDING");
            for(const auto& driver:e.fixed_drivers)if(driver.driver_id=="manual" || driver.driver_id=="clock")
                throw std::runtime_error("E_DRIVER_OWNERSHIP");
            auto r=ResolveM0(e.fixed_drivers);if(!r.supported)throw std::runtime_error(r.error);
            if(!ResolveM0({{"manual",e.initial_manual_state}}).supported)throw std::runtime_error("E_STATE_UNSUPPORTED");
            manual[e.target]=e.initial_manual_state;
        }
        for(const auto& [pad,b]:pad_map)if(contract.manifest.Find(b.ref.port_id)->direction==Direction::Input && !input_map.count(pad))
            throw std::runtime_error("E_ENDPOINT_MISSING");
        if(!input_map.count(contract.clock_pad))throw std::runtime_error("E_CLOCK_BINDING");
        if(contract.board){
            if(!contract.required_clock)throw std::runtime_error("E_CLOCK_PORT_REQUIRED");
            if(contract.board->ClockPad()!=contract.clock_pad || contract.board->Frequency()!=contract.frequency_hz)throw std::runtime_error("E_BOARD_CLOCK_BINDING");
            for(const auto& [pad,b]:pad_map)if(!contract.board->Nodes().count("pad."+std::to_string(pad)))throw std::runtime_error("E_BOARD_PAD");
            for(const auto& [pad,e]:input_map)if(pad!=contract.clock_pad)contract.board->ControlEndpoint(pad);
        }
        if(!contract.generation || !contract.max_step_events || !contract.max_step_ticks || !contract.max_delta)
            throw std::runtime_error("E_SESSION_BUDGET");
        auto signals=Signals(contract);for(const auto& signal:signals)signal_ids[signal.name]=signal.id;
        trace=std::make_shared<LiveTraceStore>(contract.run_id,contract.generation,signals,contract.trace_limits);
        log=std::make_unique<DurableJournal>(contract.durability,contract.journal_path);
        Record(Header(contract));
        auto future=ready.get_future();worker=std::thread([this]{Run();});
        try{future.get();}catch(...){worker.join();throw;}
    }
    ~Impl(){
        if(worker.joinable()){
            if(!finished.load())try{Submit({{"op","stop"},{"command_id","__session_destructor_stop"}});}catch(...){}
            worker.join();
        }
    }
    Json Submit(Json j){
        if(std::this_thread::get_id()==worker.get_id())throw std::runtime_error("E_SESSION_REENTRANT");
        auto r=std::make_shared<Request>();r->value=std::move(j);auto future=r->reply.get_future();
        {
            std::lock_guard lock(mutex);
            if(finished.load()){
                if(r->value.value("op","")=="get")return terminal_snapshot;
                if(r->value.value("op","")=="journal")return JournalSnapshot();
                if(r->value.value("op","")=="trace.commits")return Handle(r->value);
                auto id=r->value.value("command_id","");auto original=requests.find(id);
                if(original!=requests.end() && original->second==r->value)return acknowledgements.at(id);
                return {{"status","rejected"},{"error","E_SESSION_STOPPED"}};
            }
            if(queue.size()>=contract.max_queue)return {{"status","rejected"},{"error","E_QUEUE_FULL"}};
            queue.push_back(r);
        }
        condition.notify_one();return future.get();
    }
    Json Snapshot()const{
        Json outputs=last_outputs;
        if(!initialized)for(const auto& p:contract.manifest.ports)if(p.direction==Direction::Output)
            for(const auto& b:p.bits)outputs[contract.manifest.Label(b)]=nullptr;
        auto water=trace->Watermark();const auto& digests=log->CurrentDigests();
        Json pendingTargets=Json::object(),pendingFaults=Json::array();for(const auto& [target,p]:pending)pendingTargets[target]={{"state",p.value},{"command_id",p.id}};
        for(const auto& [fault_id,p]:pending_faults)pendingFaults.push_back({{"fault_id",fault_id},{"command_id",p.command_id},{"op",p.fault?"apply":"revoke"},
            {"edge_id",p.fault?p.fault->edge:(faults.count(fault_id)?faults.at(fault_id).edge:"")}});
        return {{"run_id",contract.run_id},{"state",state},{"reason",reason},{"position",Position(committed)},{"topology_position",Position(topology_position)},
            {"attempted_position",Position(attempted)},{"tick_ps","1"},{"eval_calls",std::to_string(evaluations)},
            {"input_writes",std::to_string(writes)},{"edges",std::to_string(edges)},
            {"pending_count",pending.size()+pending_faults.size()},{"pending_inputs",pendingTargets},{"pending_faults",pendingFaults},
            {"faults",FaultsJson(faults)},{"board",last_board},{"binding_diagnostics",binding_diagnostics},{"binding_status",binding_diagnostics.empty()?"bound":"teaching-unbound"},
            {"model_instances",model?1:0},{"final_calls",final_calls},
            {"guard",last_guard},{"outputs",outputs},{"durability",contract.durability},{"resolver_version",ResolverVersion},
            {"error",last_error},{"active_pulses",pulses.size()},
            {"trace",{{"revision",std::to_string(water.trace_revision)},{"tail_event_seq",std::to_string(water.tail_position.event_seq)},
                      {"committed_through_tick",std::to_string(water.tail_position.tick)},{"run_id",contract.run_id},{"generation",std::to_string(contract.generation)}}},
            {"digests",{{"circuit",digests.circuit},{"control",digests.control},{"algorithm",JournalDigestVersion}}}};
    }
    void Record(const Json& record){
        try{log->AppendRecord(record);}catch(const std::exception& e){state="Faulted";reason="JournalFailure";last_error=e.what();throw;}
    }
    void Decision(const std::string& id,const std::string& status,const std::string& by={}){
        auto& ack=acknowledgements.at(id);ack["status"]=status;if(!by.empty())ack["by"]=by;
        Json record={{"kind","decision"},{"command_id",id},{"status",status},{"by",by}};
        if(status=="blocked"){record["reason"]=reason;record["attempted_position"]=Position(attempted);record["guard"]=last_guard;}
        Record(record);
    }
    bool SetAttempted(uint64_t tick){
        if(commits>=contract.max_commits){state="Paused";reason="ResourceLimit";last_error="E_COMMIT_QUOTA";return false;}
        if(committed.event_seq==std::numeric_limits<uint64_t>::max())throw std::runtime_error("E_EVENT_POSITION_OVERFLOW");
        if(tick<committed.tick)throw std::runtime_error("E_EVENT_POSITION_ORDER");
        if(tick==committed.tick && commits && committed.delta>=contract.max_delta-1){
            state="Faulted";reason="NonConvergent";last_error="E_NON_CONVERGENT";throw std::runtime_error(last_error);
        }
        attempted={tick,tick==committed.tick && commits?uint32_t(committed.delta+1):0,committed.event_seq+1};return true;
    }
    bool Publish(Json outputs,const Json& effective,TraceValues values,bool evaluated,const std::map<std::string,uint8_t>& candidate,bool high,FaultSet nextFaults,Json board){
        AddBoardValues(board,values,signal_ids);std::vector<LiveTransition> transitions;auto next_values=trace_values;
        for(const auto& [name,value]:values){
            auto id=signal_ids.at(name);auto old=trace_values.find(id);if(old!=trace_values.end() && old->second==value)continue;
            if(attempted.event_seq>std::numeric_limits<uint64_t>::max()-transitions.size())throw std::runtime_error("E_EVENT_POSITION_OVERFLOW");
            LiveTransition t;t.position={attempted.tick,attempted.delta,attempted.event_seq+transitions.size()};t.signal_id=id;t.value=value.first;
            if(value.second>=0){t.state_code=uint8_t(value.second);t.contention=(value.second&ContentionFlag)!=0;t.valid=(value.second&1) && !t.contention;}
            transitions.push_back(t);next_values[id]=value;
        }
        auto fence=attempted;if(!transitions.empty())fence=transitions.back().position;
        trace->AppendBatch(transitions);auto revision=trace->Watermark().trace_revision+1;
        Json events=Json::array();for(const auto& t:transitions)events.push_back(TraceEventJson(t));
        Json transaction={{"kind","commit"},{"batch_id",std::to_string(commits+1)},{"position",Position(fence)},
            {"evaluation_position",Position(attempted)},{"effective_inputs",effective},{"outputs",outputs},{"evaluated",evaluated},
            {"trace_revision",std::to_string(revision)},{"trace_events",events},{"durability",contract.durability}};
        if(contract.board){transaction["board"]=board;transaction["faults"]=FaultsJson(nextFaults);transaction["board_inputs"]=candidate;transaction["clock_high"]=high;}
        log->Prepare(transaction);
        trace->Commit(fence,[&](const TraceCommit& water){
            auto previous_position=committed,previous_topology=topology_position;bool was_initialized=initialized;
            committed=fence;last_outputs.swap(outputs);trace_values.swap(next_values);last_board.swap(board);faults.swap(nextFaults);
            if(evaluated)initialized=true;else topology_position=fence;++commits;
            try{log->MarkCommitted(water);}
            catch(...){committed=previous_position;topology_position=previous_topology;last_outputs.swap(outputs);trace_values.swap(next_values);last_board.swap(board);faults.swap(nextFaults);initialized=was_initialized;--commits;throw;}
        });last_error.clear();return true;
    }
    bool EvaluateOne(uint64_t tick,const std::map<std::string,uint8_t>& candidate,bool high){
        if(!SetAttempted(tick))return false;
        std::vector<std::pair<PortBitRef,bool>> imports;Json effective=Json::array();
        TraceValues values;last_guard=Json::object();auto board=BoardFor(contract,candidate,high,last_outputs,faults,tick);
        for(const auto& [pad,e]:input_map){
            auto drivers=e.fixed_drivers;
            if(pad==contract.clock_pad)drivers.push_back({"clock",uint8_t(high?State::SHI:State::SLO)});
            else {auto it=candidate.find(e.target);if(it!=candidate.end())drivers.push_back({"manual",it->second});}
            auto resolved=ResolveM0(std::move(drivers));const auto& ref=pad_map.at(pad).ref;
            if(contract.board){const auto& node=board.at("nodes").at("pad."+std::to_string(pad));
                resolved.state=node.at("state_code");resolved.drivers.clear();for(const auto& d:node.at("drivers"))resolved.drivers.push_back({d.at("driver_id"),d.at("state")});}
            if(GuardInput(Direction::Input,resolved)==ImportDecision::PauseUnknownInput){
                state="Paused";reason="UnknownInput";last_error="E_UNKNOWN_INPUT";
                Json ds=Json::array();for(const auto& d:resolved.drivers)ds.push_back({{"driver_id",d.driver_id},{"state",d.state},{"label",StateName(d.state)}});
                last_guard={{"port_ref",contract.manifest.Label(ref)},{"package_pin",pad},{"state",resolved.state},
                    {"label",StateName(resolved.state)},{"drivers",ds},{"error",resolved.error},{"attempted_position",Position(attempted)},
                    {"last_committed_position",Position(committed)}};return false;
            }
            imports.push_back({ref,resolved.high()});auto label=contract.manifest.Label(ref);
            effective.push_back({{"port_ref",label},{"value",resolved.high()},{"state",resolved.state}});
            values["port:"+label]={resolved.high()?"1":"0",-1};
            values["pad:"+std::to_string(pad)]={resolved.high()?"1":"0",resolved.state};
        }
        try{
            log->BeginTransaction(std::to_string(commits+1),attempted,effective);
            for(const auto& [ref,value]:imports){model->WriteInput(ref,value);++writes;}
            model->Evaluate(attempted);++evaluations;
            Json outputs=Json::object();
            for(const auto& p:contract.manifest.ports)if(p.direction==Direction::Output)for(const auto& bit:p.bits){
                bool value=model->ReadOutput(bit);auto label=contract.manifest.Label(bit);outputs[label]=value;
                values["port:"+label]={value?"1":"0",-1};
                for(const auto& b:contract.bindings)if(b.ref==bit)
                    values["pad:"+std::to_string(b.package_pin)]={value?"1":"0",int(value?State::SHI:State::SLO)};
            }
            board=BoardFor(contract,candidate,high,outputs,faults,tick);
            return Publish(std::move(outputs),effective,std::move(values),true,candidate,high,faults,std::move(board));
        }catch(...){state="Faulted";reason="TransactionFailure";throw;}
    }
    bool Evaluate(uint64_t tick,const std::map<std::string,uint8_t>& candidate,bool high){
        for(uint32_t delta=0;delta<contract.max_delta;++delta){
            if(!EvaluateOne(tick,candidate,high))return false;
            if(!model->NeedsSettle())return true;
        }
        state="Faulted";reason="NonConvergent";last_error="E_NON_CONVERGENT";throw std::runtime_error(last_error);
    }
    bool ApplyPending(){
        if(!ApplyFaultPending())return false;
        if(pending.empty() && initialized)return true;
        auto candidate=manual;for(const auto& [target,p]:pending){
            if(p.pulse && p.duration>std::numeric_limits<uint64_t>::max()-committed.tick)throw std::runtime_error("E_DURATION_OVERFLOW");
            candidate[target]=p.value;
        }
        if(!Evaluate(committed.tick,candidate,clock_high)){
            for(const auto& [target,p]:pending)Decision(p.id,"blocked");return false;
        }
        manual=std::move(candidate);
        auto applied=std::move(pending);pending.clear();
        for(const auto& [target,p]:applied){
            if(p.pulse)pulses[target]={p.id,p.released,committed.tick+p.duration};
            Decision(p.id,"applied");auto& ack=acknowledgements.at(p.id);
            Record({{"kind","input_commit"},{"command_id",p.id},{"target",target},{"state",p.value},
                {"position",Position(committed)},{"batch_id",std::to_string(commits)},{"status","applied"},{"durability",contract.durability}});
            ack=Snapshot();ack["command_id"]=p.id;ack["status"]="applied";ack["position"]=Position(committed);ack["batch_id"]=std::to_string(commits);
            if(p.pulse)ack["release_tick"]=std::to_string(pulses.at(target).due);
        }
        return true;
    }
    bool CommitTopology(uint64_t tick,const FaultSet& next){
        if(!SetAttempted(tick))return false;
        try{auto board=BoardFor(contract,manual,clock_high,last_outputs,next,tick);
            log->BeginTransaction(std::to_string(commits+1),attempted,Json::array());
            return Publish(last_outputs,Json::array(),{},false,manual,clock_high,next,std::move(board));
        }catch(...){state="Faulted";reason="TransactionFailure";throw;}
    }
    bool ApplyFaultPending(){
        if(pending_faults.empty())return true;auto candidate=faults;
        for(const auto& [id,p]:pending_faults){if(p.fault)candidate[id]=*p.fault;else candidate.erase(id);}
        if(!CommitTopology(committed.tick,candidate))return false;
        auto done=std::move(pending_faults);pending_faults.clear();
        for(const auto& [id,p]:done){Decision(p.command_id,"applied");
            Record({{"kind","fault_commit"},{"command_id",p.command_id},{"fault_id",id},{"op",p.fault?"apply":"revoke"},{"position",Position(committed)},
                {"faults",FaultsJson(faults)},{"board",last_board},{"durability",contract.durability}});
            auto ack=Snapshot();ack["status"]="applied";ack["command_id"]=p.command_id;ack["fault_id"]=id;acknowledgements[p.command_id]=std::move(ack);
        }return true;
    }
    std::optional<uint64_t> NextFaultBoundary()const{
        std::optional<uint64_t> next;for(const auto& [id,f]:faults){
            if(f.start>committed.tick && (!next || f.start<*next))next=f.start;
            if(f.end && *f.end>committed.tick && (!next || *f.end<*next))next=*f.end;
        }return next;
    }
    bool NoClock(){state="Paused";reason="NoClockEdge";last_error="E_NO_CLOCK_EDGE";return false;}
    bool NextRising(){
        if(!contract.clock_enabled)return NoClock();
        auto start=committed.tick;
        for(size_t events=0;events<contract.max_step_events;++events){
            auto next=clock;auto edge_tick=next.Next();
            auto release=pulses.end();for(auto it=pulses.begin();it!=pulses.end();++it)
                if(release==pulses.end() || it->second.due<release->second.due)release=it;
            auto tick=release!=pulses.end()?std::min(edge_tick,release->second.due):edge_tick;
            auto faultTick=NextFaultBoundary();if(faultTick)tick=std::min(tick,*faultTick);
            if(tick<start || tick-start>contract.max_step_ticks)return NoClock();
            if(faultTick && *faultTick==tick){if(!CommitTopology(tick,faults))return false;
                Record({{"kind","fault_boundary"},{"position",Position(committed)},{"faults",FaultsJson(faults)},{"board",last_board}});continue;}
            if(release!=pulses.end() && release->second.due==tick){
                auto candidate=manual;std::vector<std::string> released;
                for(const auto& [target,pulse]:pulses)if(pulse.due==tick){candidate[target]=pulse.released;released.push_back(target);}
                if(!Evaluate(tick,candidate,clock_high))return false;
                manual=std::move(candidate);
                for(const auto& target:released){auto pulse=pulses.at(target);
                    Record({{"kind","decision"},{"command_id",pulse.id},{"status","pulse_released"},
                        {"target",target},{"position",Position(committed)}});pulses.erase(target);}
                continue; // data settles before a coincident clock edge
            }
            bool high=!clock_high;if(!Evaluate(tick,manual,high))return false;
            clock=next;clock_high=high;++edges;if(clock_high)return true;
        }return NoClock();
    }
    Json JournalSnapshot()const{
        const auto& digests=log->CurrentDigests();
        return {{"status","observed"},{"records",log->Records()},{"acks",acknowledgements},{"durability",contract.durability},
            {"digests",{{"circuit",digests.circuit},{"control",digests.control},{"algorithm",JournalDigestVersion}}}};
    }
    Json Handle(const Json& j){
        std::string op=j.at("op");
        if(op=="get"){auto result=Snapshot();result["status"]="observed";return result;}
        if(op=="journal")return JournalSnapshot();
        if(op=="trace.commits"){
            auto after=Number(j.value("after_revision",Json("0"))),through=Number(j.value("through_revision",Json(std::to_string(trace->Watermark().trace_revision))));
            auto limit=j.value("limit",size_t(64));if(!limit || limit>128 || after>through || through>trace->Watermark().trace_revision)throw std::runtime_error("E_TRACE_QUERY");
            Json batches=Json::array();auto next=after;
            for(const auto& r:log->Records())if(r.at("kind")=="commit"){
                auto revision=Number(r.at("trace_revision"));if(revision<=after || revision>through)continue;
                batches.push_back({{"revision",r.at("trace_revision")},{"position",r.at("position")},{"events",r.at("trace_events")}});next=revision;if(batches.size()==limit)break;
            }
            auto header=Header(contract);return {{"status","observed"},{"run_id",contract.run_id},{"generation",std::to_string(contract.generation)},
                {"signals",header.at("trace_signals")},{"batches",batches},{"next_revision",std::to_string(next)},{"complete",next==through}};
        }
        std::string id=j.at("command_id");if(id.empty())throw std::runtime_error("E_COMMAND_ID");
        auto old=requests.find(id);if(old!=requests.end()){
            if(old->second!=j)throw std::runtime_error("E_COMMAND_ID_REUSED");return acknowledgements.at(id);
        }
        if(requests.size()>=contract.max_commands && op!="stop")throw std::runtime_error("E_COMMAND_QUOTA");
        Pending intent;intent.id=id;
        std::optional<EdgeFault> faultIntent;std::string faultId;
        std::string target;
        if(op=="set" || op=="pulse"){
            target=j.at("target");
            if(!target_map.count(target) || target_map.at(target)==contract.clock_pad)throw std::runtime_error("E_INPUT_OWNERSHIP");
            int value=(op=="set"?j.at("state"):j.at("pressed_state")).get<int>();
            if(value<0 || value>255 || !ResolveM0({{"manual",uint8_t(value)}}).supported)throw std::runtime_error("E_STATE_UNSUPPORTED");
            intent.value=uint8_t(value);intent.pulse=op=="pulse";
            if(intent.pulse){
                int released=j.at("released_state").get<int>();intent.duration=Number(j.at("duration_ticks"));
                if(!intent.duration)throw std::runtime_error("E_DURATION_TICKS");
                if(released<0 || released>255 || !ResolveM0({{"manual",uint8_t(released)}}).supported)throw std::runtime_error("E_STATE_UNSUPPORTED");
                intent.released=uint8_t(released);
            }
            auto previous=pending.find(target);
            if(pulses.count(target) || (previous!=pending.end() && (intent.pulse || previous->second.pulse)))
                throw std::runtime_error("E_STIMULUS_CONFLICT");
        }else if(op=="fault.apply" || op=="fault.revoke"){
            if(!contract.board)throw std::runtime_error("E_BOARD_REQUIRED");faultId=j.at("fault_id");if(faultId.empty() || faultId.size()>128)throw std::runtime_error("E_FAULT_ID");
            if(op=="fault.apply"){
                if(j.value("effect",std::string("open"))!="open")throw std::runtime_error("E_FAULT_UNSUPPORTED");
                EdgeFault f{faultId,j.at("edge_id"),committed.tick,{}};
                auto trigger=j.value("trigger",Json{{"type","permanent"}});auto type=trigger.at("type").get<std::string>();
                if(type=="transient"){f.start=Number(trigger.at("start_tick"));f.end=Number(trigger.at("end_tick"));}
                else if(type!="permanent")throw std::runtime_error("E_FAULT_TRIGGER");
                if(f.start<committed.tick || (f.end && *f.end<=f.start))throw std::runtime_error("E_FAULT_WINDOW");
                auto all=faults;for(const auto& [key,p]:pending_faults){if(p.fault)all[key]=*p.fault;else all.erase(key);}all[faultId]=f;
                contract.board->ValidateFaults(all);faultIntent=f;
                if(pending_faults.size()>=128 && !pending_faults.count(faultId))throw std::runtime_error("E_FAULT_QUOTA");
            }else if(!faults.count(faultId) && !pending_faults.count(faultId))throw std::runtime_error("E_FAULT_UNKNOWN");
        }else if(op!="step" && op!="play" && op!="pause" && op!="stop")throw std::runtime_error("E_COMMAND_UNKNOWN");
        if(op=="step" && state=="Running")throw std::runtime_error("E_PAUSE_REQUIRED");
        requests[id]=j;acknowledgements[id]={{"command_id",id},{"status","pending"}};
        Record({{"kind","intent"},{"command_id",id},{"request",j},{"status","pending"}});
        std::string status="applied";
        if(op=="set" || op=="pulse"){
            auto previous=pending.find(target);if(previous!=pending.end())Decision(previous->second.id,"superseded",id);
            pending[target]=intent;status="pending";
            if(state=="Running")status=ApplyPending()?"applied":"blocked";
        }else if(op=="fault.apply" || op=="fault.revoke"){
            auto old=pending_faults.find(faultId);if(old!=pending_faults.end())Decision(old->second.command_id,"superseded",id);
            pending_faults[faultId]={id,faultIntent};status="pending";
            if(state=="Running")status=ApplyFaultPending()?"applied":"blocked";
        }else if(op=="step"){
            if(ApplyPending() && NextRising()){state="Paused";reason="StepComplete";}else status="blocked";
        }else if(op=="play"){
            if(!ApplyPending())status="blocked";
            else if(!contract.clock_enabled){NoClock();status="blocked";}
            else {state="Running";reason.clear();}
        }else if(op=="pause"){
            state="Paused";if(reason!="UnknownInput" && reason!="ResourceLimit")reason="User";
        }else if(op=="stop"){
            for(const auto& [name,p]:pending)Decision(p.id,"cancelled",id);pending.clear();
            for(const auto& [name,p]:pending_faults)Decision(p.command_id,"cancelled",id);pending_faults.clear();
            for(const auto& [name,pulse]:pulses)Record({{"kind","decision"},{"command_id",pulse.id},{"status","pulse_cancelled"},{"by",id}});
            pulses.clear();++final_calls;
            try{model->Finalize();}catch(...){state="Faulted";reason="AdapterFailure";throw;}
            state="Stopped";reason="User";trace->Seal();
        }
        Record({{"kind","control"},{"command_id",id},{"op",op},{"status",status},{"position",Position(committed)}});
        if(op=="stop")log->Close();
        auto ack=Snapshot();ack["command_id"]=id;ack["status"]=status;acknowledgements[id]=ack;return ack;
    }
    void Run(){
        std::shared_ptr<Request> active;
        try{
            trace->BindWriterToCurrentThread();model=factory();if(!model)throw std::runtime_error("E_MODEL_FACTORY");
            Evaluate(0,manual,false);ready.set_value();
            while(state!="Stopped"){
                {
                    std::unique_lock lock(mutex);
                    if(queue.empty() && state!="Running")condition.wait(lock,[&]{return !queue.empty();});
                    if(!queue.empty()){active=queue.front();queue.pop_front();}
                }
                if(!active){NextRising();continue;}
                try{active->reply.set_value(Handle(active->value));}
                catch(const std::exception& e){
                    if(state=="Faulted"){last_error=e.what();auto failure=Snapshot();failure["status"]="rejected";active->reply.set_value(failure);}
                    else active->reply.set_value({{"status","rejected"},{"error",e.what()}});
                    if(state=="Faulted"){active.reset();throw;}
                }
                active.reset();
            }
        }catch(...){
            auto failure=std::current_exception();
            try{ready.set_exception(failure);}catch(...){}
            if(active)try{active->reply.set_exception(failure);}catch(...){}
            if(model && !final_calls)try{++final_calls;model->Finalize();}catch(...){}
        }
        // Model destruction is worker-owned too. Stop snapshots were captured
        // before destruction; queued callers always receive a terminal reply.
        try{if(state!="Stopped"){state="Faulted";trace->Seal();}log->Close();}catch(...){}
        try{terminal_snapshot=Snapshot();terminal_snapshot["status"]="observed";}
        catch(...){terminal_snapshot={{"state","Faulted"},{"status","observed"}};}
        model.reset();std::lock_guard lock(mutex);finished=true;
        for(auto& r:queue){
            if(r->value.value("op","")=="get")r->reply.set_value(terminal_snapshot);
            else if(r->value.value("op","")=="journal")r->reply.set_value(JournalSnapshot());
            else if(r->value.value("op","")=="trace.commits")r->reply.set_value(Handle(r->value));
            else {
                auto id=r->value.value("command_id","");auto original=requests.find(id);
                if(original!=requests.end() && original->second==r->value)r->reply.set_value(acknowledgements.at(id));
                else r->reply.set_value({{"status","rejected"},{"error","E_SESSION_STOPPED"}});
            }
        }queue.clear();
    }
};
VirtualSession::VirtualSession(SessionContract c,Factory f){if(c.board && c.board->UsesState13()!=c.phase2)throw std::runtime_error("E_RESOLVER_MODE");if(c.phase1 || c.phase2)phase1_=std::make_unique<Phase1Session>(std::move(c),std::move(f));else impl_=std::make_unique<Impl>(std::move(c),std::move(f));}
VirtualSession::~VirtualSession()=default;
std::shared_ptr<const LiveTraceStore> VirtualSession::Trace()const{return phase1_?phase1_->Trace():impl_->trace;}
Json VirtualSession::Submit(Json j){
    if(j.value("command_id","").rfind("__session_",0)==0)return {{"status","rejected"},{"error","E_COMMAND_ID_RESERVED"}};
    return phase1_?phase1_->Submit(std::move(j)):impl_->Submit(std::move(j));
}
Json VirtualSession::ReplayCommitted(const SessionContract& c,Factory factory,const Json& records){
    if(c.phase1 || c.phase2)return Phase1Session::Replay(c,std::move(factory),records);
    std::promise<Json> reply;auto future=reply.get_future();
    std::thread worker([&]{
        std::unique_ptr<IDutAdapter> model;bool finalized=false;
        try{
            auto manifest=PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(c.manifest));
            if(!manifest.ok() || !ValidateElectrical(c.electrical,c.identity,c.manifest,c.bindings).empty())
                throw std::runtime_error("E_REPLAY_CONTRACT");
            if(!records.is_array() || records.empty() || records.at(0)!=Header(c))throw std::runtime_error("E_REPLAY_FINGERPRINT");
            model=factory();if(!model)throw std::runtime_error("E_MODEL_FACTORY");
            EventPosition previous;size_t count=0;Json outputs=Json::object();
            TraceValues replayValues;std::map<std::string,uint32_t> ids;
            for(const auto& signal:Signals(c))ids[signal.name]=signal.id;
            auto replayTrace=std::make_shared<LiveTraceStore>(c.run_id,c.generation,Signals(c),c.trace_limits);replayTrace->BindWriterToCurrentThread();
            for(const auto& record:records){
                if(record.at("kind")!="commit")continue;
                auto p=record.at("position");EventPosition position;
                size_t used=0;auto tick=p.at("tick").get<std::string>();position.tick=std::stoull(tick,&used);
                if(used!=tick.size())throw std::runtime_error("E_REPLAY_POSITION");
                auto seq=p.at("event_seq").get<std::string>();position.event_seq=std::stoull(seq,&used);
                if(used!=seq.size())throw std::runtime_error("E_REPLAY_POSITION");position.delta=p.at("delta");
                if(position.event_seq<=previous.event_seq || (count && (position.tick<previous.tick ||
                   (position.tick==previous.tick && position.delta<=previous.delta))))throw std::runtime_error("E_REPLAY_POSITION");
                std::vector<std::pair<PortBitRef,bool>> imports;std::set<std::string> seen;
                for(const auto& input:record.at("effective_inputs")){
                    auto label=input.at("port_ref").get<std::string>();const PortBitRef* ref=nullptr;
                    for(const auto& port:c.manifest.ports)if(port.direction==Direction::Input)
                        for(const auto& b:port.bits)if(c.manifest.Label(b)==label)ref=&b;
                    if(!ref || !seen.insert(label).second || !input.at("value").is_boolean())throw std::runtime_error("E_REPLAY_INPUT");
                    auto value=input.at("value").get<bool>();auto r=ResolveM0({{"replay",input.at("state").get<uint8_t>()}});
                    if(!r.known() || r.high()!=value)throw std::runtime_error("E_REPLAY_INPUT");imports.push_back({*ref,value});
                }
                bool evaluated=record.value("evaluated",true);
                size_t expected=0;if(evaluated)for(const auto& port:c.manifest.ports)if(port.direction==Direction::Input)expected+=port.bits.size();
                if(imports.size()!=expected)throw std::runtime_error("E_REPLAY_INPUT");
                auto eval=record.value("evaluation_position",record.at("position"));
                EventPosition eval_position{Number(eval.at("tick")),eval.at("delta").get<uint32_t>(),Number(eval.at("event_seq"))};
                if(eval_position.tick!=position.tick || eval_position.delta!=position.delta || eval_position.event_seq!=previous.event_seq+1)throw std::runtime_error("E_REPLAY_POSITION");
                std::map<std::string,uint8_t> manual;FaultSet faultSet;
                if(c.board){
                    for(const auto& e:c.inputs){const auto& value=record.at("board_inputs").at(e.target);int v=value.get<int>();
                        if(v<0 || v>255 || !ResolveM0({{"manual",uint8_t(v)}}).supported)throw std::runtime_error("E_REPLAY_BOARD_INPUT");manual[e.target]=uint8_t(v);}
                    if(record.at("board_inputs").size()!=manual.size())throw std::runtime_error("E_REPLAY_BOARD_INPUT");
                    faultSet=ParseFaults(record.at("faults"));auto before=BoardFor(c,manual,record.at("clock_high"),outputs,faultSet,position.tick);
                    for(const auto& in:record.at("effective_inputs"))for(const auto& b:c.bindings)if(c.manifest.Label(b.ref)==in.at("port_ref").get<std::string>()){
                        const auto& node=before.at("nodes").at("pad."+std::to_string(b.package_pin));
                        if(node.at("valid")!=true || node.at("state_code")!=in.at("state") || (node.at("logic")=="1")!=in.at("value").get<bool>())throw std::runtime_error("E_REPLAY_INPUT_TOPOLOGY");
                    }
                }
                for(const auto& [ref,value]:imports)model->WriteInput(ref,value);
                if(evaluated){model->Evaluate(eval_position);outputs=Json::object();
                    for(const auto& port:c.manifest.ports)if(port.direction==Direction::Output)
                        for(const auto& b:port.bits)outputs[c.manifest.Label(b)]=model->ReadOutput(b);}
                if(outputs!=record.at("outputs"))throw std::runtime_error("E_REPLAY_DIVERGENCE");
                if(c.board){
                    auto board=BoardFor(c,manual,record.at("clock_high"),outputs,faultSet,position.tick);
                    if(board!=record.at("board"))throw std::runtime_error("E_REPLAY_BOARD_DIVERGENCE");
                    TraceValues changed;
                    if(evaluated){
                        for(const auto& in:record.at("effective_inputs")){
                            auto label=in.at("port_ref").get<std::string>();changed["port:"+label]={in.at("value").get<bool>()?"1":"0",-1};
                            for(const auto& b:c.bindings)if(c.manifest.Label(b.ref)==label){const auto& node=board.at("nodes").at("pad."+std::to_string(b.package_pin));
                                if(node.at("valid")!=true || node.at("state_code")!=in.at("state") || (node.at("logic")=="1")!=in.at("value").get<bool>())throw std::runtime_error("E_REPLAY_INPUT_TOPOLOGY");}
                        }
                        for(const auto& [label,value]:outputs.items())changed["port:"+label]={value.get<bool>()?"1":"0",-1};
                    }
                    AddBoardValues(board,changed,ids);std::vector<LiveTransition> events;Json expectedEvents=Json::array();
                    for(const auto& [name,value]:changed){auto old=replayValues.find(name);if(old!=replayValues.end() && old->second==value)continue;
                        LiveTransition t{{position.tick,eval_position.delta,eval_position.event_seq+events.size()},ids.at(name),value.first};
                        if(value.second>=0){t.state_code=uint8_t(value.second);t.contention=bool(value.second&ContentionFlag);t.valid=(value.second&1) && !t.contention;}
                        expectedEvents.push_back(TraceEventJson(t));events.push_back(t);replayValues[name]=value;
                    }
                    auto fence=events.empty()?eval_position:events.back().position;
                    if(expectedEvents!=record.at("trace_events") || !(fence==position))throw std::runtime_error("E_REPLAY_TRACE_DIVERGENCE");
                    replayTrace->AppendBatch(events);auto water=replayTrace->Commit(fence);
                    if(std::to_string(water.trace_revision)!=record.at("trace_revision").get<std::string>())throw std::runtime_error("E_REPLAY_TRACE_REVISION");
                }
                previous=position;++count;
            }
            finalized=true;model->Finalize();auto digests=DurableJournal::Digests(records);
            reply.set_value({{"status","passed"},{"matched_commits",count},{"outputs",outputs},{"board_trace_verified",bool(c.board)},
                {"circuit_digest",digests.circuit},{"control_digest",digests.control},{"durability",c.durability}});
        }catch(...){if(model && !finalized)try{model->Finalize();}catch(...){}reply.set_exception(std::current_exception());}
    });
    try{auto result=future.get();worker.join();return result;}catch(...){worker.join();throw;}
}
}
