#pragma once
#include "../k2/fixture_support.hpp"
#include "eda-virtual-board/BoardTopology.h"
// Explicit test double. A separate CTest compiles the real SystemVerilog runner.
namespace m0test {
namespace vb=eda::virtual_board;using eda::Json;
inline vb::PortManifest Manifest(){vb::PortManifest m;m.top_module="k2_counter";m.source_sha256=std::string(64,'a');
    for(const std::string name:{"clock_in","reset_n","key_n","led_n"}){vb::PortDecl p;p.rtl_name=name;p.port_id="k2_counter::"+name;p.source_ref="test-double";
        p.direction=name=="led_n"?vb::Direction::Output:vb::Direction::Input;
        if(name=="led_n"){p.shape=vb::PortShape::PackedVector;p.width=6;p.decl_left=5;p.decl_right=0;}
        for(uint32_t bit=0;bit<p.width;++bit)p.bits.push_back({p.port_id,name=="led_n"?std::optional<int64_t>(bit):std::nullopt,bit});m.ports.push_back(p);
    }return m;
}
class Counter:public vb::IDutAdapter {
    bool clock=false,last=false,reset=false,key=true,finalized=false;unsigned count=0;
public:
    void WriteInput(const vb::PortBitRef& r,bool v)override{if(r.port_id.ends_with("clock_in"))clock=v;else if(r.port_id.ends_with("reset_n"))reset=v;else key=v;}
    void Evaluate(const vb::EventPosition&)override{if(!reset)count=0;else if(clock&&!last&&!key)count=(count+1)&63;last=clock;}
    bool ReadOutput(const vb::PortBitRef& r)const override{return !(count&(1u<<r.bit_slot));}
    void Finalize()override{if(finalized)throw std::runtime_error("E_FINAL_TWICE");finalized=true;}
};
inline vb::SessionContract Contract(const std::filesystem::path& root){auto c=k2fixture::Contract(root,Manifest());
    c.board=vb::BoardTopology::Load(root/"registry/boards/tang-nano-9k.virtual.json",c.electrical);
    c.required_clock=c.manifest.ports.at(0).bits.at(0);for(auto& input:c.inputs)input.fixed_drivers.clear();return c;
}
inline vb::VirtualSession::Factory Factory(){return []{return std::make_unique<Counter>();};}
inline Json Cmd(std::string op,std::string id){return {{"op",op},{"command_id",id}};}
inline Json Set(std::string target,int state,std::string id){return {{"op","set"},{"command_id",id},{"target",target},{"state",state}};}
inline Json Open(std::string edge,std::string id="open"){return {{"op","fault.apply"},{"command_id",id},{"fault_id",id},{"edge_id",edge}};}
inline Json Start(vb::VirtualSession& s){s.Submit(Set("reset_p4",2,"release-reset"));s.Submit(Set("key_p3",13,"press"));return s.Submit(Cmd("step","first"));}
}
