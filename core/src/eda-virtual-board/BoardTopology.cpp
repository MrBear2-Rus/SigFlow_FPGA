#include "BoardTopology.h"
#include "eda-platform/Sha256.h"
#include <fstream>
#include <limits>
#include <stdexcept>
namespace eda::virtual_board {
namespace {
uint64_t U64(const Json& j){auto s=j.get<std::string>();if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_TOPOLOGY_TIME");return std::stoull(s);}
int Pad(const std::string& s){if(s.rfind("pad.",0)!=0 || s.size()<5 || s.substr(4).find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_BOARD_PAD");return std::stoi(s.substr(4));}
Json StateJson(const Resolution& r){Json ds=Json::array();for(const auto& d:r.drivers)ds.push_back({{"driver_id",d.driver_id},{"state",d.state}});
    return {{"state_code",r.state},{"logic",r.known()?(r.high()?"1":"0"):(r.state==uint8_t(State::FLT)?"z":"x")},{"valid",r.known()},{"contention",bool(r.state&ContentionFlag)},{"drivers",ds}};}
}
Json FaultsJson(const FaultSet& faults){Json result=Json::array();for(const auto& [id,f]:faults)result.push_back({{"fault_id",id},{"edge_id",f.edge},{"start_tick",std::to_string(f.start)},{"end_tick",f.end?Json(std::to_string(*f.end)):Json(nullptr)}});return result;}
FaultSet ParseFaults(const Json& j){if(!j.is_array() || j.size()>128)throw std::runtime_error("E_FAULT_QUOTA");FaultSet fs;
    for(const auto& r:j){EdgeFault f;f.id=r.at("fault_id");f.edge=r.at("edge_id");f.start=U64(r.at("start_tick"));if(!r.at("end_tick").is_null())f.end=U64(r.at("end_tick"));
        if(f.id.empty() || f.id.size()>128 || f.edge.empty() || (f.end && *f.end<=f.start) || !fs.emplace(f.id,f).second)throw std::runtime_error("E_FAULT_PROFILE");}return fs;}
std::shared_ptr<const BoardTopology> BoardTopology::Load(const std::filesystem::path& path,const ElectricalProfile& profile){
    std::ifstream in(path);if(!in || std::filesystem::file_size(path)>1024*1024)throw std::runtime_error("E_BOARD_FILE");Json j;in>>j;return std::make_shared<const BoardTopology>(std::move(j),profile);}
BoardTopology::BoardTopology(Json j,const ElectricalProfile& p):descriptor_(std::move(j)){
    const auto& d=descriptor_;
    if(d.at("schema")!="sigflow.virtual-board.v1" || d.at("device")!=p.device.device || d.at("package")!=p.device.package || d.at("hardware_revision")!=p.device.board_revision ||
       d.at("electrical").at("resolver_version")!=ResolverVersion || d.at("electrical").at("state_encoding")!=ResolverVersion || d.at("timebase").at("tick_ps")!="1")throw std::runtime_error("E_BOARD_IDENTITY");
    if(d.at("components").size()>128 || d.at("connections").size()>256 || d.at("clocks").size()!=1)throw std::runtime_error("E_BOARD_QUOTA");
    std::map<std::string,std::string> models;std::set<std::string> terminals,ids;
    const auto& clock=d.at("clocks").at(0);auto clockId=clock.at("id").get<std::string>();
    if(clock.at("model")!="sigflow.clock.rational@1" || clock.at("frequency_hz").at("den")!="1" || clock.at("initial_level")!=0)throw std::runtime_error("E_BOARD_CLOCK");
    frequency_=U64(clock.at("frequency_hz").at("num"));if(!frequency_ || frequency_>500000000000ULL)throw std::runtime_error("E_BOARD_CLOCK");
    clock_endpoint_=clockId+".out";terminals.insert(clock_endpoint_);ids.insert(clockId);
    for(const auto& c:d.at("components")){
        auto id=c.at("id").get<std::string>(),model=c.at("model").get<std::string>();
        if(id.empty() || id.size()>64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos || !ids.insert(id).second)throw std::runtime_error("E_BOARD_COMPONENT");
        if(model=="sigflow.led@1"){
            if(c.at("params").at("active_level")!=0)throw std::runtime_error("E_BOARD_POLARITY");leds_[id+".K"]=0;terminals.insert(id+".K");
        }else if(model=="sigflow.button@1"){
            if(c.at("params").at("drive")!="open_drain" || c.at("params").at("pressed_level")!=0)throw std::runtime_error("E_BOARD_BUTTON");terminals.insert(id+".contact");
        }else if(model=="sigflow.seven-segment-mux@1"){
            if(!d.at("extensions").at("allow_model_instances").get<bool>() || !d.at("extensions").at("require_explicit_wiring").get<bool>())throw std::runtime_error("E_MODEL_INSTANCES_DISABLED");
            const auto& params=c.at("params");if(params.at("digits")!=4 || (params.at("common")!="anode" && params.at("common")!="cathode") || params.at("segment_order")!="abcdefg.dp" || (params.at("segment_active_level")!=0 && params.at("segment_active_level")!=1) || (params.at("digit_active_level")!=0 && params.at("digit_active_level")!=1))throw std::runtime_error("E_SEVEN_SEGMENT_PARAMETERS");
            for(auto name:{"a","b","c","d","e","f","g","dp","dig0","dig1","dig2","dig3"})terminals.insert(id+"."+name);
        }else if(model=="sigflow.dip-switch@1"){
            if(!d.at("extensions").at("allow_model_instances").get<bool>() || !d.at("extensions").at("require_explicit_wiring").get<bool>())throw std::runtime_error("E_MODEL_INSTANCES_DISABLED");
            auto width=c.at("params").at("width").get<int>();if(width<1 || width>16)throw std::runtime_error("E_DIP_WIDTH");for(int i=0;i<width;++i)terminals.insert(id+".bit"+std::to_string(i));
        }else if(model=="sigflow.uart-peer@1"){
            if(!d.at("extensions").at("allow_model_instances").get<bool>() || !d.at("extensions").at("require_explicit_wiring").get<bool>())throw std::runtime_error("E_MODEL_INSTANCES_DISABLED");
            const auto& params=c.at("params");int baud=params.at("baud"),bits=params.at("data_bits"),stops=params.at("stop_bits");std::string parity=params.at("parity");
            if(baud<1 || baud>1000000000 || bits<5 || bits>8 || (stops!=1 && stops!=2) || (parity!="none" && parity!="even" && parity!="odd"))throw std::runtime_error("E_UART_PARAMETERS");terminals.insert(id+".TX");terminals.insert(id+".RX");
        }else throw std::runtime_error("E_BOARD_MODEL_UNSUPPORTED");models[id]=model;
    }
    std::set<std::string> edgeIds,connected;
    for(const auto& r:d.at("connections")){
        ConnectionEdge e{r.at("id"),r.at("net"),r.at("endpoint")};int pad=Pad(e.net);auto found=p.device.pads.find(pad);
        if(e.id.empty() || !edgeIds.insert(e.id).second || !terminals.count(e.endpoint) || !connected.insert(e.endpoint).second)throw std::runtime_error("E_BOARD_CONNECTION");
        if(found==p.device.pads.end() || !found->second.reviewed || !found->second.available)throw std::runtime_error("E_BOARD_PAD_UNREVIEWED");
        nodes_.insert(e.net);nodes_.insert(e.endpoint);edges_.push_back(e);
        if(e.endpoint==clock_endpoint_)clock_pad_=pad;
        if(e.endpoint.ends_with(".contact") || e.endpoint.ends_with(".TX") || e.endpoint.find(".bit")!=std::string::npos)controls_[pad]=e.endpoint;
    }
    if(connected!=terminals || !clock_pad_)throw std::runtime_error("E_BOARD_CONNECTION");
    for(const auto& pull:d.value("fixed_drivers",Json::array())){
        auto node=pull.at("node").get<std::string>();int code=pull.at("state").get<int>();auto id=pull.at("driver_id").get<std::string>();
        if(!nodes_.count(node) || id.empty() || code<0 || code>255 || !ResolveM0({{id,uint8_t(code)}}).supported)throw std::runtime_error("E_BOARD_DRIVER");
        fixed_[node].push_back({id,uint8_t(code)});
    }
    for(const auto& [node,ds]:fixed_)if(!ResolveM0(ds).supported)throw std::runtime_error("E_BOARD_DRIVER");
    auto text=d.dump();fingerprint_=platform::Sha256Hex(text.data(),text.size());
}
std::string BoardTopology::ControlEndpoint(int pad)const{auto it=controls_.find(pad);if(it==controls_.end())throw std::runtime_error("E_BOARD_INPUT_ENDPOINT");return it->second;}
bool BoardTopology::HasEdge(const std::string& id)const{for(const auto& e:edges_)if(e.id==id)return true;return false;}
void BoardTopology::ValidateFaults(const FaultSet& fs)const{if(fs.size()>128)throw std::runtime_error("E_FAULT_QUOTA");for(const auto& [id,f]:fs)if(!HasEdge(f.edge) || id!=f.id || id.empty() || (f.end && *f.end<=f.start))throw std::runtime_error("E_FAULT_PROFILE");}
Json BoardTopology::Resolve(const std::map<std::string,std::vector<Driver>>& drivers,const FaultSet& faults,uint64_t tick)const{
    ValidateFaults(faults);std::map<std::string,std::string> parent;for(const auto& n:nodes_)parent[n]=n;
    auto root=[&](std::string n){while(parent.at(n)!=n)n=parent.at(n);return n;};Json wires=Json::object();
    for(const auto& e:edges_){bool closed=true;for(const auto& [id,f]:faults)if(f.edge==e.id && f.Active(tick))closed=false;
        wires[e.id]=closed;if(closed){auto a=root(e.net),b=root(e.endpoint);if(a!=b)parent[std::max(a,b)]=std::min(a,b);}}
    std::map<std::string,std::vector<Driver>> groups;
    auto add=[&](const auto& bynode){for(const auto& [n,ds]:bynode){if(!nodes_.count(n))throw std::runtime_error("E_BOARD_NODE");for(const auto& dr:ds)groups[root(n)].push_back({n+":"+dr.driver_id,dr.state});}};
    add(fixed_);add(drivers);Json nodes=Json::object(),leds=Json::object();
    for(const auto& n:nodes_){auto resolved=ResolveM0(groups[root(n)]);if(!resolved.supported)throw std::runtime_error(resolved.error);nodes[n]=StateJson(resolved);}
    for(const auto& [endpoint,active]:leds_){const auto& s=nodes.at(endpoint);leds[endpoint.substr(0,endpoint.size()-2)]={{"lit",s.at("valid")==true && s.at("logic")=="0" && s.at("contention")==false},{"state_code",s.at("state_code")}};}
    return {{"board_id",descriptor_.at("id")},{"nodes",nodes},{"wires",wires},{"leds",leds}};
}
}
