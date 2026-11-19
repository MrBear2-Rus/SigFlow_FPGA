#pragma once
#include "debug/ITransport.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <stdexcept>
#include <vector>
namespace sigflow::virtual_lab {
// Real bit transport into a Verilated overlay. No protocol responses are
// synthesized here: every byte is decoded from the RTL tx wire.
class BitClockTransport final:public debug::ITransport {
public:
    using Cycle=std::function<bool(bool)>;
    BitClockTransport(Cycle cycle,unsigned hz,unsigned baud):cycle_(std::move(cycle)),div_((hz+baud/2)/baud){if(div_<4)throw std::runtime_error("E_UART_CLOCK_RATIO");}
    bool Open()override{open_=true;return true;}void Close()override{open_=false;}bool IsOpen()const override{return open_;}
    bool Write(const uint8_t* data,size_t size,int)override{if(!open_ || size>4096)return false;sent_.emplace_back(data,data+size);for(size_t n=0;n<size;++n){Bit(false);for(int b=0;b<8;++b)Bit(bool(data[n]&(1u<<b)));Bit(true);}return true;}
    size_t Read(uint8_t* out,size_t count,int timeout)override{if(!open_)return 0;size_t budget=std::max(1,timeout)*10000ULL;budget=std::min<size_t>(budget,5000000);while(bytes_.empty() && budget--)Tick(true);size_t n=0;while(n<count && !bytes_.empty()){out[n++]=bytes_.front();bytes_.pop_front();}return n;}
    void DiscardInput()override{bytes_.clear();}
    uint64_t Cycles()const{return cycles_;}uint64_t ReceivedBytes()const{return received_;}
    const std::vector<std::vector<uint8_t>>& SentFrames()const{return sent_;}
private:
    void Bit(bool rx){for(unsigned i=0;i<div_;++i)Tick(rx);}
    void Tick(bool rx){bool tx=cycle_(rx);++cycles_;if(!receiving_){if(previous_ && !tx){receiving_=true;count_=0;bit_=0;byte_=0;}}else{++count_;if(count_==div_+div_/2+bit_*div_){if(bit_<8){if(tx)byte_|=uint8_t(1u<<bit_);++bit_;}else{if(!tx)throw std::runtime_error("E_VIRTUAL_RTL_UART_STOP");bytes_.push_back(byte_);++received_;receiving_=false;}}}previous_=tx;}
    Cycle cycle_;unsigned div_,count_=0,bit_=0;uint8_t byte_=0;bool open_=false,previous_=true,receiving_=false;std::deque<uint8_t> bytes_;uint64_t cycles_=0,received_=0;
    std::vector<std::vector<uint8_t>> sent_;
};
}
