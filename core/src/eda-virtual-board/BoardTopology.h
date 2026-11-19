#pragma once
#include <eda/api/Types.h>
#include <eda/api/electrical_profile.hpp>
#include <eda/api/logic_state.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace eda::virtual_board {
struct ConnectionEdge { std::string id, net, endpoint; };
struct EdgeFault {
    std::string id, edge;
    uint64_t start=0;
    std::optional<uint64_t> end;
    bool Active(uint64_t tick) const { return tick>=start && (!end || tick<*end); }
};
using FaultSet=std::map<std::string,EdgeFault>;
Json FaultsJson(const FaultSet&);
FaultSet ParseFaults(const Json&);
class BoardTopology {
public:
    static std::shared_ptr<const BoardTopology> Load(const std::filesystem::path&,const ElectricalProfile&);
    BoardTopology(Json,const ElectricalProfile&);
    const Json& Descriptor() const { return descriptor_; }
    const std::string& Fingerprint() const { return fingerprint_; }
    const std::set<std::string>& Nodes() const { return nodes_; }
    const std::vector<ConnectionEdge>& Edges() const { return edges_; }
    int ClockPad() const { return clock_pad_; }
    uint64_t Frequency() const { return frequency_; }
    const std::string& ClockEndpoint() const { return clock_endpoint_; }
    std::string ControlEndpoint(int pad) const;
    bool HasEdge(const std::string&) const;
    void ValidateFaults(const FaultSet&) const;
    // Drivers belong to individual nodes. An open edge splits connected components.
    Json Resolve(const std::map<std::string,std::vector<Driver>>&,const FaultSet&,uint64_t tick,const Json& overlay=Json::object()) const;
    bool UsesState13()const;
private:
    Json descriptor_;
    std::string fingerprint_,clock_endpoint_;
    uint64_t frequency_=0;
    int clock_pad_=0;
    std::set<std::string> nodes_;
    std::vector<ConnectionEdge> edges_;
    std::map<int,std::string> controls_;
    std::map<std::string,int> leds_;
    std::map<std::string,std::vector<Driver>> fixed_;
};
}
