#include "Phase1Faults.h"
#include "Phase2Faults.h"
#include "Phase3Faults.h"
#include "Phase4Faults.h"
#include "eda-platform/Sha256.h"
#include <limits>
#include <algorithm>
namespace eda::virtual_board {
namespace {
std::string Resource(const Json& spec){auto type=spec.at("effect").at("type").get<std::string>(),target=spec.at("target").at("id").get<std::string>();if(type=="bridge"){auto other=spec.at("effect").at("other_node").get<std::string>();return "bridge:"+std::min(target,other)+":"+std::max(target,other);}if(type=="common_mismatch" || type=="dead_segment" || type=="led_polarity_reverse" || type=="uart_baud_offset_ppm" || type=="uart_framing_error" || type=="uart_parity_flip")return spec.at("target").dump()+":"+type+":"+spec.at("effect").value("segment",spec.at("effect").value("bit",std::string()));return spec.at("target").dump();}
void Keys(const Json& object,std::initializer_list<const char*> names){
    if(!object.is_object())throw std::runtime_error("E_FAULT_SCHEMA_OBJECT");
    for(const auto& [key,value]:object.items()){bool known=false;for(auto name:names)known|=key==name;if(!known)throw std::runtime_error("E_FAULT_SCHEMA_FIELD: "+key);}
}
std::string PeripheralSource(const BoardTopology& board,const std::string& endpoint){
    if(endpoint==board.ClockEndpoint())return "oscillator";
    for(const auto& c:board.Descriptor().at("components")){auto id=c.at("id").get<std::string>(),model=c.at("model").get<std::string>();
        if(model=="sigflow.button@1" && endpoint==id+".contact")return "contact";
        if(model=="sigflow.uart-peer@1" && endpoint==id+".TX")return "uart.tx";
        if(model=="sigflow.dip-switch@1")for(unsigned bit=0;bit<c.at("params").at("width").get<unsigned>();++bit)if(endpoint==id+".bit"+std::to_string(bit))return "dip";
    }return {};
}
}
uint64_t P1Tick(const Json& j){if(!j.is_string())throw std::runtime_error("E_P1_TICK_STRING");auto s=j.get<std::string>();if(s.empty() || s.size()>20 || s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_P1_TICK_STRING");size_t n=0;auto v=std::stoull(s,&n);if(n!=s.size())throw std::runtime_error("E_P1_TICK_STRING");return v;}
uint64_t P1Add(uint64_t a,uint64_t b){if(b>std::numeric_limits<uint64_t>::max()-a)throw std::runtime_error("E_TIME_OVERFLOW");return a+b;}
Pcg32::Pcg32(uint64_t seed,const std::string& stream){
    auto material=std::string("sigflow.pcg32-v1\n")+std::to_string(seed)+"\n"+stream;auto h=platform::Sha256Hex(material.data(),material.size());
    auto le=[&](size_t at){uint64_t value=0;for(size_t i=0;i<8;++i)value|=std::stoull(h.substr(at+2*i,2),nullptr,16)<<(i*8);return value;};
    increment_=(le(16)<<1)|1;Next();state_+=le(0);Next();
}
uint32_t Pcg32::Next(){uint64_t old=state_;state_=old*6364136223846793005ULL+increment_;uint32_t x=uint32_t(((old>>18)^old)>>27),rotation=uint32_t(old>>59);return (x>>rotation)|(x<<((-rotation)&31));}
uint32_t Pcg32::Bounded(uint32_t bound){if(!bound)throw std::runtime_error("E_RNG_BOUND");uint32_t threshold=uint32_t(-bound)%bound;for(;;){auto value=Next();if(value>=threshold)return value%bound;}}
Json Pcg32::Snapshot()const{return {{"algorithm","pcg32-v1"},{"state",std::to_string(state_)},{"increment",std::to_string(increment_)}};}
Phase1Faults::Phase1Faults(const BoardTopology& board):board_(&board){profile_=EmptyProfile(board);}
Json Phase1Faults::EmptyProfile(const BoardTopology& board){return {{"schema",board.UsesState13()?"sigflow.fault-profile.v2":"sigflow.fault-profile.v1"},{"id","empty"},{"revision",1},{"board_ref",board.Descriptor().at("id").get<std::string>()+"@"+board.Descriptor().at("version").get<std::string>()},{"tick_ps","1"},{"rng",{{"algorithm","pcg32-v1"},{"seed","0"},{"streams","by_fault_id"}}},{"conflict_policy","reject_overlapping_mutations"},{"faults",Json::array()}};}
bool Phase1Faults::Active(const Rule& r,uint64_t tick)const{return r.spec.at("enabled")==true && r.start && tick>=*r.start && (!r.end || tick<*r.end);}
void Phase1Faults::ValidateConflicts(const std::map<std::string,Rule>& rules,uint64_t tick)const{
    for(auto a=rules.begin();a!=rules.end();++a)for(auto b=std::next(a);b!=rules.end();++b)if(a->second.spec.at("enabled")==true && b->second.spec.at("enabled")==true && Resource(a->second.spec)==Resource(b->second.spec) && a->second.start && b->second.start){auto begin=std::max(*a->second.start,*b->second.start);if((!a->second.end || begin<*a->second.end) && (!b->second.end || begin<*b->second.end))throw std::runtime_error("E_FAULT_CONFLICT: "+a->first+" / "+b->first);}
    std::map<std::string,std::string> resources;
    for(const auto& [id,r]:rules){if(!Active(r,tick))continue;auto key=Resource(r.spec);
        auto [it,inserted]=resources.emplace(key,id);if(!inserted)throw std::runtime_error("E_FAULT_CONFLICT: "+it->second+" / "+id+" -> "+key);}
}
void Phase1Faults::Load(const Json& p,uint64_t tick){
    Keys(p,{"schema","id","revision","board_ref","tick_ps","rng","conflict_policy","faults"});Keys(p.at("rng"),{"algorithm","seed","streams"});
    if(p.at("schema")!=(board_->UsesState13()?"sigflow.fault-profile.v2":"sigflow.fault-profile.v1") || !p.at("id").is_string() || p.at("id").get<std::string>().empty() || !p.at("revision").is_number_unsigned() && !p.at("revision").is_number_integer() || p.at("revision").get<int64_t>()<1 || p.at("tick_ps")!="1" || p.at("rng").at("algorithm")!="pcg32-v1" || p.at("rng").at("streams")!="by_fault_id" || p.at("conflict_policy")!="reject_overlapping_mutations")throw std::runtime_error("E_FAULT_PROFILE_VERSION");
    const auto& descriptor=board_->Descriptor();auto exact=descriptor.at("id").get<std::string>()+"@"+descriptor.at("version").get<std::string>();
    if(p.at("board_ref")!=exact)throw std::runtime_error("E_FAULT_BOARD_REF: expected "+exact);
    if(!p.at("faults").is_array() || p.at("faults").size()>128)throw std::runtime_error("E_FAULT_QUOTA");uint64_t seed=P1Tick(p.at("rng").at("seed"));std::map<std::string,Rule> next;
    for(const auto& f:p.at("faults")){
        Keys(f,{"id","enabled","visibility","hook","target","effect","trigger"});Keys(f.at("target"),{"kind","id"});
        if(f.at("visibility")!="student_visible" && f.at("visibility")!="teacher_only")throw std::runtime_error("E_FAULT_VISIBILITY");
        std::string id=f.at("id"),kind=f.at("effect").at("type"),target=f.at("target").at("id"),targetKind=f.at("target").at("kind"),hook=f.at("hook"),trigger=f.at("trigger").at("kind");
        if(id.empty() || id.size()>128 || !f.at("enabled").is_boolean() || !next.emplace(id,Rule{}).second)throw std::runtime_error("E_FAULT_ID");auto& r=next.at(id);r.spec=f;r.rng=Pcg32(seed,id);
        bool component=false;std::string model;for(const auto& c:descriptor.at("components"))if(c.at("id")==target){component=true;model=c.at("model");}
        if(kind=="open"){Keys(f.at("effect"),{"type"});if(hook!="topology.transform" || targetKind!="connection" || !board_->HasEdge(target))throw std::runtime_error("E_FAULT_TARGET");}
        else if(kind=="stuck_at"){
            Keys(f.at("effect"),{"type","mode","value"});
            auto mode=f.at("effect").at("mode").get<std::string>();if(hook!="driver.transform" || (mode!="replace_source" && mode!="short_to_rail") || !f.at("effect").at("value").is_number_integer() || f.at("effect").at("value").get<int>()<0 || f.at("effect").at("value").get<int>()>1)throw std::runtime_error("E_STUCK_PARAMETERS");
            if(targetKind=="fpga_pad_driver"){if(!target.starts_with("fpga0.pad.") || !board_->Nodes().count(target.substr(6)))throw std::runtime_error("E_FAULT_TARGET");}
            else if(targetKind=="peripheral_driver"){if(!board_->Nodes().count(target) || PeripheralSource(*board_,target).empty())throw std::runtime_error("E_FAULT_TARGET");}
            else throw std::runtime_error("E_FAULT_TARGET");
        }else if(kind=="clock_stop"){
            Keys(f.at("effect"),{"type","hold_level","resume_phase"});
            if(hook!="clock.schedule" || targetKind!="clock" || target!=descriptor.at("clocks").at(0).at("id").get<std::string>() || f.at("effect").at("resume_phase")!="restart_half_period" || !f.at("effect").at("hold_level").is_number_integer() || f.at("effect").at("hold_level").get<int>()<0 || f.at("effect").at("hold_level").get<int>()>1 || trigger=="intermittent")throw std::runtime_error("E_CLOCK_STOP_PARAMETERS");
        }else if(kind=="button_bounce"){
            Keys(f.at("effect"),{"type","drive","duration_ticks","relative_edges"});
            if(hook!="peripheral.event" || targetKind!="component" || !component || model!="sigflow.button@1" || trigger!="intermittent" || f.at("effect").at("drive")!="open_drain")throw std::runtime_error("E_BOUNCE_TARGET");
            auto duration=P1Tick(f.at("effect").at("duration_ticks"));const auto& es=f.at("effect").at("relative_edges");if(!duration || !es.is_array() || es.empty() || es.size()>4096)throw std::runtime_error("E_BOUNCE_EDGES");uint64_t last=0;bool first=true;
            for(const auto& edge:es){Keys(edge,{"dt","contact"});auto dt=P1Tick(edge.at("dt"));std::string contact=edge.at("contact");if((first && dt!=0) || (!first && dt<=last) || dt>duration || (contact!="closed" && contact!="released"))throw std::runtime_error("E_BOUNCE_EDGES");first=false;last=dt;}
            if(last!=duration || es.back().at("contact")!="closed")throw std::runtime_error("E_BOUNCE_TERMINAL_EDGE");
        }else if(!ValidatePhase2Fault(*board_,f) && !ValidatePhase3Fault(*board_,f) && !ValidatePhase4Fault(*board_,f))throw std::runtime_error("E_FAULT_UNSUPPORTED");
        const auto& tr=f.at("trigger");if(trigger=="permanent" || trigger=="transient"){
            if(trigger=="permanent")Keys(tr,{"kind","start_tick"});else Keys(tr,{"kind","start_tick","end_tick"});
            r.start=P1Tick(tr.at("start_tick"));if(trigger=="transient"){r.end=P1Tick(tr.at("end_tick"));if(*r.end<=*r.start)throw std::runtime_error("E_FAULT_WINDOW");}
        }else if(trigger=="intermittent"){
            Keys(tr,{"kind","event","probability_ppm","active_ticks","cooldown_ticks","max_activations"});Keys(tr.at("event"),{"type","target","action","occurrence"});
            if(kind!="button_bounce" || !tr.at("probability_ppm").is_number_integer() || !tr.at("max_activations").is_number_integer())throw std::runtime_error("E_INTERMITTENT_PARAMETERS");
            const auto& e=tr.at("event");if(e.at("type")!="input.intent" || e.at("action")!="press" || e.at("target")!=target || !e.at("occurrence").is_number_integer() || e.at("occurrence").get<int64_t>()<1 || tr.at("probability_ppm").get<int64_t>()<0 || tr.at("probability_ppm").get<int64_t>()>1000000 || tr.at("max_activations").get<int64_t>()<1 || !P1Tick(tr.at("active_ticks")))throw std::runtime_error("E_INTERMITTENT_PARAMETERS");
            P1Tick(tr.at("cooldown_ticks"));if(kind=="button_bounce" && P1Tick(tr.at("active_ticks"))<=P1Tick(f.at("effect").at("duration_ticks")))throw std::runtime_error("E_BOUNCE_ACTIVE_WINDOW");
        }else throw std::runtime_error("E_FAULT_TRIGGER");
    }
    // Reject conflicting enabled windows statically as well as dynamically.
    for(auto a=next.begin();a!=next.end();++a)for(auto b=std::next(a);b!=next.end();++b)if(a->second.spec.at("enabled")==true && b->second.spec.at("enabled")==true && Resource(a->second.spec)==Resource(b->second.spec) && a->second.start && b->second.start){
        auto begin=std::max(*a->second.start,*b->second.start);if((!a->second.end || begin<*a->second.end) && (!b->second.end || begin<*b->second.end))throw std::runtime_error("E_FAULT_CONFLICT: "+a->first+" / "+b->first);}
    // A profile edit must not restart unrelated live bounce schedules or streams.
    // Changing the global seed explicitly starts a new set of streams.
    if(profile_.at("rng")==p.at("rng"))for(auto& [id,r]:next){auto old=rules_.find(id);if(old==rules_.end())continue;if(old->second.spec==r.spec)r=old->second;else r.rng=old->second.rng;}
    ValidateConflicts(next,tick);profile_=p;rules_=std::move(next);events_.push_back({{"type","profile.loaded"},{"id",p.at("id")},{"revision",p.at("revision")},{"tick",std::to_string(tick)}});
}
void Phase1Faults::Enable(const std::string& id,bool enabled,uint64_t tick){auto next=rules_;auto it=next.find(id);if(it==next.end())throw std::runtime_error("E_FAULT_UNKNOWN");auto& r=it->second;r.spec["enabled"]=enabled;if(!enabled){r.edges.clear();r.contact.reset();}ValidateConflicts(next,tick);rules_=std::move(next);events_.push_back({{"type",enabled?"fault.enabled":"fault.revoked"},{"fault_id",id},{"tick",std::to_string(tick)}});}
void Phase1Faults::Upsert(const Json& rule,uint64_t tick){
    Json profile=profile_;profile["faults"]=Json::array();auto id=rule.at("id").get<std::string>();for(const auto& [name,r]:rules_)if(name!=id)profile["faults"].push_back(r.spec);profile["faults"].push_back(rule);
    Phase1Faults candidate(*board_);candidate.Load(profile,tick);for(const auto& [name,r]:rules_)if(name!=id)candidate.rules_[name]=r;candidate.intent_=intent_;candidate.events_=events_;candidate.events_.push_back({{"type","fault.upserted"},{"fault_id",id},{"tick",std::to_string(tick)}});*this=std::move(candidate);
}
void Phase1Faults::InputIntent(const std::string& target,uint8_t state,uint64_t tick,bool userIntent){
    auto old=intent_.find(target);bool press=userIntent && state==uint8_t(State::SLO) && (old==intent_.end() || old->second!=state);intent_[target]=state;
    for(auto& [id,r]:rules_){if(r.spec.at("effect").at("type")!="button_bounce" || r.spec.at("target").at("id")!=target)continue;
        if(state==uint8_t(State::FLT)){if(!r.edges.empty() || r.contact){events_.push_back({{"type","bounce.cancelled_by_release"},{"fault_id",id},{"tick",std::to_string(tick)}});}r.edges.clear();r.contact.reset();r.end=tick;continue;}
        if(!press || r.spec.at("enabled")!=true)continue;const auto& tr=r.spec.at("trigger");++r.matches;auto wanted=tr.at("event").at("occurrence").get<uint64_t>();if(r.matches<wanted || tick<r.cooldown_until || r.activations>=tr.at("max_activations").get<uint64_t>())continue;
        auto draw=r.rng.Bounded(1000000);bool fired=draw<tr.at("probability_ppm").get<uint32_t>();events_.push_back({{"type","fault.random_decision"},{"fault_id",id},{"tick",std::to_string(tick)},{"draw_ppm",draw},{"fired",fired},{"rng",r.rng.Snapshot()}});if(!fired)continue;
        r.start=tick;r.end=P1Add(tick,P1Tick(tr.at("active_ticks")));r.cooldown_until=P1Add(tick,P1Tick(tr.at("cooldown_ticks")));++r.activations;r.edges.clear();
        for(const auto& e:r.spec.at("effect").at("relative_edges"))r.edges[P1Add(tick,P1Tick(e.at("dt")))]=e.at("contact")=="closed"?uint8_t(State::SLO):uint8_t(State::FLT);
        ValidateConflicts(rules_,tick);
    }
}
void Phase1Faults::Advance(uint64_t tick){
    ValidateConflicts(rules_,tick);for(auto& [id,r]:rules_){bool active=Active(r,tick);if(active!=r.observed_active){events_.push_back({{"type",active?"fault.activated":"fault.deactivated"},{"fault_id",id},{"tick",std::to_string(tick)}});r.observed_active=active;
        // Phase 3 reset_glitch: activation anchors the exact-width pulse on the
        // external contact; both edges are deterministic scheduler events.
        if(active && r.spec.at("effect").at("type")=="reset_glitch"){
            auto duration=P1Tick(r.spec.at("effect").at("duration_ticks"));
            auto pressed=r.spec.at("effect").at("pressed_state").get<int>()==1?uint8_t(State::SHI):uint8_t(State::SLO);
            r.edges[*r.start]=pressed;r.edges[P1Add(*r.start,duration)]=uint8_t(State::FLT);
            r.end=P1Add(*r.start,duration);
        }}
        while(!r.edges.empty() && r.edges.begin()->first<=tick){auto edge=*r.edges.begin();r.edges.erase(r.edges.begin());r.contact=edge.second;events_.push_back({{"type","pin.edge"},{"origin","fault.expansion"},{"fault_id",id},{"tick",std::to_string(edge.first)},{"state",edge.second}});}
        if(r.end && tick>=*r.end){r.edges.clear();r.contact.reset();}}
}
std::optional<uint64_t> Phase1Faults::NextDeadline(uint64_t tick)const{std::optional<uint64_t> next;auto add=[&](uint64_t value){if(value>tick && (!next || value<*next))next=value;};for(const auto& [id,r]:rules_)if(r.spec.at("enabled")==true){if(r.start)add(*r.start);if(r.end)add(*r.end);if(!r.edges.empty())add(r.edges.begin()->first);}return next;}
std::optional<bool> Phase1Faults::ClockHold(uint64_t tick)const{for(const auto& [id,r]:rules_)if(Active(r,tick) && r.spec.at("effect").at("type")=="clock_stop")return r.spec.at("effect").at("hold_level").get<int>()==1;return {};}
std::optional<uint64_t> Phase1Faults::ClockDenOffset(uint64_t tick)const{for(const auto& [id,r]:rules_)if(Active(r,tick) && r.spec.at("effect").at("type")=="clock_divider_offset")return P1Tick(r.spec.at("effect").at("den_offset"));return {};}
Json Phase1Faults::CdcRule(const std::string& target,uint64_t tick)const{
    for(const auto& [id,r]:rules_)if(Active(r,tick) && r.spec.at("effect").at("type")=="cdc_sampling_approx" && r.spec.at("target").at("id")==target)return r.spec;
    return Json();}
std::optional<uint32_t> Phase1Faults::CdcDraw(const std::string& target,uint64_t tick){
    for(auto& [id,r]:rules_){
        if(!Active(r,tick) || r.spec.at("effect").at("type")!="cdc_sampling_approx" || r.spec.at("target").at("id").get<std::string>()!=target)continue;
        auto maximum=r.spec.at("effect").at("max_delay_cycles").get<uint32_t>();
        auto chosen=r.rng.Bounded(maximum+1);
        events_.push_back({{"type","cdc.sampling_approx"},{"approximation",CdcApproximation},{"fault_id",id},
            {"tick",std::to_string(tick)},{"policy","seeded_enum"},{"chosen_delay_cycles",chosen},
            {"window_ps",r.spec.at("effect").at("window_ps")},{"max_delay_cycles",maximum},{"rng",r.rng.Snapshot()}});
        return chosen;
    }
    return {};
}
void Phase1Faults::Transform(std::map<std::string,std::vector<Driver>>& drivers,FaultSet& edges,uint64_t tick)const{
    for(const auto& [id,r]:rules_){if(!Active(r,tick))continue;auto kind=r.spec.at("effect").at("type").get<std::string>(),target=r.spec.at("target").at("id").get<std::string>();
        if(kind=="open")edges[id]={id,target,0,{}};
        else if((kind=="button_bounce" || kind=="reset_glitch") && r.contact){auto& ds=drivers[target+".contact"];for(auto& d:ds)if(d.driver_id=="contact")d.state=*r.contact;}
        else if(kind=="stuck_at"){
            bool fpga=r.spec.at("target").at("kind")=="fpga_pad_driver";auto node=fpga?target.substr(6):target;auto value=r.spec.at("effect").at("value")==1?State::SHI:State::SLO;
            if(r.spec.at("effect").at("mode")=="short_to_rail"){
                // P2 explicitly models a Power rail; P1 retains its frozen Strong rail.
                uint8_t rail=board_->UsesState13()?uint8_t(value==State::SHI?31:29):uint8_t(value);
                drivers[node].push_back({"fault:"+id,rail});
            }
            else {bool found=false;auto source=fpga?std::string():PeripheralSource(*board_,target);for(auto& d:drivers[node])if(fpga?d.driver_id.starts_with("dut:"):d.driver_id==source){d.state=uint8_t(value);found=true;}
                // A missing original source remains floating; replacement is not a new driver.
                if(!found && !fpga)throw std::runtime_error("E_STUCK_DRIVER_MISSING: "+target);}
        }
    }
}
Json Phase1Faults::ActiveRules(uint64_t tick)const{Json rules=Json::array();for(const auto& [id,r]:rules_)if(Active(r,tick))rules.push_back(r.spec);return rules;}
Json Phase1Faults::Snapshot(uint64_t tick)const{Json rules=Json::array();for(const auto& [id,r]:rules_)rules.push_back({{"id",id},{"type",r.spec.at("effect").at("type")},{"target",r.spec.at("target")},{"effect",r.spec.at("effect")},{"trigger",r.spec.at("trigger")},{"enabled",r.spec.at("enabled")},{"active",Active(r,tick)},{"matches",std::to_string(r.matches)},{"activations",std::to_string(r.activations)},{"remaining_edges",r.edges.size()},{"cooldown_until",std::to_string(r.cooldown_until)},{"rng",r.rng.Snapshot()}});return {{"profile_id",profile_.at("id")},{"revision",profile_.at("revision")},{"rules",rules},{"events",events_}};}
}
