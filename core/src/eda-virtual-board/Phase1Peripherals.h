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
    // Phase 3 matrix keypad: replace the closed key set of one component.
    void SetKeys(const std::string& component,const Json& keys);
    void AddDrivers(std::map<std::string,std::vector<Driver>>&)const;
    // Anti-ghost diode mode couples active-low rows into columns as drivers;
    // diode-less mode returns bridges so pressed keys merge row/column nets
    // and ghost keys emerge from the real equipotential resolution.
    Json KeypadOverlay()const;
    void ApplyFaults(const Json&);
    // Phase 4 peripheral fault events (i2c.nak_injected, spi.mode_fault_applied,
    // crc.tampered) drain into the deterministic fault ledger.
    Json DrainEvents(){Json events=fault_events_;fault_events_=Json::array();return events;}
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
        // Phase 4 crc_tamper: upper-layer packet CRC mutation, bit-level
        // frames stay fully legal.
        Json crc_tamper=Json();uint64_t crc_frames_seen=0;
    };
    struct Keypad {
        unsigned rows=0,cols=0;bool diodes=false;
        std::set<std::pair<unsigned,unsigned>> closed;
    };
    // Open-drain I2C sensor: reacts to resolved SCL/SDA levels and drives SDA
    // (SLO when pulling low, FLT when released) for the next evaluation.
    struct I2c {
        int address=0;std::vector<uint8_t> registers;
        enum class Phase : uint8_t { Idle,Addr,AckAddr,Write,AckData,Read,MasterAck };
        Phase phase=Phase::Idle;
        bool prev_valid=false,prev_scl=false,prev_sda=false;
        unsigned bits=0;uint8_t shift=0;bool read_mode=false,ack_slot_drive=false,drive_low=false;
        unsigned reg_pointer=0,ack_count=0,nak_count=0,transactions=0,read_bit=0,write_stage=0;
        uint8_t read_shift=0,last_read_byte=0;bool master_ack=false;
        Json nak=Json();std::string bus_state="idle";
    };
    // SPI monochrome OLED (SSD1306 teaching subset): CS/SCK/MOSI/DC/RES pins
    // drive a command state machine and a page-addressed VRAM.
    struct Oled {
        int width=0,height=0,cpol=0,cpha=0;
        bool prev_valid=false,prev_sck=false,prev_cs=true,prev_res=true;
        bool display_on=false,inverted=false;
        unsigned page=0,column=0;uint64_t write_count=0;
        std::vector<uint8_t> vram;
        uint8_t shift=0;unsigned bits=0,frame_bits=0;bool byte_is_data=false;
        Json mode_fault=Json();
    };
    const BoardTopology* board_;
    Json faults_=Json::array();
    uint64_t tick_=0;
    std::map<std::string,DutyIntegrator> channels_;
    std::map<std::string,Uart> uarts_;
    std::map<std::string,std::pair<unsigned,uint32_t>> dips_;
    std::map<std::string,Keypad> keypads_;
    std::map<std::string,I2c> i2cs_;
    std::map<std::string,Oled> oleds_;
    std::map<std::string,std::map<std::string,std::string>> keypad_nets_,i2c_nets_,oled_nets_;
    Json pending_events_=Json::array(),fault_events_=Json::array();
    std::map<std::string,std::optional<uint64_t>> digit0_edge_;
    std::map<std::string,uint64_t> scan_period_;
    std::map<std::string,bool> previous_digit0_;
    uint64_t BitTime(const Uart&,uint64_t units,uint64_t denominator=1,bool transmit=false)const;
    void StartTx(Uart&,uint64_t);
};
}
