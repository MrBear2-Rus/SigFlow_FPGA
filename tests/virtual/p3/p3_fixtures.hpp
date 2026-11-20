#pragma once
// Phase 3 native fixtures. The doubles are explicit algebra stand-ins, exactly
// like k4_k5/TestCounter.h; separate CTest entries build real Verilator
// runners from examples/virtual/phase3 for the product-level acceptance.
#include "../k2/fixture_support.hpp"
#include "eda-virtual-board/BoardTopology.h"
#include "eda-virtual-board/Phase1Faults.h"
#include <sstream>
namespace p3test {
namespace vb=eda::virtual_board;using eda::Json;
inline vb::PortManifest Manifest(bool scan){
    vb::PortManifest m;m.top_module=scan?"p3_scan":"p3_probe";m.source_sha256=std::string(64,'c');
    for(const auto& [name,width,output]:std::initializer_list<std::tuple<std::string,uint32_t,bool>>{
        {"clock_in",1,false},{"reset_n",1,false},{scan?"row_drive":"row_sense",4,scan},{"col_sense",4,false},{"led_n",6,true}}){
        vb::PortDecl p;p.rtl_name=name;p.port_id=m.top_module+"::"+name;p.source_ref="test-double";
        p.direction=output?vb::Direction::Output:vb::Direction::Input;
        if(width>1){p.shape=vb::PortShape::PackedVector;p.decl_left=int32_t(width-1);p.decl_right=0;}
        p.width=width;
        for(uint32_t bit=0;bit<width;++bit)p.bits.push_back({p.port_id,width>1?std::optional<int64_t>(bit):std::nullopt,bit});
        m.ports.push_back(p);
    }return m;
}
// Passive prober: no row drivers, mirrors two rows plus four columns on the
// LEDs so the classic ghost-key path stays electrically observable.
class Probe:public vb::IDutAdapter {
    bool clock_=false,last_=false,reset_=false,finalized_=false;bool rows_[4]={},cols_[4]={};unsigned pattern_=0b111111;
public:
    void WriteInput(const vb::PortBitRef& r,bool v)override{
        if(r.port_id.ends_with("clock_in"))clock_=v;else if(r.port_id.ends_with("reset_n"))reset_=v;
        else if(r.port_id.ends_with("row_sense"))rows_[size_t(r.bit_slot)]=v;else if(r.port_id.ends_with("col_sense"))cols_[size_t(r.bit_slot)]=v;}
    void Evaluate(const vb::EventPosition&)override{
        if(!reset_)pattern_=0b111111;
        else if(clock_ && !last_){unsigned value=0;for(int i=0;i<4;++i)value|=unsigned(!cols_[i])<<i;value|=unsigned(!rows_[0])<<4;value|=unsigned(!rows_[1])<<5;pattern_=value;}
        last_=clock_;}
    bool ReadOutput(const vb::PortBitRef& r)const override{return !(pattern_&(1u<<r.bit_slot));}
    void Finalize()override{if(finalized_)throw std::runtime_error("E_FINAL_TWICE");finalized_=true;}
};
inline vb::VirtualSession::Factory ProbeFactory(){return []{return std::make_unique<Probe>();};}
// Push-pull one-hot scanner double: drives one row low, the other rows high,
// so diode-less multi-key presses short the scan network into real CON.
class Scanner:public vb::IDutAdapter {
    bool clock_=false,last_=false,reset_=false,finalized_=false;bool cols_[4]={};unsigned divider_=0,row_=0;
public:
    void WriteInput(const vb::PortBitRef& r,bool v)override{
        if(r.port_id.ends_with("clock_in"))clock_=v;else if(r.port_id.ends_with("reset_n"))reset_=v;
        else if(r.port_id.ends_with("col_sense"))cols_[size_t(r.bit_slot)]=v;}
    void Evaluate(const vb::EventPosition&)override{
        if(!reset_){divider_=0;row_=0;}
        else if(clock_ && !last_){if(++divider_>=2700){divider_=0;row_=(row_+1)&3;}}
        last_=clock_;}
    bool ReadOutput(const vb::PortBitRef& r)const override{
        if(r.port_id.ends_with("row_drive"))return row_!=size_t(r.bit_slot);
        if(r.port_id.ends_with("led_n"))return r.bit_slot<4?cols_[size_t(r.bit_slot)]:true;return true;}
    void Finalize()override{if(finalized_)throw std::runtime_error("E_FINAL_TWICE");finalized_=true;}
};
inline vb::VirtualSession::Factory ScannerFactory(){return []{return std::make_unique<Scanner>();};}
inline vb::SessionContract Contract(const std::filesystem::path& root,bool scan){
    auto m=Manifest(scan);
    vb::SessionContract c;c.manifest=m;c.identity={"GW1NR-LV9QN88PC6/I5","QFN88","6202"};
    auto file=root/"registry/electrical/tang-nano-9k.6202.reviewed.v2.json";
    std::vector<vb::Diagnostic> ds;if(!vb::LoadElectricalProfile(file,c.electrical,ds))throw std::runtime_error("profile");
    c.electrical_sha256=eda::platform::Sha256FileHex(file);
    c.adapter_sha256=std::string(64,'d');c.run_id=scan?"p3-scan-reference":"p3-probe-reference";
    c.board=vb::BoardTopology::Load(root/"registry/boards/tang-nano-9k.phase3.virtual.json",c.electrical);
    c.required_clock=m.ports.at(0).bits.at(0);
    std::stringstream cst;
    cst<<"IO_LOC \"clock_in\" 52;\nIO_PORT \"clock_in\" IO_TYPE=LVCMOS33 PULL_MODE=NONE;\n";
    cst<<"IO_LOC \"reset_n\" 4;\nIO_PORT \"reset_n\" IO_TYPE=LVCMOS18 PULL_MODE=NONE;\n";
    for(auto& [port,pad]:std::initializer_list<std::pair<const char*,int>>{
        {scan?"row_drive[0]":"row_sense[0]",25},{scan?"row_drive[1]":"row_sense[1]",26},{scan?"row_drive[2]":"row_sense[2]",27},{scan?"row_drive[3]":"row_sense[3]",28},
        {"col_sense[0]",29},{"col_sense[1]",30},{"col_sense[2]",33},{"col_sense[3]",34},
        {"led_n[0]",10},{"led_n[1]",11},{"led_n[2]",13},{"led_n[3]",14},{"led_n[4]",15},{"led_n[5]",16}})
        cst<<"IO_LOC \""<<port<<"\" "<<pad<<";\nIO_PORT \""<<port<<"\" IO_TYPE="<<((pad>=10&&pad<=16)||pad==4?"LVCMOS18":"LVCMOS33")<<" PULL_MODE=NONE;\n";
    auto imported=eda::cst::CstGenerator().ImportCst(cst.str(),m);
    if(!imported.ok())throw std::runtime_error("P3 CST import: "+(imported.diagnostics.empty()?std::string("unknown"):imported.diagnostics.front().code+" "+imported.diagnostics.front().message+" @"+std::to_string(imported.diagnostics.front().row)));c.bindings=imported.bindings;
    eda::cst::ConstraintSheetV2 sheet;sheet.manifest=m;sheet.bindings=c.bindings;sheet.device=c.identity.device;sheet.topModule=m.top_module;
    if(!eda::cst::ConstraintValidator::Validate(sheet,&c.electrical).empty())throw std::runtime_error("P3 K1 gate");
    if(scan)c.inputs={{"reset_p4",4,{},uint8_t(vb::State::SLO)},{"clock_p52",52,{},uint8_t(vb::State::FLT)},
        {"col0",29,{},uint8_t(vb::State::FLT)},{"col1",30,{},uint8_t(vb::State::FLT)},
        {"col2",33,{},uint8_t(vb::State::FLT)},{"col3",34,{},uint8_t(vb::State::FLT)}};
    else c.inputs={{"reset_p4",4,{},uint8_t(vb::State::SLO)},{"clock_p52",52,{},uint8_t(vb::State::FLT)},
        {"keypad_row0",25,{},uint8_t(vb::State::FLT)},{"keypad_row1",26,{},uint8_t(vb::State::FLT)},
        {"keypad_row2",27,{},uint8_t(vb::State::FLT)},{"keypad_row3",28,{},uint8_t(vb::State::FLT)},
        {"col0",29,{},uint8_t(vb::State::FLT)},{"col1",30,{},uint8_t(vb::State::FLT)},
        {"col2",33,{},uint8_t(vb::State::FLT)},{"col3",34,{},uint8_t(vb::State::FLT)}};
    c.phase1=true;c.phase2=true;c.fault_profile=vb::Phase1Faults::EmptyProfile(*c.board);
    c.adapter_sha256=std::string(64,'d');return c;
}
inline Json Cmd(std::string op,std::string id){return {{"op",op},{"command_id",id}};}
inline Json Set(std::string target,int state,std::string id){return {{"op","set"},{"command_id",id},{"target",target},{"state",state}};}
inline Json Keys(std::string id,Json keys){return {{"op","keypad.set"},{"command_id",id},{"target","keypad0"},{"keys",keys}};}
inline Json Rule(std::string id,std::string type,std::string target,std::string kind,std::string hook,Json effect=Json::object()){
    effect["type"]=type;return {{"id",id},{"enabled",true},{"visibility","student_visible"},{"target",{{"id",target},{"kind",kind}}},{"hook",hook},{"effect",effect},{"trigger",{{"kind","permanent"},{"start_tick","0"}}}};}
}
