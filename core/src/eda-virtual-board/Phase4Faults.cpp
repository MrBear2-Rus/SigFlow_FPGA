#include "Phase4Faults.h"
#include <set>
namespace eda::virtual_board {
bool ValidatePhase4Fault(const BoardTopology& board,const Json& f){
    std::string type=f.at("effect").at("type"),hook=f.at("hook"),target=f.at("target").at("id"),kind=f.at("target").at("kind");
    const auto& e=f.at("effect");std::set<std::string> fields{"type"};
    bool known=true,valid=true;std::string model;
    for(const auto& c:board.Descriptor().at("components"))if(c.at("id")==target)model=c.at("model");
    const auto& trigger=f.at("trigger");
    if(type=="i2c_nak"){
        fields.insert("phase");fields.insert("nth_transaction");
        valid=hook=="peripheral.event" && kind=="component" && model=="sigflow.i2c-sensor@1";
        valid&=e.at("phase").is_string() && (e.at("phase")=="address" || e.at("phase")=="data");
        valid&=e.at("nth_transaction").is_number_integer() && e.at("nth_transaction").get<int64_t>()>=1;
        valid&=trigger.at("kind")!="intermittent";
    }else if(type=="spi_mode_fault"){
        fields.insert("mode");fields.insert("slip_bits");
        valid=hook=="peripheral.event" && kind=="component" && model=="sigflow.spi-oled@1";
        valid&=e.at("mode").is_string() && (e.at("mode")=="invert_cpha" || e.at("mode")=="bit_slip");
        valid&=trigger.at("kind")!="intermittent";
        if(valid && e.at("mode")=="bit_slip"){
            valid&=e.contains("slip_bits") && e.at("slip_bits").is_number_integer() && e.at("slip_bits").get<int64_t>()>=1 && e.at("slip_bits").get<int64_t>()<=7;
        }
    }else if(type=="crc_tamper"){
        fields.insert("protocol_id");fields.insert("xor_mask");fields.insert("frame_index");
        valid=hook=="protocol.transform" && kind=="component" && model=="sigflow.uart-peer@1";
        valid&=e.at("protocol_id")==std::string(UartPacketCrc8);
        valid&=e.at("xor_mask").is_number_integer() && e.at("xor_mask").get<int64_t>()>=0 && e.at("xor_mask").get<int64_t>()<=255;
        valid&=e.at("frame_index").is_number_integer() && e.at("frame_index").get<int64_t>()>=0;
        valid&=trigger.at("kind")!="intermittent";
    }else if(type=="cdc_sampling_approx"){
        fields.insert("window_ps");fields.insert("max_delay_cycles");fields.insert("policy");
        valid=hook=="input.sampling" && kind=="input" && !target.empty();
        valid&=e.at("policy")==std::string("seeded_enum");
        valid&=e.at("max_delay_cycles").is_number_integer() && e.at("max_delay_cycles").get<int64_t>()>=1 && e.at("max_delay_cycles").get<int64_t>()<=2;
        if(valid)P1Tick(e.at("window_ps"));
        valid&=trigger.at("kind")!="intermittent";
    }else known=false;
    if(!known)return false;
    for(const auto& [field,value]:e.items())if(!fields.count(field))throw std::runtime_error("E_P4_FAULT_FIELD: "+field);
    if(!valid)throw std::runtime_error("E_P4_FAULT_PARAMETERS");
    return true;
}
}
