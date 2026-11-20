#include "BlindProjection.h"
#include "eda-platform/Sha256.h"
#include <set>
namespace eda::virtual_board {
namespace {
Json Fields(const Json& in,std::initializer_list<const char*> fields){Json out=Json::object();for(auto key:fields)if(in.contains(key))out[key]=in.at(key);return out;}
Json Node(const Json& in){auto out=Fields(in,{"state_code","logic","valid","contention"});if(!in.contains("explanation"))return out;const auto& raw=in.at("explanation");auto ex=Fields(raw,{"schema","resolver","state_code","logic","valid","contention","sticky_conflict"});std::map<std::string,std::string> names;Json drivers=Json::array(),steps=Json::array(),sources=Json::array();for(const auto& d:raw.at("drivers")){std::string name="observed-source-"+std::to_string(names.size()+1);names[d.at("driver_id")]=name;auto copy=Fields(d,{"state_code","strength","level"});copy["driver_id"]=name;drivers.push_back(copy);}for(const auto& s:raw.at("fold_steps")){auto copy=Fields(s,{"step_index","lhs_state","rhs_state","result_state","contention_triggered"});copy["rhs_driver_id"]=names.at(s.at("rhs_driver_id"));steps.push_back(copy);}for(const auto& s:raw.at("conflict_sources")){auto copy=Fields(s,{"lhs_state","rhs_state"});copy["lhs_driver_id"]=names.at(s.at("lhs_driver_id"));copy["rhs_driver_id"]=names.at(s.at("rhs_driver_id"));sources.push_back(copy);}ex["drivers"]=drivers;ex["fold_steps"]=steps;ex["conflict_sources"]=sources;ex["summary"]="Observed electrical state; use paired probes and fold steps.";out["explanation"]=ex;return out;}
Json Board(const Json& in){auto out=Fields(in,{"board_id","leds","peripherals"});out["nodes"]=Json::object();for(const auto& [id,node]:in.at("nodes").items())out["nodes"][id]=Node(node);return out;}
}
bool StudentCommandAllowed(const std::string& op){static const std::set<std::string> allowed={"get","step","play","pause","stop","set","pulse","uart.send","dip.set","keypad.set","trace.commits","probe.attach","probe.detach","probe.read","capture.arm","capture.read","capture.compare","evidence.query","journal","replay.current"};return allowed.count(op);}
Json StudentProjection(const Json& in,const std::string& token){
    auto out=Fields(in,{"status","command_id","state","reason","tick_ps","position","attempted_position","topology_position","outputs","input_writes","eval_calls","final_calls","pending_count","pending_inputs","binding_status","signals","batches","next_revision","complete","generation","matched_commits","repetitions"});out["run_token"]=token;out["run_id"]=token;out["mode"]="student_blind_test";
    if(in.contains("state")){out["phase1"]=true;out["phase2"]=true;}
    if(in.contains("board"))out["board"]=Board(in.at("board"));if(in.contains("trace")){out["trace"]=in.at("trace");out["trace"]["run_id"]=token;}
    if(in.contains("guard"))out["guard"]=Fields(in.at("guard"),{"port_ref","package_pin","state","label","attempted_position","last_committed_position"});
    if(in.contains("error"))out["error"]=in.value("reason",std::string())=="UnknownInput"?"E_UNKNOWN_INPUT":in.value("reason",std::string())=="NoClockEdge"?"E_NO_CLOCK_EDGE":in.at("error")==""?"":"E_BLIND_OPERATION_DENIED";
    if(in.contains("records")){out["schema"]="sigflow.public-observation-journal.v1";out["records"]=Json::array();for(const auto& r:in.at("records"))if(r.value("kind",std::string())=="commit"){auto copy=Fields(r,{"position","outputs","trace_events"});copy["kind"]="observation";if(r.contains("board"))copy["board"]=Board(r.at("board"));out["records"].push_back(copy);}}
    if(in.contains("instrument_reply")){const auto& reply=in.at("instrument_reply");auto copy=Fields(reply,{"probe_id","target","position","nominal_mv","voltage_model","read_only","schema","source","layout","armed","triggered","trigger_index","decimation","sample_clock_hz","clock_pad","targets","samples"});if(reply.contains("state"))copy["state"]=Node(reply.at("state"));out["instrument_reply"]=copy;}
    if(in.contains("digests")){out["digests"]=Json::object();for(auto key:{"circuit","control"}){auto value=token+":"+in.at("digests").at(key).get<std::string>();out["digests"][key]=platform::Sha256Hex(value.data(),value.size());}out["digests"]["algorithm"]="sigflow.public-run-digest.v1";}
    return out;
}
}
