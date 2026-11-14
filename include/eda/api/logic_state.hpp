#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "port_manifest.hpp"
namespace eda::virtual_board {
// SigFlow-owned encoding v1. I = Ideal. CON is a flag, not a table row.
enum class State : uint8_t {
    UND=0x00, FLT=0x02, WUD=0x04, WLO=0x05, WHI=0x07,
    SUD=0x0c, SLO=0x0d, SHI=0x0f, ILO=0x15, IHI=0x17,
    PLO=0x1d, PHI=0x1f
};
inline constexpr uint8_t ContentionFlag=0x20;
inline constexpr const char* ResolverVersion="sigflow.state-m0.v1";
struct Driver { std::string driver_id; uint8_t state=uint8_t(State::FLT); };
struct Resolution {
    uint8_t state=uint8_t(State::FLT);
    bool supported=true;
    std::vector<Driver> drivers; // stable driver_id ordering, retained for diagnosis
    std::string error;
    bool known() const {return supported && !(state&ContentionFlag) && (state&1);}
    bool high() const {return (state&2)!=0;}
};
// Initial production subset: F/W/S drivers. Ideal/Power are named but rejected
// until their complete table semantics have separate acceptance vectors.
Resolution ResolveM0(std::vector<Driver>);
std::string StateName(uint8_t);
enum class ImportDecision { ObserveOnly, Import0, Import1, PauseUnknownInput };
ImportDecision GuardInput(Direction,const Resolution&);
}
