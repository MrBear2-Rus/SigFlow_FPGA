#include "Phase3Faults.h"
#include <set>
namespace eda::virtual_board {
namespace {
// 10^12*den must stay inside uint64_t for the rational half-period accumulator.
constexpr uint64_t MaxClockDen=18446744ULL;
}
bool ValidatePhase3Fault(const BoardTopology& board,const Json& f){
    std::string type=f.at("effect").at("type"),hook=f.at("hook"),target=f.at("target").at("id"),kind=f.at("target").at("kind");
    const auto& e=f.at("effect");std::set<std::string> fields{"type"};
    bool known=true,valid=true;std::string model;
    for(const auto& c:board.Descriptor().at("components"))if(c.at("id")==target)model=c.at("model");
    const auto& trigger=f.at("trigger");
    if(type=="reset_glitch"){
        fields.insert("pressed_state");fields.insert("duration_ticks");
        valid=hook=="peripheral.event" && kind=="component" && model=="sigflow.button@1";
        valid&=e.at("pressed_state").is_number_integer() && e.at("pressed_state").get<int>()>=0 && e.at("pressed_state").get<int>()<=1;
        valid&=trigger.at("kind")!="intermittent";
        if(valid){
            auto duration=P1Tick(e.at("duration_ticks"));
            valid&=duration>0;
            if(trigger.at("kind")=="transient"){
                auto start=P1Tick(trigger.at("start_tick")),end=P1Tick(trigger.at("end_tick"));
                // The pulse must finish inside its activation window; the
                // deactivation reset is the release edge itself.
                valid&=end>=P1Add(start,duration);
            }
        }
    }else if(type=="clock_divider_offset"){
        fields.insert("den_offset");fields.insert("resume_phase");
        valid=hook=="clock.schedule" && kind=="clock" && target==board.Descriptor().at("clocks").at(0).at("id").get<std::string>();
        valid&=e.at("resume_phase")=="restart_half_period" && trigger.at("kind")!="intermittent";
        if(valid){
            const auto& frequency=board.Descriptor().at("clocks").at(0).at("frequency_hz");
            auto num=P1Tick(frequency.at("num")),base=P1Tick(frequency.at("den")),offset=P1Tick(e.at("den_offset"));
            auto effective=P1Add(base,offset);
            valid&=offset>=1 && effective<=MaxClockDen && 1000000000000ULL*effective>=2*num;
        }
    }else known=false;
    if(!known)return false;
    for(const auto& [field,value]:e.items())if(!fields.count(field))throw std::runtime_error("E_P3_FAULT_FIELD: "+field);
    if(!valid)throw std::runtime_error("E_P3_FAULT_PARAMETERS");
    return true;
}
}
