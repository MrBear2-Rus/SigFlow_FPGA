#pragma once
#include <eda/api/Types.h>
#include <eda/api/live_trace.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
// Frozen SFVB v1 binary sync-frame contract (planning book §4.4).
// 64-byte header, explicit little-endian serialization, typed records;
// no C++ struct memcpy, no compiler padding assumptions, no wx dependency.
namespace eda::virtual_board {
inline constexpr char SfvbMagic[4]={'S','F','V','B'};
inline constexpr uint16_t SfvbVersion=1;
inline constexpr uint32_t SfvbHeaderBytes=64;
inline constexpr size_t SfvbMaxPayloadBytes=64u*1024*1024;
inline constexpr const char* SfvbFrameSchema="sigflow.virtual-frame.v1";
enum class SfvbKind : uint16_t { Snapshot=1,Delta=2,Trace=3,Display=4 };
const char* SfvbKindName(uint16_t);
struct SfvbAck {
    std::string command_id,status;      // applied/pending/superseded/blocked/rejected/observed/cancelled
    uint64_t applied_tick=0;uint32_t applied_delta=0;uint64_t applied_order=0;
};struct SfvbPadChange {
    uint32_t numeric_id=0;std::string target;uint8_t state_code=0;
    char logic=0;bool valid=false,contention=false;
};
struct SfvbComponentState { uint32_t numeric_id=0;std::string component;Json state; };
struct SfvbTraceEvent {
    uint32_t numeric_id=0;uint64_t tick=0,event_seq=0;uint32_t delta=0;
    char value=0;uint8_t state_code=0;bool valid=true,contention=false,has_state_code=false;
};
struct SfvbTraceCursor {
    uint64_t revision=0,committed_through_tick=0,committed_event_seq=0;std::string cursor;
};
struct SfvbEvent { std::string type;Json detail; };
struct SfvbFrame {
    uint16_t kind=uint16_t(SfvbKind::Delta);
    uint32_t flags=0,run_handle=0,generation=1;
    uint64_t seq=0,base_seq=0,tick=0;uint32_t delta=0;
    std::string schema=SfvbFrameSchema,run_id,state_encoding,state,reason,error;
    uint64_t tick_ps=1;
    // Full snapshots assign stable run-scoped numeric ids for signals/nodes/components.
    std::vector<std::tuple<uint32_t,uint8_t,std::string>> dictionary;
    std::vector<SfvbAck> acks;
    std::vector<SfvbPadChange> pad_changes;
    std::vector<SfvbComponentState> components;
    SfvbTraceCursor trace;
    std::vector<SfvbTraceEvent> trace_events;
    std::vector<SfvbEvent> events;
    Json diagnostics=Json::array();
};
// The JSON delta frame is the reference projection; binary decode must be
// losslessly equivalent to it field by field.
Json SfvbFrameToJson(const SfvbFrame&);
SfvbFrame SfvbFrameFromJson(const Json&);
std::vector<uint8_t> SfvbEncodeFrame(const SfvbFrame&);
// Throws std::runtime_error("E_SFVB_*") on any structural violation.
SfvbFrame SfvbDecodeFrame(const uint8_t* data,size_t size);
// Stateful receiver: sequence continuity, baseline and generation checks.
class SfvbStreamDecoder {
public:
    enum class Status { Ok,NeedFullSnapshot,StaleGeneration,FormatError };
    struct Result { Status status=SfvbStreamDecoder::Status::Ok;SfvbFrame frame;std::string error; };
    Result Feed(const uint8_t* data,size_t size);
    bool HasBaseline()const{return has_baseline_;}
    uint64_t LastSeq()const{return last_seq_;}
    const std::map<uint32_t,std::string>& Dictionary()const{return dictionary_;}
private:
    bool has_baseline_=false;uint64_t last_seq_=0;uint32_t run_handle_=0,generation_=0;
    std::map<uint32_t,std::string> dictionary_;
};
// Converts chronological session snapshots (already student-projected in blind
// mode) plus their committed trace transitions into snapshot+delta frames.
class SfvbFrameBuilder {
public:
    SfvbFrameBuilder(uint32_t run_handle,uint32_t generation,const std::vector<LiveSignal>& signals);
    SfvbFrame Feed(const Json& sessionSnapshot,const std::vector<LiveTransition>& traceEvents={});
    uint64_t Frames()const{return seq_;}
    const std::map<std::string,uint32_t>& Dictionary()const{return ids_;}
private:
    uint32_t run_handle_=0,generation_=1,next_id_=0;
    uint64_t seq_=0,previous_tick_=0;
    std::string run_id_;
    std::map<std::string,uint32_t> ids_;
    std::set<uint32_t> signal_ids_;
    std::map<std::string,std::tuple<std::string,int,bool,bool>> previous_nodes_;
    std::map<std::string,Json> previous_components_;
};
}
