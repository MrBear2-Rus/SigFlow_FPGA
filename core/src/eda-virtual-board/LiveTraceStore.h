#pragma once
#include <eda/api/live_trace.hpp>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <map>

namespace eda::virtual_board {
// One writer; published chunks and index pages never change. Readers own the
// prefix independently of the store, including after Seal/destruction.
class LiveTraceStore {
public:
    LiveTraceStore(std::string run_id,uint64_t generation,std::vector<LiveSignal>,TraceLimits={});
    void BindWriterToCurrentThread();
    void AppendBatch(std::span<const LiveTransition>);
    void DiscardPending();
    // The barrier runs after all allocation/validation, before publication.
    // DurableJournal flushes its marker here. If it throws, nothing is published.
    TraceCommit Commit(std::optional<EventPosition> fence={},
        const std::function<void(const TraceCommit&)>& before_publish={});
    void Seal();
    ReadView AcquireReadView() const;
    TracePage Query(const ReadView&,uint32_t signal,uint64_t begin,uint64_t end,
                    std::optional<TraceCursor> cursor={},size_t limit=10000) const;
    TraceValue ValueAtPosition(const ReadView&,uint32_t,const EventPosition&) const;
    TraceValue ValueAtTick(const ReadView&,uint32_t,uint64_t) const;
    uint64_t SubscribeCommit(std::function<void(const TraceCommit&)>) const;
    void UnsubscribeCommit(uint64_t) const;
    TraceCommit Watermark() const;
private:
    void CheckWriter();
    void CheckView(const ReadView&,uint32_t) const;
    std::string run_id_;
    uint64_t generation_;
    TraceLimits limits_;
    std::shared_ptr<const std::vector<LiveSignal>> signals_;
    std::map<uint32_t,uint32_t> widths_;
    mutable std::mutex mutex_;
    std::optional<std::thread::id> writer_;
    bool sealed_=false,notifying_=false;
    std::vector<LiveTransition> pending_;
    size_t records_=0,bytes_=0,chunks_=0;
    std::shared_ptr<const TraceSnapshot> current_;
    mutable uint64_t next_view_=1,next_subscription_=1;
    mutable std::vector<std::weak_ptr<const TraceSnapshot>> leases_;
    mutable std::map<uint64_t,std::function<void(const TraceCommit&)>> subscriptions_;
};
}
