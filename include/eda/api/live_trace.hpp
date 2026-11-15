#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eda::virtual_board {
struct EventPosition { uint64_t tick=0; uint32_t delta=0; uint64_t event_seq=0; };
inline bool operator==(const EventPosition& a,const EventPosition& b) {
    return a.tick==b.tick && a.delta==b.delta && a.event_seq==b.event_seq;
}
inline bool PositionLess(const EventPosition& a,const EventPosition& b) {
    return a.tick<b.tick || (a.tick==b.tick && a.event_seq<b.event_seq);
}
struct LiveSignal { uint32_t id=0; std::string name; uint32_t width=1; std::string kind; };
struct LiveTransition {
    EventPosition position;
    uint32_t signal_id=0;
    std::string value;
    std::optional<uint8_t> state_code;
    bool valid=true, contention=false;
};
struct TraceCommit { uint64_t trace_revision=0; EventPosition tail_position; std::string batch_id; };
struct TraceSnapshot;
class ReadView {
public:
    ReadView()=default;
    bool Valid() const { return bool(snapshot_); }
    void Reset() { snapshot_.reset(); }
    uint64_t Revision() const;
    uint64_t Generation() const;
    const std::string& RunId() const;
    const std::vector<LiveSignal>& Signals() const;
    EventPosition Tail() const;
    uint64_t ViewId() const { return view_id_; }
private:
    friend class LiveTraceStore;
    std::shared_ptr<const TraceSnapshot> snapshot_;
    uint64_t view_id_=0;
};
struct TraceCursor {
    std::string run_id;
    uint64_t generation=0, revision=0, view_id=0, offset=0;
    uint32_t signal_id=0;
    uint64_t tick_begin=0,tick_end=0;
};
struct TracePage { std::vector<LiveTransition> records; std::optional<TraceCursor> next; };
struct TraceValue { std::string value="x"; bool known=false; std::optional<LiveTransition> transition; };
struct TraceLimits { size_t max_records=2000000,max_bytes=256*1024*1024,max_chunks=100000,max_read_views=256; };
}
