#pragma once
#include "Phase1Faults.h"
#include <deque>
namespace eda::virtual_board {
class DutyIntegrator {
public:
    explicit DutyIntegrator(uint64_t window=20000000000ULL);
    void Advance(uint64_t tick);
    void Set(bool active,bool valid){active_=active;valid_=valid;}
    Json Snapshot()const;
private:
    struct Interval{uint64_t begin,end;bool active,valid;};
    uint64_t window_,tick_=0,active_ticks_=0,valid_ticks_=0;
    bool active_=false,valid_=false;
    std::deque<Interval> intervals_;
};
class Phase1Peripherals {
public:
    Phase1Peripherals(const BoardTopology&,uint64_t defaultWindow);
    void Advance(uint64_t tick);
    void Observe(Json& board,uint64_t tick);
    void Send(const std::string& uart,const std::vector<uint8_t>& bytes,uint64_t tick);
    void SetDip(const std::string& component,uint32_t value);
    void AddDrivers(std::map<std::string,std::vector<Driver>>&)const;
    void ApplyFaults(const Json&);
    std::optional<uint64_t> NextDeadline(uint64_t tick)const;
    Json Snapshot()const;
private:
    struct Uart {
        int baud=0,bits=8,stops=1;std::string parity;
        int offset_ppm=0;bool bad_start=false,bad_stop=false,parity_flip=false;
        std::deque<uint8_t> queue;std::vector<bool> tx_bits;
        uint64_t tx_start=0,tx_index=0;bool tx_high=true;
        std::optional<uint64_t> tx_due,rx_due;
        uint64_t rx_start=0;int rx_stage=-1;uint32_t rx_byte=0;bool rx_error=false,rx_parity_error=false,rx_previous=true;
        Json received=Json::array(),transmitted=Json::array();uint64_t rx_sequence=0,tx_sequence=0;
    };
    const BoardTopology* board_;
    Json faults_=Json::array();
    uint64_t tick_=0;
    std::map<std::string,DutyIntegrator> channels_;
    std::map<std::string,Uart> uarts_;
    std::map<std::string,std::pair<unsigned,uint32_t>> dips_;
    std::map<std::string,std::optional<uint64_t>> digit0_edge_;
    std::map<std::string,uint64_t> scan_period_;
    std::map<std::string,bool> previous_digit0_;
    uint64_t BitTime(const Uart&,uint64_t units,uint64_t denominator=1,bool transmit=false)const;
    void StartTx(Uart&,uint64_t);
};
}
