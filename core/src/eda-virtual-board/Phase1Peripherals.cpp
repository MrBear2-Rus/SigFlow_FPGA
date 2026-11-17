#include "Phase1Peripherals.h"
#include <algorithm>
#include <limits>
namespace eda::virtual_board {
DutyIntegrator::DutyIntegrator(uint64_t window):window_(window){if(!window)throw std::runtime_error("E_INTEGRATION_WINDOW");}
void DutyIntegrator::Advance(uint64_t tick){
    if(tick<tick_)throw std::runtime_error("E_TIME_ORDER");if(tick>tick_){
        if(!intervals_.empty() && intervals_.back().active==active_ && intervals_.back().valid==valid_)intervals_.back().end=tick;
        else{if(intervals_.size()>=1000000)throw std::runtime_error("E_INTEGRATION_QUOTA");intervals_.push_back({tick_,tick,active_,valid_});}
        if(active_ && valid_)active_ticks_+=tick-tick_;if(valid_)valid_ticks_+=tick-tick_;tick_=tick;
    }
    auto begin=tick>window_?tick-window_:0;
    while(!intervals_.empty() && intervals_.front().begin<begin){auto& first=intervals_.front();auto cutoff=std::min(first.end,begin),amount=cutoff-first.begin;
        if(first.active && first.valid)active_ticks_-=amount;if(first.valid)valid_ticks_-=amount;first.begin=cutoff;if(first.begin==first.end)intervals_.pop_front();else break;}
}
Json DutyIntegrator::Snapshot()const{auto measured=std::min(tick_,window_);uint32_t ppm=measured?uint32_t((static_cast<unsigned __int128>(active_ticks_)*1000000)/measured):0;
    return {{"active_ticks",std::to_string(active_ticks_)},{"window_ticks",std::to_string(measured)},{"integration_window_ticks",std::to_string(window_)},{"valid_ticks",std::to_string(valid_ticks_)},{"unknown_ticks",std::to_string(measured-valid_ticks_)},{"duty_ppm",ppm},{"instant_active",active_},{"instant_valid",valid_}};}
Phase1Peripherals::Phase1Peripherals(const BoardTopology& board,uint64_t window):board_(&board){
    for(const auto& c:board.Descriptor().at("components")){std::string id=c.at("id"),model=c.at("model");auto integration=c.value("observe",Json::object()).value("integration_window_ticks",Json(std::to_string(window)));auto size=P1Tick(integration);
        if(model=="sigflow.led@1")channels_.emplace(id,DutyIntegrator(size));
        if(model=="sigflow.seven-segment-mux@1"){for(int digit=0;digit<4;++digit)for(auto segment:{"a","b","c","d","e","f","g","dp"})channels_.emplace(id+".dig"+std::to_string(digit)+"."+segment,DutyIntegrator(size));digit0_edge_[id]={};scan_period_[id]=0;previous_digit0_[id]=false;}
        if(model=="sigflow.dip-switch@1"){unsigned width=c.at("params").at("width");uint32_t initial=c.at("params").value("initial_value",0u);if(initial>=(1u<<width))throw std::runtime_error("E_DIP_VALUE");dips_[id]={width,initial};}
        if(model=="sigflow.uart-peer@1"){Uart u;const auto& p=c.at("params");u.baud=p.at("baud");u.bits=p.at("data_bits");u.parity=p.at("parity");u.stops=p.at("stop_bits");uarts_[id]=std::move(u);}
    }
}
uint64_t Phase1Peripherals::BitTime(const Uart& u,uint64_t units,uint64_t denominator)const{
    auto scaled=static_cast<unsigned __int128>(units)*1000000000000ULL;auto divisor=uint64_t(u.baud)*denominator;auto result=(scaled+divisor/2)/divisor;if(result>std::numeric_limits<uint64_t>::max())throw std::runtime_error("E_TIME_OVERFLOW");return uint64_t(result);
}
void Phase1Peripherals::StartTx(Uart& u,uint64_t tick){
    if(u.queue.empty())return;auto byte=u.queue.front();u.queue.pop_front();u.tx_bits={false};unsigned ones=0;
    for(int i=0;i<u.bits;++i){bool bit=bool(byte&(1u<<i));u.tx_bits.push_back(bit);ones+=bit;}
    if(u.parity!="none")u.tx_bits.push_back(u.parity=="even"?bool(ones&1):!bool(ones&1));for(int i=0;i<u.stops;++i)u.tx_bits.push_back(true);
    u.tx_start=tick;u.tx_index=0;u.tx_high=false;u.tx_due=P1Add(tick,BitTime(u,1));
}
void Phase1Peripherals::Send(const std::string& id,const std::vector<uint8_t>& bytes,uint64_t tick){auto it=uarts_.find(id);if(it==uarts_.end())throw std::runtime_error("E_UART_TARGET");auto& u=it->second;if(bytes.empty() || bytes.size()+u.queue.size()>4096)throw std::runtime_error("E_UART_QUEUE");for(auto byte:bytes)u.queue.push_back(byte);if(!u.tx_due)StartTx(u,tick);}
void Phase1Peripherals::SetDip(const std::string& id,uint32_t value){auto it=dips_.find(id);if(it==dips_.end() || value>=(1u<<it->second.first))throw std::runtime_error("E_DIP_VALUE");it->second.second=value;}
void Phase1Peripherals::Advance(uint64_t tick){
    if(tick<tick_)throw std::runtime_error("E_TIME_ORDER");for(auto& [id,c]:channels_)c.Advance(tick);tick_=tick;
    for(auto& [id,u]:uarts_)if(u.tx_due && *u.tx_due<=tick){
        if(*u.tx_due!=tick)throw std::runtime_error("E_UART_SKIPPED_DEADLINE");++u.tx_index;
        if(u.tx_index==u.tx_bits.size()){
            uint32_t byte=0;for(int i=0;i<u.bits;++i)if(u.tx_bits[i+1])byte|=1u<<i;if(u.transmitted.size()>=4096)throw std::runtime_error("E_UART_EVENT_QUOTA");
            u.transmitted.push_back({{"type","uart.tx"},{"tick",std::to_string(tick)},{"start_tick",std::to_string(u.tx_start)},{"byte",byte},{"sequence",std::to_string(++u.tx_sequence)},{"valid",true},{"framing_error",false}});
            u.tx_due.reset();u.tx_high=true;u.tx_bits.clear();StartTx(u,tick);
        }else{u.tx_high=u.tx_bits.at(u.tx_index);u.tx_due=P1Add(u.tx_start,BitTime(u,u.tx_index+1));}
    }
}
void Phase1Peripherals::AddDrivers(std::map<std::string,std::vector<Driver>>& drivers)const{
    for(const auto& [id,u]:uarts_)drivers[id+".TX"].push_back({"uart.tx",uint8_t(u.tx_high?State::SHI:State::SLO)});
    for(const auto& [id,p]:dips_)for(unsigned bit=0;bit<p.first;++bit)drivers[id+".bit"+std::to_string(bit)].push_back({"dip",uint8_t(p.second&(1u<<bit)?State::SHI:State::SLO)});
}
std::optional<uint64_t> Phase1Peripherals::NextDeadline(uint64_t tick)const{std::optional<uint64_t> next;for(const auto& [id,u]:uarts_)for(auto due:{u.tx_due,u.rx_due})if(due && *due>tick && (!next || *due<*next))next=due;return next;}
void Phase1Peripherals::Observe(Json& board,uint64_t tick){
    if(tick!=tick_)throw std::runtime_error("E_OBSERVATION_POSITION");Json peripherals=Json::object();
    for(auto& [id,led]:board.at("leds").items()){const auto& node=board.at("nodes").at(id+".K");auto& integral=channels_.at(id);integral.Set(led.at("lit"),node.at("valid"));led["integration"]=integral.Snapshot();}
    for(const auto& c:board_->Descriptor().at("components")){std::string id=c.at("id"),model=c.at("model");
        if(model=="sigflow.seven-segment-mux@1"){
            Json digits=Json::array();bool digit0=false;
            for(int digit=0;digit<4;++digit){Json segments=Json::array();const auto& select=board.at("nodes").at(id+".dig"+std::to_string(digit));bool selected=select.at("valid")==true && (select.at("logic")=="1")==(c.at("params").at("digit_active_level").get<int>()==1);if(digit==0)digit0=selected;
                for(auto segment:{"a","b","c","d","e","f","g","dp"}){const auto& node=board.at("nodes").at(id+"."+segment);bool valid=node.at("valid")==true && select.at("valid")==true;
                    bool active=valid && selected && (node.at("logic")=="1")==(c.at("params").at("segment_active_level").get<int>()==1);auto& integral=channels_.at(id+".dig"+std::to_string(digit)+"."+segment);integral.Set(active,valid);auto observation=integral.Snapshot();observation["segment"]=segment;observation["state_code"]=node.at("state_code");observation["digit_state_code"]=select.at("state_code");segments.push_back(std::move(observation));}
                digits.push_back(std::move(segments));
            }
            if(digit0 && !previous_digit0_[id]){if(digit0_edge_[id] && tick>*digit0_edge_[id])scan_period_[id]=tick-*digit0_edge_[id];digit0_edge_[id]=tick;}previous_digit0_[id]=digit0;
            peripherals[id]={{"model",model},{"common",c.at("params").at("common")},{"digits",digits},{"scan_period_ticks",std::to_string(scan_period_[id])}};
        }
    }
    for(auto& [id,u]:uarts_){const auto& node=board.at("nodes").at(id+".RX");bool valid=node.at("valid"),high=node.at("logic")=="1";
        if(!u.rx_due && valid && u.rx_previous && !high){u.rx_start=tick;u.rx_stage=-1;u.rx_byte=0;u.rx_error=false;u.rx_parity_error=false;u.rx_due=P1Add(tick,BitTime(u,1,2));}
        else if(u.rx_due && *u.rx_due==tick){
            if(!valid)u.rx_error=true;
            if(u.rx_stage==-1){if(high || !valid){u.rx_error=true;u.rx_stage=u.bits+(u.parity!="none")+u.stops;}else u.rx_stage=0;}
            else{if(u.rx_stage<u.bits){if(high && valid)u.rx_byte|=1u<<u.rx_stage;}
                else if(u.parity!="none" && u.rx_stage==u.bits){unsigned ones=0;for(int i=0;i<u.bits;++i)ones+=bool(u.rx_byte&(1u<<i));bool expected=u.parity=="even"?bool(ones&1):!bool(ones&1);if(!valid || high!=expected)u.rx_parity_error=true;}
                else if(!valid || !high)u.rx_error=true;++u.rx_stage;}
            if(u.rx_stage>=u.bits+(u.parity!="none")+u.stops){if(u.received.size()>=4096)throw std::runtime_error("E_UART_EVENT_QUOTA");u.received.push_back({{"type",u.rx_error?"uart.framing_error":u.rx_parity_error?"uart.parity_error":"uart.rx"},{"tick",std::to_string(tick)},{"start_tick",std::to_string(u.rx_start)},{"byte",u.rx_byte},{"sequence",std::to_string(++u.rx_sequence)},{"valid",!u.rx_error && !u.rx_parity_error},{"framing_error",u.rx_error},{"parity_error",u.rx_parity_error},{"state_code",node.at("state_code")}});u.rx_due.reset();}
            else u.rx_due=P1Add(u.rx_start,BitTime(u,uint64_t(3+2*u.rx_stage),2));
        }
        u.rx_previous=valid?high:u.rx_previous;
    }
    for(const auto& [id,p]:dips_)peripherals[id]={{"model","sigflow.dip-switch@1"},{"width",p.first},{"value",p.second}};
    board["peripherals"]=peripherals;board["uart"]=Snapshot().at("uart");
}
Json Phase1Peripherals::Snapshot()const{
    Json integrals=Json::object(),uart=Json::object(),dips=Json::object();for(const auto& [name,c]:channels_)integrals[name]=c.Snapshot();
    for(const auto& [id,u]:uarts_)uart[id]={{"received",u.received},{"transmitted",u.transmitted},{"tx_busy",bool(u.tx_due)},{"queued_bytes",u.queue.size()},{"tx_due",u.tx_due?Json(std::to_string(*u.tx_due)):Json(nullptr)},{"rx_due",u.rx_due?Json(std::to_string(*u.rx_due)):Json(nullptr)},{"rx_stage",u.rx_stage},{"rx_byte",u.rx_byte},{"rx_start",std::to_string(u.rx_start)},{"rx_error",u.rx_error},{"rx_parity_error",u.rx_parity_error},{"tx_index",std::to_string(u.tx_index)},{"tx_high",u.tx_high}};
    for(const auto& [id,p]:dips_)dips[id]=p.second;return {{"tick",std::to_string(tick_)},{"integrators",integrals},{"uart",uart},{"dips",dips}};
}
}
