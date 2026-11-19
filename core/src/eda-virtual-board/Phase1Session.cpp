#include "Phase1Session.h"
#include "Phase1Peripherals.h"
#include "DurableJournal.h"
#include "ElectricalValidator.h"
#include "PortManifestBuilder.h"
#include "eda-platform/Sha256.h"
#include <condition_variable>
#include <future>
#include <thread>
#include <mutex>
#include <atomic>
#include <set>
namespace eda::virtual_board {
namespace {
Json Pos(const EventPosition& p){return {{"tick",std::to_string(p.tick)},{"delta",p.delta},{"event_seq",std::to_string(p.event_seq)}};}
std::string Hash(const Json& j){auto s=j.dump();return platform::Sha256Hex(s.data(),s.size());}
struct RationalClock {
    uint64_t denominator,floor=0,remainder=0;
    explicit RationalClock(uint64_t hz):denominator(2*hz){if(!hz || hz>500000000000ULL)throw std::runtime_error("E_CLOCK_RESOLUTION");}
    void Restart(uint64_t tick){floor=tick;remainder=0;}
    uint64_t Next(){auto carry=remainder+1000000000000ULL%denominator;floor=P1Add(floor,1000000000000ULL/denominator+carry/denominator);remainder=carry%denominator;return P1Add(floor,remainder>=denominator/2?1:0);}
};
std::vector<LiveSignal> Signals(const SessionContract& c){
    std::vector<LiveSignal> signals;std::set<std::string> names;auto add=[&](const std::string& name,const std::string& kind){if(names.insert(name).second)signals.push_back({uint32_t(signals.size()),name,1,kind});};
    for(const auto& p:c.manifest.ports)for(const auto& bit:p.bits)add("port:"+c.manifest.Label(bit),"port");for(const auto& b:c.bindings)add("pad:"+std::to_string(b.package_pin),"pad");
    for(const auto& node:c.board->Nodes())add(node,"endpoint");for(const auto& edge:c.board->Edges())add(edge.id,"connection");for(const auto& comp:c.board->Descriptor().at("components"))if(comp.at("model")=="sigflow.led@1")add(comp.at("id").get<std::string>()+".lit","led");return signals;
}
Json Header(const SessionContract& c){
    Json bindings=Json::array(),inputs=Json::array(),signals=Json::array();for(const auto& b:c.bindings)bindings.push_back({{"ref",c.manifest.Label(b.ref)},{"pad",b.package_pin},{"io_type",b.io_type},{"pull_up",b.pull_up},{"pull_down",b.pull_down}});
    for(const auto& i:c.inputs){Json ds=Json::array();for(const auto& d:i.fixed_drivers)ds.push_back({{"id",d.driver_id},{"state",d.state}});inputs.push_back({{"target",i.target},{"pad",i.package_pin},{"state",i.initial_manual_state},{"drivers",ds}});}
    for(const auto& s:Signals(c))signals.push_back({{"id",s.id},{"name",s.name},{"width",s.width},{"kind",s.kind}});
    return {{"kind","run_header"},{"schema","sigflow.session-journal.p1.v1"},{"scheduler_version","sigflow.scheduler.p1.v1"},{"digest_version","sigflow.sha256-chain.p1.v1"},{"rng","pcg32-v1"},
        {"source_sha256",c.manifest.source_sha256},{"adapter_sha256",c.adapter_sha256},{"electrical_sha256",c.electrical_sha256},{"bindings_sha256",Hash(bindings)},{"inputs_sha256",Hash(inputs)},
        {"board_sha256",c.board->Fingerprint()},{"resolver_version",ResolverVersion},{"fault_profile",c.fault_profile},{"integration_window_ticks",std::to_string(c.integration_window_ticks)},
        {"tick_ps","1"},{"clock_pad",c.clock_pad},{"frequency_hz",std::to_string(c.frequency_hz)},{"clock_enabled",c.clock_enabled},{"max_delta",c.max_delta},
        {"trace_run_id",c.run_id},{"trace_generation",std::to_string(c.generation)},{"trace_signals",signals}};
}
struct Runtime {
    Phase1Faults faults;Phase1Peripherals peripherals;RationalClock clock;
    std::map<std::string,uint8_t> manual;std::map<std::string,std::pair<uint64_t,uint8_t>> pulses;
    uint64_t tick=0;std::optional<uint64_t> clock_due;bool clock_high=false,stopped=false;
    Json outputs=Json::object(),board=Json::object();
    explicit Runtime(const SessionContract& c):faults(*c.board),peripherals(*c.board,c.integration_window_ticks),clock(c.frequency_hz){
        if(!c.fault_profile.empty())faults.Load(c.fault_profile,0);for(const auto& input:c.inputs)manual[input.target]=input.initial_manual_state;
        stopped=!c.clock_enabled || bool(faults.ClockHold(0));if(auto hold=faults.ClockHold(0))clock_high=*hold;if(!stopped)clock_due=clock.Next();
    }
};
struct Engine {
    const SessionContract& c;Runtime runtime;std::unique_ptr<IDutAdapter> model;
    EventPosition committed,attempted;Json guard=Json::object();uint64_t commits=0,writes=0,evals=0,finals=0;
    std::map<std::string,uint32_t> ids;std::map<std::string,std::pair<std::string,int>> values;
    explicit Engine(const SessionContract& contract,VirtualSession::Factory factory):c(contract),runtime(c),model(factory()){
        if(!model)throw std::runtime_error("E_MODEL_FACTORY");for(const auto& s:Signals(c))ids[s.name]=s.id;
    }
    ~Engine(){if(model && !finals)try{Finalize();}catch(...) {}}
    void Finalize(){if(!finals){++finals;model->Finalize();}}
    Json Resolve(const Runtime& state)const{
        std::map<std::string,std::vector<Driver>> drivers;drivers[c.board->ClockEndpoint()].push_back({"oscillator",uint8_t(state.clock_high?State::SHI:State::SLO)});
        for(const auto& input:c.inputs){drivers["pad."+std::to_string(input.package_pin)]=input.fixed_drivers;if(input.package_pin!=c.clock_pad)drivers[c.board->ControlEndpoint(input.package_pin)].push_back({"contact",state.manual.at(input.target)});}
        for(const auto& b:c.bindings){auto node="pad."+std::to_string(b.package_pin),label=c.manifest.Label(b.ref);if(b.pull_up)drivers[node].push_back({"cst.pullup",uint8_t(State::WHI)});if(b.pull_down)drivers[node].push_back({"cst.pulldown",uint8_t(State::WLO)});
            if(c.manifest.Find(b.ref.port_id)->direction==Direction::Output && state.outputs.contains(label))drivers[node].push_back({"dut:"+label,uint8_t(state.outputs.at(label).get<bool>()?State::SHI:State::SLO)});}
        state.peripherals.AddDrivers(drivers);FaultSet opens;state.faults.Transform(drivers,opens,state.tick);return c.board->Resolve(drivers,opens,state.tick);
    }
    void ValidateAction(const Json& a,const Runtime& state)const{
        auto op=a.at("op").get<std::string>();if(op=="profile.load"){auto copy=state.faults;copy.Load(a.at("profile"),state.tick);}
        else if(op=="fault.enable" || op=="fault.revoke"){auto copy=state.faults;copy.Enable(a.at("fault_id"),op=="fault.enable"?a.value("enabled",true):false,state.tick);}
        else if(op=="fault.apply"){auto copy=state.faults;copy.Upsert(LegacyOpen(a,state.tick),state.tick);}
        else if(op=="set" || op=="pulse"){
            auto target=a.at("target").get<std::string>();const InputEndpoint* input=nullptr;for(const auto& i:c.inputs)if(i.target==target)input=&i;if(!input || input->package_pin==c.clock_pad || !c.board->ControlEndpoint(input->package_pin).ends_with(".contact"))throw std::runtime_error("E_INPUT_OWNERSHIP");
            int v=op=="set"?a.at("state").get<int>():a.at("pressed_state").get<int>();if(v<0 || v>255 || !ResolveM0({{"manual",uint8_t(v)}}).supported)throw std::runtime_error("E_STATE_UNSUPPORTED");
            if(op=="pulse"){auto duration=P1Tick(a.at("duration_ticks"));int release=a.at("released_state");if(!duration || release<0 || release>255 || !ResolveM0({{"manual",uint8_t(release)}}).supported || state.pulses.count(target))throw std::runtime_error("E_STIMULUS_CONFLICT");P1Add(state.tick,duration);}
        }else if(op=="dip.set"){auto copy=state.peripherals;copy.SetDip(a.at("target"),a.at("value"));}
        else if(op=="uart.send"){std::vector<uint8_t> bytes;for(const auto& b:a.at("bytes")){if(!b.is_number_integer() || b.get<int>()<0 || b.get<int>()>255)throw std::runtime_error("E_UART_BYTE");bytes.push_back(b.get<uint8_t>());}auto copy=state.peripherals;copy.Send(a.at("target"),bytes,state.tick);}
        else throw std::runtime_error("E_COMMAND_UNKNOWN");
    }
    Json LegacyOpen(const Json& a,uint64_t tick)const{if(a.value("effect",std::string("open"))!="open")throw std::runtime_error("E_FAULT_UNSUPPORTED");auto trigger=a.value("trigger",Json{{"type","permanent"}});Json tr={{"kind",trigger.at("type")},{"start_tick",std::to_string(tick)}};if(trigger.at("type")=="transient"){tr["start_tick"]=trigger.at("start_tick");tr["end_tick"]=trigger.at("end_tick");}
        return {{"id",a.at("fault_id")},{"enabled",true},{"visibility","student_visible"},{"hook","topology.transform"},{"target",{{"kind","connection"},{"id",a.at("edge_id")}}},{"effect",{{"type","open"}}},{"trigger",tr}};}
    void Action(Runtime& state,const Json& a){
        auto op=a.at("op").get<std::string>();ValidateAction(a,state);
        if(op=="profile.load")state.faults.Load(a.at("profile"),state.tick);
        else if(op=="fault.enable" || op=="fault.revoke")state.faults.Enable(a.at("fault_id"),op=="fault.enable"?a.value("enabled",true):false,state.tick);
        else if(op=="fault.apply")state.faults.Upsert(LegacyOpen(a,state.tick),state.tick);
        else if(op=="set" || op=="pulse"){
            std::string target=a.at("target");uint8_t value=op=="set"?a.at("state").get<uint8_t>():a.at("pressed_state").get<uint8_t>();state.manual[target]=value;
            state.faults.InputIntent(target,value,state.tick);if(op=="pulse")state.pulses[target]={P1Add(state.tick,P1Tick(a.at("duration_ticks"))),a.at("released_state").get<uint8_t>()};
        }else if(op=="dip.set")state.peripherals.SetDip(a.at("target"),a.at("value"));
        else if(op=="uart.send"){std::vector<uint8_t> bytes;for(const auto& b:a.at("bytes"))bytes.push_back(b.get<uint8_t>());state.peripherals.Send(a.at("target"),bytes,state.tick);}
    }
    Json Checkpoint(const Runtime& s)const{return {{"faults",s.faults.Snapshot(s.tick)},{"peripherals",s.peripherals.Snapshot()},{"clock_high",s.clock_high},{"clock_stopped",s.stopped},{"next_clock_tick",s.clock_due?Json(std::to_string(*s.clock_due)):Json(nullptr)},{"clock_remainder",std::to_string(s.clock.remainder)},{"clock_floor",std::to_string(s.clock.floor)}};}
    std::optional<uint64_t> NextTick()const{
        std::optional<uint64_t> next;auto add=[&](std::optional<uint64_t> due){if(due && *due>runtime.tick && (!next || *due<*next))next=due;};add(runtime.clock_due);add(runtime.faults.NextDeadline(runtime.tick));add(runtime.peripherals.NextDeadline(runtime.tick));for(const auto& [target,p]:runtime.pulses)add(p.first);return next;
    }
    Json Event(uint64_t tick,bool evaluate,const std::string& cause,Json actions=Json::array())const{return {{"tick",std::to_string(tick)},{"evaluate",evaluate},{"cause",cause},{"actions",actions}};}
    bool Transact(const Json& spec,const std::function<void(const Json&,const std::vector<LiveTransition>&)>& publish){
        auto tick=P1Tick(spec.at("tick"));bool evaluated=spec.at("evaluate");std::string cause=spec.at("cause");
        if(tick<runtime.tick || (tick==runtime.tick && commits && committed.delta+1>=c.max_delta))throw std::runtime_error("E_NON_CONVERGENT");
        if(cause=="initial" && commits)throw std::runtime_error("E_P1_INITIAL_ORDER");if(cause=="event" && NextTick()!=std::optional<uint64_t>(tick))throw std::runtime_error("E_P1_EVENT_ORDER");
        if(cause=="target" && NextTick() && *NextTick()<tick)throw std::runtime_error("E_P1_SKIPPED_EVENT");
        Runtime next=runtime;next.faults.ClearEvents();next.peripherals.Advance(tick);next.tick=tick;
        if(cause=="event"){
            for(auto it=next.pulses.begin();it!=next.pulses.end();)if(it->second.first==tick){next.manual[it->first]=it->second.second;next.faults.InputIntent(it->first,it->second.second,tick,false);it=next.pulses.erase(it);}else ++it;
        }
        for(const auto& action:spec.at("actions"))Action(next,action);
        next.faults.Advance(tick);auto hold=next.faults.ClockHold(tick);bool stopped=!c.clock_enabled || bool(hold);
        if(hold)next.clock_high=*hold;
        if(stopped){next.stopped=true;next.clock_due.reset();}
        else if(runtime.stopped){next.stopped=false;next.clock.Restart(tick);next.clock_due=next.clock.Next();}
        else if(cause=="event" && runtime.clock_due==std::optional<uint64_t>(tick)){next.clock_high=!runtime.clock_high;next.clock_due=next.clock.Next();}
        attempted={tick,tick==runtime.tick && commits?uint32_t(committed.delta+1):0,committed.event_seq+1};guard=Json::object();
        Json before=Resolve(next),effective=Json::array();std::vector<std::pair<PortBitRef,bool>> imports;
        if(evaluated){for(const auto& input:c.inputs){const auto& node=before.at("nodes").at("pad."+std::to_string(input.package_pin));const PinBindingV2* binding=nullptr;for(const auto& b:c.bindings)if(b.package_pin==input.package_pin)binding=&b;
                if(!binding)throw std::runtime_error("E_ENDPOINT_BINDING");if(node.at("valid")!=true){guard={{"port_ref",c.manifest.Label(binding->ref)},{"package_pin",input.package_pin},{"state",node.at("state_code")},{"label",StateName(node.at("state_code"))},{"drivers",node.at("drivers")},{"attempted_position",Pos(attempted)},{"last_committed_position",Pos(committed)}};return false;}
                bool high=node.at("logic")=="1";imports.push_back({binding->ref,high});effective.push_back({{"port_ref",c.manifest.Label(binding->ref)},{"value",high},{"state",node.at("state_code")}});}
            for(const auto& [ref,value]:imports){model->WriteInput(ref,value);++writes;}model->Evaluate(attempted);++evals;
            if(model->NeedsSettle())throw std::runtime_error("E_NON_CONVERGENT: external settle adapter requires P1 support");
            next.outputs=Json::object();for(const auto& port:c.manifest.ports)if(port.direction==Direction::Output)for(const auto& ref:port.bits)next.outputs[c.manifest.Label(ref)]=model->ReadOutput(ref);
        }
        next.board=Resolve(next);next.peripherals.Observe(next.board,tick);
        std::map<std::string,std::pair<std::string,int>> changed;
        if(evaluated){for(const auto& input:effective)changed["port:"+input.at("port_ref").get<std::string>()]={input.at("value").get<bool>()?"1":"0",-1};for(const auto& [label,value]:next.outputs.items())changed["port:"+label]={value.get<bool>()?"1":"0",-1};}
        for(const auto& [name,node]:next.board.at("nodes").items()){changed[name]={node.at("logic"),node.at("state_code")};if(name.starts_with("pad.") && ids.count("pad:"+name.substr(4)))changed["pad:"+name.substr(4)]=changed[name];}
        for(const auto& [name,closed]:next.board.at("wires").items())changed[name]={closed.get<bool>()?"1":"0",-1};for(const auto& [name,led]:next.board.at("leds").items())changed[name+".lit"]={led.at("lit").get<bool>()?"1":"0",-1};
        auto nextValues=values;std::vector<LiveTransition> events;Json traceEvents=Json::array();
        for(const auto& [name,value]:changed){auto it=values.find(name);if(it!=values.end() && it->second==value)continue;LiveTransition event{{tick,attempted.delta,attempted.event_seq+events.size()},ids.at(name),value.first};
            if(value.second>=0){event.state_code=uint8_t(value.second);event.contention=(value.second&ContentionFlag)!=0;event.valid=(value.second&1) && !event.contention;}events.push_back(event);traceEvents.push_back(TraceEventJson(event));nextValues[name]=value;}
        auto fence=events.empty()?attempted:events.back().position;
        Json transaction={{"kind","commit"},{"batch_id",std::to_string(commits+1)},{"position",Pos(fence)},{"evaluation_position",Pos(attempted)},{"effective_inputs",effective},{"outputs",next.outputs},{"evaluated",evaluated},{"trace_revision",std::to_string(commits+1)},{"trace_events",traceEvents},{"durability",c.durability},{"board",next.board},{"faults",Json::array()},{"p1",Checkpoint(next)},{"p1_event",spec}};
        publish(transaction,events);runtime=std::move(next);values=std::move(nextValues);committed=fence;++commits;return true;
    }
};
void Validate(const SessionContract& c){
    if(!c.board || c.run_id.empty() || !c.generation || !c.max_delta || !c.max_commits || !c.max_commands || !c.max_step_events || !c.max_step_ticks || !c.required_clock || c.frequency_hz!=c.board->Frequency() || c.clock_pad!=c.board->ClockPad())throw std::runtime_error("E_P1_CONTRACT");
    if(!PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(c.manifest)).ok() || !ValidateElectrical(c.electrical,c.identity,c.manifest,c.bindings).empty())throw std::runtime_error("E_P1_BINDING");
    std::set<int> pads;std::set<std::string> refs,targets;for(const auto& b:c.bindings)if(!c.manifest.Contains(b.ref) || !pads.insert(b.package_pin).second || !refs.insert(c.manifest.Label(b.ref)).second || !c.board->Nodes().count("pad."+std::to_string(b.package_pin)))throw std::runtime_error("E_SESSION_BINDING");
    std::set<int> inputPads;for(const auto& i:c.inputs){const PinBindingV2* binding=nullptr;for(const auto& b:c.bindings)if(b.package_pin==i.package_pin)binding=&b;if(!binding || c.manifest.Find(binding->ref.port_id)->direction!=Direction::Input || !inputPads.insert(i.package_pin).second || !targets.insert(i.target).second || !ResolveM0(i.fixed_drivers).supported || !ResolveM0({{"manual",i.initial_manual_state}}).supported)throw std::runtime_error("E_ENDPOINT_BINDING");if(i.package_pin!=c.clock_pad)c.board->ControlEndpoint(i.package_pin);}
    bool clockBound=false;for(const auto& b:c.bindings)if(b.ref==*c.required_clock && b.package_pin==c.clock_pad)clockBound=true;if(!clockBound)throw std::runtime_error("E_CLOCK_BINDING");
    for(const auto& port:c.manifest.ports)for(const auto& ref:port.bits){bool bound=false;for(const auto& b:c.bindings)if(b.ref==ref){bound=true;if(port.direction==Direction::Input && !inputPads.count(b.package_pin))throw std::runtime_error("E_ENDPOINT_MISSING");}if(!bound && (port.direction!=Direction::Output || !c.allow_unbound_outputs))throw std::runtime_error("E_MISSING_BINDING");}
    auto faults=Phase1Faults(*c.board);if(!c.fault_profile.empty())faults.Load(c.fault_profile,0);
}
}
struct Phase1Session::Impl {
    struct Request{Json command;std::promise<Json> reply;};
    SessionContract c;VirtualSession::Factory factory;std::unique_ptr<Engine> engine;std::unique_ptr<DurableJournal> journal;std::shared_ptr<LiveTraceStore> trace;
    std::thread worker;std::mutex mutex;std::condition_variable condition;std::deque<std::shared_ptr<Request>> queue;std::promise<void> ready;std::atomic<bool> finished{false};
    std::string state="Paused",reason="Ready",error;Json terminal;
    std::map<std::string,Json> requests,acks,pendingInputs,pendingMutations;
    explicit Impl(SessionContract contract,VirtualSession::Factory f):c(std::move(contract)),factory(std::move(f)){
        Validate(c);trace=std::make_shared<LiveTraceStore>(c.run_id,c.generation,Signals(c),c.trace_limits);journal=std::make_unique<DurableJournal>(c.durability,c.journal_path);journal->AppendRecord(Header(c));auto future=ready.get_future();worker=std::thread([this]{Run();});try{future.get();}catch(...){worker.join();throw;}
    }
    ~Impl(){if(worker.joinable()){if(!finished.load())try{Submit({{"op","stop"},{"command_id","__session_destructor_stop"}});}catch(...){}worker.join();}}
    Json TerminalReply(const Json& command)const{auto op=command.value("op",std::string());if(op=="get")return terminal;if(op=="journal")return Audit();if(op=="trace.commits")return TracePage(command);
        auto id=command.value("command_id",std::string());auto previous=requests.find(id);if(previous!=requests.end()){if(previous->second!=command)return {{"status","rejected"},{"error","E_COMMAND_ID_REUSED"}};auto ack=acks.find(id);if(ack!=acks.end())return ack->second;}return {{"status","rejected"},{"error","E_SESSION_STOPPED"}};}
    Json Submit(Json command){auto request=std::make_shared<Request>();request->command=std::move(command);auto future=request->reply.get_future();{std::lock_guard lock(mutex);if(finished.load())return TerminalReply(request->command);if(queue.size()>=c.max_queue)return {{"status","rejected"},{"error","E_QUEUE_FULL"}};queue.push_back(request);}condition.notify_one();return future.get();}
    Json Snapshot()const{
        const auto& e=*engine;Json input=Json::object(),faults=Json::array(),mutations=Json::array();for(const auto& [target,a]:pendingInputs)input[target]={{"state",a.value("state",a.value("pressed_state",2))},{"command_id",a.at("command_id")}};
        for(const auto& [key,a]:pendingMutations){mutations.push_back(a);faults.push_back({{"fault_id",a.value("fault_id",key)},{"command_id",a.at("command_id")},{"op",a.at("op")=="fault.apply"?"apply":a.at("op")=="fault.revoke"?"revoke":a.at("op").get<std::string>()},{"edge_id",a.value("edge_id",std::string())}});}
        auto d=journal->CurrentDigests();Json board=e.runtime.board.empty()?e.Resolve(e.runtime):e.runtime.board;
        return {{"phase1",true},{"state",state},{"reason",reason},{"error",error},{"run_id",c.run_id},{"tick_ps","1"},{"position",Pos(e.committed)},{"attempted_position",Pos(e.attempted)},{"topology_position",Pos(e.committed)},
            {"guard",e.guard},{"board",board},{"outputs",e.runtime.outputs},{"faults",e.runtime.faults.Snapshot(e.runtime.tick)},{"p1",e.Checkpoint(e.runtime)},
            {"input_writes",std::to_string(e.writes)},{"eval_calls",std::to_string(e.evals)},{"final_calls",e.finals},{"pending_count",pendingInputs.size()+pendingMutations.size()},{"pending_inputs",input},{"pending_faults",faults},{"pending_mutations",mutations},
            {"binding_status","bound"},{"trace",{{"revision",std::to_string(e.commits)},{"run_id",c.run_id},{"generation",std::to_string(c.generation)},{"committed_through_tick",std::to_string(e.committed.tick)},{"tail_event_seq",std::to_string(e.committed.event_seq)}}},
            {"digests",{{"circuit",d.circuit},{"control",d.control},{"algorithm","sigflow.sha256-chain.p1.v1"}}}};
    }
    Json Audit()const{const auto& d=journal->CurrentDigests();return {{"status","observed"},{"records",journal->Records()},{"digests",{{"circuit",d.circuit},{"control",d.control}}}};}
    Json TracePage(const Json& j)const{
        auto committedCount=trace->Watermark().trace_revision;uint64_t after=P1Tick(j.value("after_revision",Json("0"))),through=P1Tick(j.value("through_revision",Json(std::to_string(committedCount))));size_t limit=j.value("limit",size_t(64));if(!limit || limit>128 || after>through || through>committedCount)throw std::runtime_error("E_TRACE_QUERY");Json batches=Json::array();auto next=after;
        for(const auto& record:journal->Records())if(record.at("kind")=="commit"){auto revision=P1Tick(record.at("trace_revision"));if(revision<=after || revision>through)continue;batches.push_back({{"revision",record.at("trace_revision")},{"position",record.at("position")},{"events",record.at("trace_events")}});next=revision;if(batches.size()==limit)break;}
        return {{"status","observed"},{"run_id",c.run_id},{"generation",std::to_string(c.generation)},{"signals",Header(c).at("trace_signals")},{"batches",batches},{"next_revision",std::to_string(next)},{"complete",next==through}};
    }
    bool Commit(const Json& spec){
        if(engine->commits>=c.max_commits){state="Paused";reason="ResourceLimit";error="E_COMMIT_QUOTA";return false;}
        bool ok=engine->Transact(spec,[&](const Json& tx,const std::vector<LiveTransition>& events){
            auto p=tx.at("evaluation_position");journal->BeginTransaction(tx.at("batch_id"),{P1Tick(p.at("tick")),p.at("delta"),P1Tick(p.at("event_seq"))},tx.at("effective_inputs"));journal->Prepare(tx);trace->AppendBatch(events);auto fence=tx.at("position");
            trace->Commit(EventPosition{P1Tick(fence.at("tick")),fence.at("delta"),P1Tick(fence.at("event_seq"))},[&](const TraceCommit& committed){journal->MarkCommitted(committed);});});
        if(!ok){state="Paused";reason="UnknownInput";error="E_UNKNOWN_INPUT";}else error.clear();return ok;
    }
    void Decision(const Json& a,const std::string& decision){journal->AppendRecord({{"kind","decision"},{"command_id",a.at("command_id")},{"status",decision},{"position",Pos(engine->committed)}});}
    bool ApplyPending(){
        if(!pendingMutations.empty()){Json actions=Json::array();for(const auto& [key,a]:pendingMutations)actions.push_back(a);if(!Commit(engine->Event(engine->runtime.tick,false,"mutation",actions)))return false;for(const auto& [key,a]:pendingMutations)Decision(a,"applied");pendingMutations.clear();}
        if(!pendingInputs.empty()){Json actions=Json::array();for(const auto& [key,a]:pendingInputs)actions.push_back(a);if(!Commit(engine->Event(engine->runtime.tick,true,"input",actions)))return false;for(const auto& [key,a]:pendingInputs)Decision(a,"applied");pendingInputs.clear();}
        // Check every Pad at the current retained boundary, before scheduling time.
        return Commit(engine->Event(engine->runtime.tick,true,"settle"));
    }
    bool NoClock(){state="Paused";reason="NoClockEdge";error="E_NO_CLOCK_EDGE";return false;}
    bool Step(const Json& command){
        std::string mode=command.value("mode","rising");auto start=engine->runtime.tick;std::optional<uint64_t> target;
        if(mode=="ticks"){auto count=P1Tick(command.at("ticks"));if(!count)throw std::runtime_error("E_STEP_TICKS");target=P1Add(start,count);}else if(mode!="next_event" && mode!="rising")throw std::runtime_error("E_STEP_MODE");
        if(!ApplyPending())return false;
        for(size_t count=0;count<c.max_step_events;++count){if(mode=="rising" && engine->runtime.stopped)return NoClock();auto next=engine->NextTick();
            if(target && (!next || *target<*next)){if(*target-start>c.max_step_ticks){state="Paused";reason="ResourceLimit";error="E_STEP_TICK_BUDGET";return false;}if(!Commit(engine->Event(*target,true,"target")))return false;return true;}if(!next)return NoClock();if(*next-start>c.max_step_ticks){state="Paused";reason="ResourceLimit";error="E_STEP_TICK_BUDGET";return false;}
            bool clockEvent=engine->runtime.clock_due==next;if(!Commit(engine->Event(*next,true,"event")))return false;
            if(mode=="next_event" || (target && engine->runtime.tick==*target) || (mode=="rising" && clockEvent && !engine->runtime.stopped && engine->runtime.clock_high))return true;
        }state="Paused";reason="ResourceLimit";error="E_STEP_EVENT_BUDGET";return false;
    }
    Json Handle(const Json& command){
        std::string op=command.at("op");if(op=="get"){auto frame=Snapshot();frame["status"]="observed";return frame;}if(op=="journal")return Audit();if(op=="trace.commits")return TracePage(command);
        std::string id=command.at("command_id");if(id.empty())throw std::runtime_error("E_COMMAND_ID");auto old=requests.find(id);if(old!=requests.end()){if(old->second!=command)throw std::runtime_error("E_COMMAND_ID_REUSED");return acks.at(id);}if(requests.size()>=c.max_commands && op!="stop")throw std::runtime_error("E_COMMAND_QUOTA");
        bool mutation=op=="profile.load" || op=="fault.enable" || op=="fault.revoke" || op=="fault.apply" || op=="uart.send" || op=="dip.set",input=op=="set" || op=="pulse";
        if(mutation || input)engine->ValidateAction(command,engine->runtime);else if(op!="step" && op!="play" && op!="pause" && op!="stop")throw std::runtime_error("E_COMMAND_UNKNOWN");if(op=="step" && state=="Running")throw std::runtime_error("E_PAUSE_REQUIRED");
        if(op=="step"){auto mode=command.value("mode",std::string("rising"));if(mode=="ticks"){auto ticks=P1Tick(command.at("ticks"));if(!ticks)throw std::runtime_error("E_STEP_TICKS");P1Add(engine->runtime.tick,ticks);}else if(mode!="rising" && mode!="next_event")throw std::runtime_error("E_STEP_MODE");}
        requests[id]=command;journal->AppendRecord({{"kind","intent"},{"command_id",id},{"request",command}});std::string status="applied";
        if(mutation || input){auto key=input?command.at("target").get<std::string>():op=="profile.load"?std::string("profile"):op=="uart.send"?"uart:"+id:command.value("fault_id",command.value("target",id));auto& pending=input?pendingInputs:pendingMutations;auto previous=pending.find(key);if(previous!=pending.end())Decision(previous->second,"superseded");pending[key]=command;status="pending";if(state=="Running")status=ApplyPending()?"applied":"blocked";}
        else if(op=="step"){if(Step(command)){state="Paused";reason="StepComplete";}else status="blocked";}
        else if(op=="play"){if(!ApplyPending())status="blocked";else{state="Running";reason.clear();}}
        else if(op=="pause"){state="Paused";if(reason!="UnknownInput" && reason!="ResourceLimit")reason="User";}
        else if(op=="stop"){for(const auto& [key,a]:pendingInputs)Decision(a,"cancelled");for(const auto& [key,a]:pendingMutations)Decision(a,"cancelled");pendingInputs.clear();pendingMutations.clear();engine->Finalize();state="Stopped";reason="User";trace->Seal();}
        journal->AppendRecord({{"kind","control"},{"command_id",id},{"op",op},{"status",status},{"position",Pos(engine->committed)}});if(op=="stop")journal->Close();auto frame=Snapshot();frame["status"]=status;frame["command_id"]=id;acks[id]=frame;return frame;
    }
    void Run(){
        try{trace->BindWriterToCurrentThread();engine=std::make_unique<Engine>(c,factory);Commit(engine->Event(0,true,"initial"));ready.set_value();
            while(state!="Stopped" && state!="Faulted"){
                std::shared_ptr<Request> request;{std::unique_lock lock(mutex);if(queue.empty() && state!="Running")condition.wait(lock,[&]{return !queue.empty();});if(!queue.empty()){request=queue.front();queue.pop_front();}}
                if(!request){auto next=engine->NextTick();if(!next)NoClock();else if(!Commit(engine->Event(*next,true,"event"))){}continue;}
                try{request->reply.set_value(Handle(request->command));}catch(const std::exception& e){error=e.what();if(error.starts_with("E_NON_CONVERGENT") || error.starts_with("E_JOURNAL")){state="Faulted";reason="RuntimeFailure";}auto rejected=Snapshot();rejected["status"]="rejected";auto id=request->command.value("command_id",std::string());if(requests.count(id) && requests.at(id)==request->command)acks[id]=rejected;request->reply.set_value(rejected);}
            }
        }catch(...){try{ready.set_exception(std::current_exception());}catch(...){}state="Faulted";reason="RuntimeFailure";try{throw;}catch(const std::exception& e){error=e.what();}}
        if(engine){try{engine->Finalize();trace->Seal();journal->Close();}catch(...){}terminal=Snapshot();terminal["status"]="observed";}engine.reset();std::lock_guard lock(mutex);finished=true;
        for(auto& request:queue)try{request->reply.set_value(TerminalReply(request->command));}catch(const std::exception& e){request->reply.set_value({{"status","rejected"},{"error",e.what()}});}queue.clear();
    }
};
Phase1Session::Phase1Session(SessionContract c,VirtualSession::Factory f):impl_(std::make_unique<Impl>(std::move(c),std::move(f))){}
Phase1Session::~Phase1Session()=default;
Json Phase1Session::Submit(Json j){return impl_->Submit(std::move(j));}
std::shared_ptr<const LiveTraceStore> Phase1Session::Trace()const{return impl_->trace;}
Json Phase1Session::Replay(const SessionContract& c,VirtualSession::Factory factory,const Json& records){
    Validate(c);if(!records.is_array() || records.empty() || records.at(0)!=Header(c))throw std::runtime_error("E_REPLAY_FINGERPRINT");std::promise<Json> reply;auto future=reply.get_future();
    std::thread worker([&]{try{Engine engine(c,factory);for(const auto& record:records)if(record.at("kind")=="commit"){
            bool committed=engine.Transact(record.at("p1_event"),[&](const Json& actual,const std::vector<LiveTransition>&){if(actual!=record)throw std::runtime_error("E_P1_REPLAY_DIVERGENCE");});if(!committed)throw std::runtime_error("E_P1_REPLAY_UNKNOWN_INPUT");}
            engine.Finalize();auto digests=DurableJournal::Digests(records);reply.set_value({{"status","passed"},{"matched_commits",engine.commits},{"circuit_digest",digests.circuit},{"control_digest",digests.control},{"board_trace_verified",true},{"fault_rng_peripherals_verified",true}});
        }catch(...){reply.set_exception(std::current_exception());}});try{auto result=future.get();worker.join();return result;}catch(...){worker.join();throw;}
}
}
