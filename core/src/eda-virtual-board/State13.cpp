#include "State13.h"
#include <algorithm>
#include <stdexcept>
namespace eda::virtual_board {
namespace {
constexpr uint8_t Base(uint8_t a,uint8_t b){
    // Product identity is FLT=2. The reverse-engineering prose/matrix used
    // conflicting UND/FLT names; do not import its claimed identity at index 0.
    if(a==b)return a;if(a==2)return b;if(b==2)return a;
    auto sa=a&28,sb=b&28;if(sa==sb)return uint8_t(sa|2|32);
    auto strong=sa>sb?a:b,weak=sa>sb?b:a;
    return uint8_t(strong|(((weak&28)>=12 && (strong&3)!=(weak&3) && (strong&28)!=20)?32:0));
}
constexpr auto MakeTable(){std::array<uint8_t,1024> t{};for(int a=0;a<32;++a)for(int b=0;b<32;++b)t[a*32+b]=Base(a,b);return t;}
constexpr auto table=MakeTable();
std::string Level(uint8_t s){return (s&32)?"conflict":!(s&1)?s==2?"floating":"unknown":(s&2)?"high":"low";}
std::vector<Driver> Ordered(std::vector<Driver> ds){std::sort(ds.begin(),ds.end(),[](const auto& a,const auto& b){return a.driver_id<b.driver_id;});std::string previous;for(const auto& d:ds){if(d.driver_id.empty() || d.driver_id==previous)throw std::runtime_error("E_DRIVER_ID");if(d.state&192)throw std::runtime_error("E_STATE_INVALID");auto b=d.state&31;bool declared=b==0 || b==2 || b==4 || b==5 || b==7 || b==12 || b==13 || b==15 || b==21 || b==23 || b==29 || b==31;bool internal=(d.state&32) && (b==6 || b==14 || b==22 || b==30);if(!declared && !internal)throw std::runtime_error("E_STATE_UNSUPPORTED");previous=d.driver_id;}return ds;}
}
const std::array<uint8_t,1024>& State13Table(){return table;}
uint8_t CombineState13(uint8_t a,uint8_t b){if((a|b)&192)throw std::runtime_error("E_STATE_INVALID");auto sa=a&28,sb=b&28;auto retained=sa>sb?(a&32):sa<sb?(b&32):((a|b)&32);return table[(a&31)*32+(b&31)]|retained;}
Resolution ResolveState13(std::vector<Driver> ds){Resolution r;try{r.drivers=Ordered(std::move(ds));for(const auto& d:r.drivers)r.state=CombineState13(r.state,d.state);}catch(const std::exception& e){r.supported=false;r.error=e.what();}return r;}
Json ExplainState13(std::vector<Driver> ds){ds=Ordered(std::move(ds));Json drivers=Json::array(),steps=Json::array(),conflicts=Json::array();uint8_t state=2;size_t i=0;bool history=false;
    for(const auto& d:ds){auto colon=d.driver_id.find(':');drivers.push_back({{"driver_id",d.driver_id},{"source_endpoint",d.driver_id.substr(0,colon)},{"source",colon==std::string::npos?d.driver_id:d.driver_id.substr(colon+1)},{"state_code",d.state},{"strength",(d.state&28)>>2},{"level",Level(d.state)}});auto next=CombineState13(state,d.state);history|=bool((state|d.state|next)&32);steps.push_back({{"step_index",i++},{"lhs_state",state},{"rhs_driver_id",d.driver_id},{"rhs_state",d.state},{"result_state",next},{"contention_triggered",bool(table[(state&31)*32+(d.state&31)]&32)}});state=next;}
    for(size_t a=0;a<ds.size();++a)for(size_t b=a+1;b<ds.size();++b)if(CombineState13(ds[a].state,ds[b].state)&32)conflicts.push_back({{"lhs_driver_id",ds[a].driver_id},{"rhs_driver_id",ds[b].driver_id},{"lhs_state",ds[a].state},{"rhs_state",ds[b].state},{"reason","opposing_or_indeterminate_drivers"}});
    auto result=ResolveState13(ds);return {{"schema","sigflow.resolution-explanation.v1"},{"resolver",State13Version},{"drivers",drivers},{"fold_steps",steps},{"conflict_sources",conflicts},{"state_code",state},{"logic",result.known()?(result.high()?"1":"0"):state==2?"z":"x"},{"valid",result.known()},{"contention",bool(state&32)},{"sticky_conflict",history},{"summary",state==2?"FLT: no remaining active driver":state&32?"CON: inspect retained sources and each table fold":!(state&1)?"Unknown: the surviving source has no valid polarity":"The strongest surviving source determines the nominal logic level"}};
}
uint32_t GetHighMask13(const std::vector<Driver>& ds){if(ds.size()>32)throw std::runtime_error("E_MASK_WIDTH");uint32_t mask=0;for(size_t i=0;i<ds.size();++i)if((ds[i].state&3)==3 && !(ds[i].state&32))mask|=uint32_t(1)<<i;return mask;}
uint32_t GetDrivenMask13(const std::vector<Driver>& ds){if(ds.size()>32)throw std::runtime_error("E_MASK_WIDTH");uint32_t mask=0;for(size_t i=0;i<ds.size();++i)if(ds[i].state&28)mask|=uint32_t(1)<<i;return mask;}
}
