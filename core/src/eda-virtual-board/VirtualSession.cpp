#include "VirtualSession.h"
#include "ElectricalValidator.h"
#include "PortManifestBuilder.h"
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
Json Header(const SessionContract& c){
    Json bs=Json::array();
    for(const auto& b:c.bindings)bs.push_back({{"port_ref",c.manifest.Label(b.ref)},{"package_pin",b.package_pin},
        {"io_type",b.io_type},{"drive",b.drive},{"pull_up",b.pull_up},{"pull_down",b.pull_down}});
    auto text=bs.dump();
    return {{"kind","run_header"},{"schema","sigflow.session-journal.m0.v1"},
        {"source_sha256",c.manifest.source_sha256},{"adapter_sha256",c.adapter_sha256},
        {"electrical_sha256",c.electrical_sha256},{"bindings_sha256",platform::Sha256Hex(text.data(),text.size())},
        {"resolver_version",ResolverVersion},{"tick_ps","1"},{"frequency_hz",std::to_string(c.frequency_hz)}};
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
    EventPosition committed,attempted;
    Clock clock;
    bool clock_high=false;
    uint64_t evaluations=0,writes=0,final_calls=0,edges=0;
    bool initialized=false;
    struct Pending {std::string id;uint8_t value;};
    std::map<std::string,Pending> pending;
    std::map<std::string,Json> acknowledgements,requests;
    std::map<std::string,uint8_t> manual;
    std::map<int,PinBindingV2> pad_map;
    std::map<int,InputEndpoint> input_map;
    std::map<std::string,int> target_map;
    Json journal=Json::array(),last_guard=Json::object();
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
            if(!refs.count(contract.manifest.Label(b)))throw std::runtime_error("E_MISSING_BINDING");
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
        journal.push_back(Header(contract));
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
        auto r=std::make_shared<Request>();r->value=std::move(j);auto future=r->reply.get_future();
        {
            std::lock_guard lock(mutex);
            if(finished.load()){
                if(r->value.value("op","")=="get")return terminal_snapshot;
                if(r->value.value("op","")=="journal")return {{"status","observed"},{"records",journal},{"acks",acknowledgements},{"durability","memory"}};
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
        Json outputs=Json::object();
        for(const auto& p:contract.manifest.ports)if(p.direction==Direction::Output)
            for(const auto& b:p.bits)outputs[contract.manifest.Label(b)]=initialized?Json(model->ReadOutput(b)):Json(nullptr);
        return {{"run_id",contract.run_id},{"state",state},{"reason",reason},{"position",Position(committed)},
            {"attempted_position",Position(attempted)},{"tick_ps","1"},{"eval_calls",std::to_string(evaluations)},
            {"input_writes",std::to_string(writes)},{"edges",std::to_string(edges)},
            {"pending_count",pending.size()},{"model_instances",model?1:0},{"final_calls",final_calls},
            {"guard",last_guard},{"outputs",outputs},{"durability","memory"},{"resolver_version",ResolverVersion}};
    }
    void Decision(const std::string& id,const std::string& status,const std::string& by={}){
        auto& ack=acknowledgements.at(id);ack["status"]=status;if(!by.empty())ack["by"]=by;
        journal.push_back({{"kind","decision"},{"command_id",id},{"status",status},{"by",by}});
    }
    bool Evaluate(uint64_t tick,const std::map<std::string,uint8_t>& candidate,bool high){
        if(commits>=contract.max_commits){state="Paused";reason="ResourceLimit";return false;}
        if(committed.event_seq==std::numeric_limits<uint64_t>::max() ||
           (tick==committed.tick && evaluations && committed.delta==std::numeric_limits<uint32_t>::max()))
            throw std::runtime_error("E_EVENT_POSITION_OVERFLOW");
        attempted={tick,tick==committed.tick && evaluations?uint32_t(committed.delta+1):0,committed.event_seq+1};
        std::vector<std::pair<PortBitRef,bool>> imports;Json effective=Json::array();
        last_guard=Json::object();
        // Preflight ALL inputs before the first DUT assignment, including async
        // reset, initialization and combinational changes between clock edges.
        for(const auto& [pad,e]:input_map){
            auto drivers=e.fixed_drivers;
            if(pad==contract.clock_pad)drivers.push_back({"clock",uint8_t(high?State::SHI:State::SLO)});
            else {
                auto it=candidate.find(e.target);
                if(it!=candidate.end())drivers.push_back({"manual",it->second});
            }
            auto r=ResolveM0(std::move(drivers));const auto& ref=pad_map.at(pad).ref;
            if(GuardInput(Direction::Input,r)==ImportDecision::PauseUnknownInput){
                state="Paused";reason="UnknownInput";
                Json ds=Json::array();for(const auto& d:r.drivers)ds.push_back({{"driver_id",d.driver_id},{"state",d.state},{"label",StateName(d.state)}});
                last_guard={{"port_ref",contract.manifest.Label(ref)},{"package_pin",pad},{"state",r.state},
                    {"label",StateName(r.state)},{"drivers",ds},{"error",r.error},{"attempted_position",Position(attempted)},
                    {"last_committed_position",Position(committed)}};
                return false;
            }
            imports.push_back({ref,r.high()});effective.push_back({{"port_ref",contract.manifest.Label(ref)},{"value",r.high()},{"state",r.state}});
        }
        try{
            for(const auto& [ref,value]:imports){model->WriteInput(ref,value);++writes;}
            model->Evaluate(attempted);++evaluations;initialized=true;committed=attempted;++commits;
        }catch(...){state="Faulted";reason="AdapterFailure";throw;}
        Json output=Json::object();
        try{for(const auto& p:contract.manifest.ports)if(p.direction==Direction::Output)
            for(const auto& b:p.bits)output[contract.manifest.Label(b)]=model->ReadOutput(b);}
        catch(...){state="Faulted";reason="AdapterFailure";throw;}
        journal.push_back({{"kind","commit"},{"batch_id",std::to_string(commits)},{"position",Position(committed)},
            {"effective_inputs",effective},{"outputs",output},{"durability","memory"}});
        return true;
    }
    bool ApplyPending(){
        if(pending.empty() && initialized)return true;
        auto candidate=manual;for(const auto& [target,p]:pending)candidate[target]=p.value;
        if(!Evaluate(committed.tick,candidate,clock_high)){
            for(const auto& [target,p]:pending)Decision(p.id,"blocked");return false;
        }
        manual=std::move(candidate);
        auto applied=std::move(pending);pending.clear();
        for(const auto& [target,p]:applied){
            Decision(p.id,"applied");auto& ack=acknowledgements.at(p.id);
            ack=Snapshot();ack["command_id"]=p.id;ack["status"]="applied";ack["position"]=Position(committed);ack["batch_id"]=std::to_string(commits);
            journal.push_back({{"kind","input_commit"},{"command_id",p.id},{"target",target},{"state",p.value},
                {"position",Position(committed)},{"batch_id",std::to_string(commits)},{"status","applied"},{"durability","memory"}});
        }
        return true;
    }
    bool NextRising(){
        do{
            auto next=clock;auto tick=next.Next();bool high=!clock_high;
            if(!Evaluate(tick,manual,high))return false;
            clock=next;clock_high=high;++edges;
        }while(!clock_high);
        return true;
    }
    Json Handle(const Json& j){
        std::string op=j.at("op");
        if(op=="get"){auto s=Snapshot();s["status"]="observed";return s;}
        if(op=="journal")return {{"status","observed"},{"records",journal},{"acks",acknowledgements},{"durability","memory"}};
        std::string id=j.at("command_id");
        if(id.empty())throw std::runtime_error("E_COMMAND_ID");
        auto old=requests.find(id);
        if(old!=requests.end()){
            if(old->second!=j)throw std::runtime_error("E_COMMAND_ID_REUSED");return acknowledgements.at(id);
        }
        if(requests.size()>=contract.max_commands && op!="stop")throw std::runtime_error("E_COMMAND_QUOTA");
        // Validate before inserting an intent or replacing a pending command.
        if(op=="set"){
            std::string target=j.at("target");int value=j.at("state");
            if(!target_map.count(target) || target_map.at(target)==contract.clock_pad)throw std::runtime_error("E_INPUT_OWNERSHIP");
            if(value<0 || value>255 || !ResolveM0({{"manual",uint8_t(value)}}).supported)throw std::runtime_error("E_STATE_UNSUPPORTED");
        }else if(op!="step" && op!="play" && op!="pause" && op!="stop")throw std::runtime_error("E_COMMAND_UNKNOWN");
        if(op=="step" && state=="Running")throw std::runtime_error("E_PAUSE_REQUIRED");
        requests[id]=j;acknowledgements[id]={{"command_id",id},{"status","pending"}};
        journal.push_back({{"kind","intent"},{"command_id",id},{"request",j},{"status","pending"}});
        std::string status="applied";
        if(op=="set"){
            std::string target=j.at("target");auto existing=pending.find(target);
            if(existing!=pending.end())Decision(existing->second.id,"superseded",id);
            pending[target]={id,uint8_t(j.at("state").get<int>())};status="pending";
            if(state=="Running")status=ApplyPending()?"applied":"blocked";
        }else if(op=="step"){
            if(ApplyPending() && NextRising()){state="Paused";reason="StepComplete";}else status="blocked";
        }else if(op=="play"){
            if(ApplyPending()){state="Running";reason.clear();}else status="blocked";
        }else if(op=="pause"){
            state="Paused";if(reason!="UnknownInput" && reason!="ResourceLimit")reason="User";
        }else if(op=="stop"){
            for(const auto& [target,p]:pending)Decision(p.id,"cancelled",id);pending.clear();
            ++final_calls;
            try{model->Finalize();}catch(...){state="Faulted";reason="AdapterFailure";throw;}
            state="Stopped";reason="User";
        }
        auto ack=Snapshot();ack["command_id"]=id;ack["status"]=status;
        acknowledgements[id]=ack;
        journal.push_back({{"kind","control"},{"command_id",id},{"op",op},{"status",status},{"position",Position(committed)}});
        return ack;
    }
    void Run(){
        std::shared_ptr<Request> active;
        try{
            model=factory();if(!model)throw std::runtime_error("E_MODEL_FACTORY");
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
                    active->reply.set_value({{"status","rejected"},{"error",e.what()}});
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
        try{terminal_snapshot=Snapshot();terminal_snapshot["status"]="observed";}
        catch(...){terminal_snapshot={{"state","Faulted"},{"status","observed"}};}
        model.reset();std::lock_guard lock(mutex);finished=true;
        for(auto& r:queue){
            if(r->value.value("op","")=="get")r->reply.set_value(terminal_snapshot);
            else if(r->value.value("op","")=="journal")r->reply.set_value({{"status","observed"},{"records",journal},{"acks",acknowledgements},{"durability","memory"}});
            else {
                auto id=r->value.value("command_id","");auto original=requests.find(id);
                if(original!=requests.end() && original->second==r->value)r->reply.set_value(acknowledgements.at(id));
                else r->reply.set_value({{"status","rejected"},{"error","E_SESSION_STOPPED"}});
            }
        }queue.clear();
    }
};
VirtualSession::VirtualSession(SessionContract c,Factory f):impl_(std::make_unique<Impl>(std::move(c),std::move(f))){}
VirtualSession::~VirtualSession()=default;
Json VirtualSession::Submit(Json j){
    if(j.value("command_id","").rfind("__session_",0)==0)return {{"status","rejected"},{"error","E_COMMAND_ID_RESERVED"}};
    return impl_->Submit(std::move(j));
}
Json VirtualSession::ReplayCommitted(const SessionContract& c,Factory factory,const Json& records){
    std::promise<Json> reply;auto future=reply.get_future();
    std::thread worker([&]{
        std::unique_ptr<IDutAdapter> model;bool finalized=false;
        try{
            auto manifest=PortManifestBuilder::Deserialize(PortManifestBuilder::Serialize(c.manifest));
            if(!manifest.ok() || !ValidateElectrical(c.electrical,c.identity,c.manifest,c.bindings).empty())
                throw std::runtime_error("E_REPLAY_CONTRACT");
            if(!records.is_array() || records.empty() || records.at(0)!=Header(c))throw std::runtime_error("E_REPLAY_FINGERPRINT");
            model=factory();if(!model)throw std::runtime_error("E_MODEL_FACTORY");
            EventPosition previous;size_t count=0;Json outputs;
            for(const auto& record:records){
                if(record.at("kind")!="commit")continue;
                auto p=record.at("position");EventPosition position;
                size_t used=0;auto tick=p.at("tick").get<std::string>();position.tick=std::stoull(tick,&used);
                if(used!=tick.size())throw std::runtime_error("E_REPLAY_POSITION");
                auto seq=p.at("event_seq").get<std::string>();position.event_seq=std::stoull(seq,&used);
                if(used!=seq.size())throw std::runtime_error("E_REPLAY_POSITION");position.delta=p.at("delta");
                if(position.event_seq!=previous.event_seq+1 || (count && (position.tick<previous.tick ||
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
                size_t expected=0;for(const auto& port:c.manifest.ports)if(port.direction==Direction::Input)expected+=port.bits.size();
                if(imports.size()!=expected)throw std::runtime_error("E_REPLAY_INPUT");
                for(const auto& [ref,value]:imports)model->WriteInput(ref,value);
                model->Evaluate(position);outputs=Json::object();
                for(const auto& port:c.manifest.ports)if(port.direction==Direction::Output)
                    for(const auto& b:port.bits)outputs[c.manifest.Label(b)]=model->ReadOutput(b);
                if(outputs!=record.at("outputs"))throw std::runtime_error("E_REPLAY_DIVERGENCE");
                previous=position;++count;
            }
            finalized=true;model->Finalize();reply.set_value({{"status","passed"},{"matched_commits",count},{"outputs",outputs},{"durability","memory"}});
        }catch(...){if(model && !finalized)try{model->Finalize();}catch(...){}reply.set_exception(std::current_exception());}
    });
    try{auto result=future.get();worker.join();return result;}catch(...){worker.join();throw;}
}
}
