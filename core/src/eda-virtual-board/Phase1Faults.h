#pragma once
#include "BoardTopology.h"
#include <optional>
namespace eda::virtual_board {
uint64_t P1Tick(const Json&);
uint64_t P1Add(uint64_t,uint64_t);
// Versioned PCG-XSH-RR 64/32. Seed derivation is SHA256 over UTF-8 domain,
// decimal seed and fault id. All byte-to-u64 conversion is explicitly LE.
class Pcg32 {
public:
    Pcg32(uint64_t seed,const std::string& stream);
    uint32_t Next();
    uint32_t Bounded(uint32_t bound);
    Json Snapshot()const;
private:uint64_t state_=0,increment_=1;
};
class Phase1Faults {
public:
    explicit Phase1Faults(const BoardTopology&);
    static Json EmptyProfile(const BoardTopology&);
    void Load(const Json&,uint64_t tick);
    void Enable(const std::string&,bool,uint64_t);
    void Upsert(const Json& rule,uint64_t tick);
    void InputIntent(const std::string& target,uint8_t state,uint64_t tick,bool userIntent=true);
    void Advance(uint64_t tick);
    std::optional<uint64_t> NextDeadline(uint64_t tick)const;
    std::optional<bool> ClockHold(uint64_t tick)const;
    // Phase 3: active divide-ratio offset on the rational main clock.
    std::optional<uint64_t> ClockDenOffset(uint64_t tick)const;
    void Note(const Json& event){events_.push_back(event);}
    // Phase 4 cdc_sampling_approx: active rule spec for an input target and
    // the deterministic seeded draw of its sampling-delay branch. The draw is
    // recorded in the fault ledger as a functional approximation, never as an
    // analog metastability or MTBF claim.
    Json CdcRule(const std::string& target,uint64_t tick)const;
    std::optional<uint32_t> CdcDraw(const std::string& target,uint64_t tick);
    void Transform(std::map<std::string,std::vector<Driver>>&,FaultSet&,uint64_t)const;
    Json Snapshot(uint64_t)const;
    Json ActiveRules(uint64_t)const;
    const Json& Profile()const{return profile_;}
    const Json& Events()const{return events_;}
    void ClearEvents(){events_=Json::array();}
private:
    struct Rule {
        Json spec;Pcg32 rng{0,""};uint64_t matches=0,activations=0,cooldown_until=0;
        std::optional<uint64_t> start,end;
        std::map<uint64_t,uint8_t> edges;
        std::optional<uint8_t> contact;
        bool observed_active=false;
    };
    Json profile_,events_=Json::array();
    std::map<std::string,Rule> rules_;
    std::map<std::string,uint8_t> intent_;
    const BoardTopology* board_;
    bool Active(const Rule&,uint64_t)const;
    void ValidateConflicts(const std::map<std::string,Rule>&,uint64_t)const;
};
}
