#pragma once
#include "VirtualSession.h"
#include "BoardTopology.h"
#include <deque>
namespace eda::virtual_board {
class VirtualInstruments {
public:
    explicit VirtualInstruments(const SessionContract& c):c_(c){}
    Json Command(const Json&,const Json& frame);
    void Observe(const Json& frame);
    Json Evidence()const{return evidence_;}
    Json Capture()const;
    Json Current(const Json& frame)const;
    static Json Compare(const Json& expected,const Json& actual);
private:
    const SessionContract& c_;
    std::map<std::string,std::string> probes_;
    Json evidence_=Json::array(),config_,samples_=Json::array();
    std::deque<Json> pre_;
    bool armed_=false,triggered_=false,clock_high_=false;
    uint64_t edges_=0,decimated_=0,previous_=0,hits_=0,post_=0,trigger_index_=0;
    Json Measure(const std::string&,const std::string&,const Json&)const;
};
}
