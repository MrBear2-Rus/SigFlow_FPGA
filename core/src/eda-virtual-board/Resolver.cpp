#include <eda/api/logic_state.hpp>
#include <algorithm>
namespace eda::virtual_board {
ImportDecision GuardInput(Direction direction,const Resolution& r){
    if(direction==Direction::Output)return ImportDecision::ObserveOnly;
    if(direction!=Direction::Input || !r.known())return ImportDecision::PauseUnknownInput;
    return r.high()?ImportDecision::Import1:ImportDecision::Import0;
}
std::string StateName(uint8_t s){
    if(s&ContentionFlag)return "CON";
    switch(State(s)){
    case State::UND:return "UND";case State::FLT:return "FLT";
    case State::WUD:return "WUD";case State::WLO:return "WLO";case State::WHI:return "WHI";
    case State::SUD:return "SUD";case State::SLO:return "SLO";case State::SHI:return "SHI";
    case State::ILO:return "ILO";case State::IHI:return "IHI";case State::PLO:return "PLO";case State::PHI:return "PHI";
    }return "INVALID";
}
Resolution ResolveM0(std::vector<Driver> ds){
    Resolution r;std::sort(ds.begin(),ds.end(),[](const auto& a,const auto& b){return a.driver_id<b.driver_id;});r.drivers=ds;
    int strongest=0;bool low=false,high=false,unknown=false,undefined=false,contention=false;
    std::string previous;
    for(const auto& d:ds){
        if(d.driver_id.empty() || d.driver_id==previous){r.supported=false;r.error="E_DRIVER_ID";return r;}previous=d.driver_id;
        uint8_t base=d.state&0x1f;
        int strength=0;
        switch(State(base)){
        case State::FLT:break;
        case State::UND:undefined=true;break;
        case State::WUD:case State::WLO:case State::WHI:strength=1;break;
        case State::SUD:case State::SLO:case State::SHI:strength=2;break;
        default:r.supported=false;r.error="E_STATE_UNSUPPORTED";return r;
        }
        if(d.state&0xc0){r.supported=false;r.error="E_STATE_INVALID";return r;}
        if(d.state&ContentionFlag)contention=true;
        if(strength==0)continue;
        if(strength>strongest){strongest=strength;low=high=unknown=false;}
        if(strength==strongest){
            if(!(base&1))unknown=true;else if(base&2)high=true;else low=true;
        }
    }
    if(undefined)r.state=uint8_t(State::UND);
    else if(!strongest)r.state=uint8_t(State::FLT);
    else if(unknown || (low&&high))r.state=uint8_t(strongest==1?State::WUD:State::SUD);
    else r.state=uint8_t(strongest==1?(high?State::WHI:State::WLO):(high?State::SHI:State::SLO));
    if(contention || (low&&high))r.state|=ContentionFlag;
    return r;
}
}
