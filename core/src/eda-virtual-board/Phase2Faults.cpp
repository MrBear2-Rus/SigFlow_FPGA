#include "Phase2Faults.h"
#include <set>
namespace eda::virtual_board {
bool ValidatePhase2Fault(const BoardTopology& board,const Json& f){
    if(!board.UsesState13())return false;std::string type=f.at("effect").at("type"),hook=f.at("hook"),target=f.at("target").at("id"),kind=f.at("target").at("kind");const auto& e=f.at("effect");std::set<std::string> fields{"type"};
    bool known=true,valid=true;std::string model;for(const auto& c:board.Descriptor().at("components"))if(c.at("id")==target)model=c.at("model");
    if(type=="bridge"){fields.insert("other_node");valid=hook=="topology.transform" && kind=="node" && board.Nodes().count(target) && e.at("other_node").is_string() && board.Nodes().count(e.at("other_node")) && e.at("other_node")!=target;}
    else if(type=="pull_removal"){valid=hook=="driver.transform" && kind=="driver";bool found=false;for(const auto& pull:board.Descriptor().value("fixed_drivers",Json::array()))found|=target==pull.at("node").get<std::string>()+":"+pull.at("driver_id").get<std::string>();auto colon=target.find(':');found|=colon!=std::string::npos && board.Nodes().count(target.substr(0,colon)) && (target.substr(colon+1)=="cst.pullup" || target.substr(colon+1)=="cst.pulldown");valid&=found;}
    else if(type=="mapping_overlay"){fields.insert("bindings");valid=hook=="mapping.overlay" && kind=="pad_map" && target=="effective" && e.at("bindings").is_array() && !e.at("bindings").empty() && e.at("bindings").size()<=88;for(const auto& b:e.at("bindings")){valid&=b.is_object() && b.size()==2 && b.contains("port_ref") && b.at("port_ref").is_string() && b.contains("pad");if(!b.at("pad").is_null())valid&=b.at("pad").is_number_integer() && board.Nodes().count("pad."+std::to_string(b.at("pad").get<int>()));}}
    else if(type=="led_polarity_reverse")valid=hook=="peripheral.observe" && kind=="component" && model=="sigflow.led@1";
    else if(type=="common_mismatch")valid=hook=="peripheral.observe" && kind=="component" && model=="sigflow.seven-segment-mux@1";
    else if(type=="dead_segment"){fields.insert("segment");valid=hook=="peripheral.observe" && kind=="component" && model=="sigflow.seven-segment-mux@1" && e.at("segment").is_string();std::string s=e.at("segment");valid&=s=="dp" || (s.size()==1 && s[0]>='a' && s[0]<='g');}
    else if(type=="uart_baud_offset_ppm"){fields.insert("offset_ppm");valid=hook=="protocol.transform" && kind=="component" && model=="sigflow.uart-peer@1" && e.at("offset_ppm").is_number_integer() && e.at("offset_ppm").get<int64_t>()>=-500000 && e.at("offset_ppm").get<int64_t>()<=500000;}
    else if(type=="uart_framing_error"){fields.insert("bit");valid=hook=="protocol.transform" && kind=="component" && model=="sigflow.uart-peer@1" && (e.at("bit")=="start" || e.at("bit")=="stop");}
    else if(type=="uart_parity_flip")valid=hook=="protocol.transform" && kind=="component" && model=="sigflow.uart-peer@1";
    else known=false;
    if(!known)return false;for(const auto& [field,value]:e.items())if(!fields.count(field))throw std::runtime_error("E_P2_FAULT_FIELD");if(!valid || f.at("trigger").at("kind")=="intermittent")throw std::runtime_error("E_P2_FAULT_PARAMETERS");return true;
}
Json Phase2TopologyOverlay(const Json& rules){Json bridges=Json::array(),removed=Json::array();for(const auto& r:rules){auto type=r.at("effect").at("type");if(type=="bridge")bridges.push_back({{"lhs",r.at("target").at("id")},{"rhs",r.at("effect").at("other_node")}});if(type=="pull_removal")removed.push_back(r.at("target").at("id"));}return {{"bridges",bridges},{"removed_drivers",removed}};}
std::vector<PinBindingV2> Phase2Bindings(const PortManifest& m,const std::vector<PinBindingV2>& original,const BoardTopology& board,const Json& rules){
    auto result=original;std::set<std::string> touched;for(const auto& r:rules)if(r.at("effect").at("type")=="mapping_overlay")for(const auto& item:r.at("effect").at("bindings")){std::string ref=item.at("port_ref");if(!touched.insert(ref).second)throw std::runtime_error("E_MAPPING_CONFLICT");bool found=false;for(auto it=result.begin();it!=result.end();++it)if(m.Label(it->ref)==ref){found=true;if(item.at("pad").is_null())result.erase(it);else it->package_pin=item.at("pad").get<int>();break;}if(!found)throw std::runtime_error("E_MAPPING_PORT");}
    std::set<int> pads;for(const auto& b:result)if(!pads.insert(b.package_pin).second || !board.Nodes().count("pad."+std::to_string(b.package_pin)))throw std::runtime_error("E_MAPPING_PAD");return result;
}
}
