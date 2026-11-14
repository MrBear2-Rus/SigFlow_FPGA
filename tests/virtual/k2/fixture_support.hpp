#pragma once
#include "ConstraintSheet.h"
#include "eda-virtual-board/VirtualSession.h"
#include "eda-virtual-board/ElectricalValidator.h"
#include "eda-platform/Sha256.h"
#include <fstream>
namespace k2fixture {
namespace vb=eda::virtual_board;
inline vb::SessionContract Contract(const std::filesystem::path& root,const vb::PortManifest& m){
    vb::SessionContract c;c.manifest=m;c.identity={"GW1NR-LV9QN88PC6/I5","QFN88","6202"};
    auto file=root/"registry/electrical/tang-nano-9k.6202.reviewed.v1.json";
    std::vector<vb::Diagnostic> ds;if(!vb::LoadElectricalProfile(file,c.electrical,ds))throw std::runtime_error("profile");
    c.electrical_sha256=eda::platform::Sha256FileHex(file);
    c.adapter_sha256=std::string(64,'b');c.run_id="k2-reference";
    std::ifstream f(root/"tests/virtual/k2/counter.cst");std::string text((std::istreambuf_iterator<char>(f)),{});
    auto imported=eda::cst::CstGenerator().ImportCst(text,m);
    if(!imported.ok())throw std::runtime_error("CST import");c.bindings=imported.bindings;
    eda::cst::ConstraintSheetV2 s;s.manifest=m;s.bindings=c.bindings;s.device=c.identity.device;s.topModule=m.top_module;
    if(!eda::cst::ConstraintValidator::Validate(s,&c.electrical).empty())throw std::runtime_error("K1 gate");
    c.inputs={{"key_p3",3,{{"board-pullup",uint8_t(vb::State::WHI)}},uint8_t(vb::State::FLT)},
              {"reset_p4",4,{{"board-pullup",uint8_t(vb::State::WHI)}},uint8_t(vb::State::SLO)},
              {"clock_p52",52,{},uint8_t(vb::State::FLT)}};
    return c;
}
}
