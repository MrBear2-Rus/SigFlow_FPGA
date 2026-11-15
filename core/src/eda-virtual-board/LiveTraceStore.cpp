#include "LiveTraceStore.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace eda::virtual_board {
namespace {
using Chunk=std::vector<LiveTransition>;
struct IndexPage {
    std::shared_ptr<const IndexPage> previous;
    std::vector<std::shared_ptr<const Chunk>> chunks;
};
constexpr size_t ChunkOverhead=sizeof(IndexPage)+sizeof(Chunk)+256*sizeof(std::shared_ptr<const Chunk>)+128;
size_t Bytes(const LiveTransition& r){return sizeof(LiveTransition)+r.value.size()+32;}
}
struct TraceSnapshot {
    std::string run_id;
    uint64_t generation=0,revision=0;
    EventPosition tail;
    bool has_tail=false;
    std::shared_ptr<const std::vector<LiveSignal>> signals;
    std::shared_ptr<const IndexPage> index;
};
uint64_t ReadView::Revision()const{return snapshot_?snapshot_->revision:0;}
uint64_t ReadView::Generation()const{return snapshot_?snapshot_->generation:0;}
const std::string& ReadView::RunId()const{if(!snapshot_)throw std::runtime_error("E_TRACE_VIEW_EXPIRED");return snapshot_->run_id;}
const std::vector<LiveSignal>& ReadView::Signals()const{if(!snapshot_)throw std::runtime_error("E_TRACE_VIEW_EXPIRED");return *snapshot_->signals;}
EventPosition ReadView::Tail()const{if(!snapshot_)throw std::runtime_error("E_TRACE_VIEW_EXPIRED");return snapshot_->tail;}
LiveTraceStore::LiveTraceStore(std::string id,uint64_t gen,std::vector<LiveSignal> signals,TraceLimits limits)
    :run_id_(std::move(id)),generation_(gen),limits_(limits){
    if(run_id_.empty() || run_id_.size()>256 || !gen || signals.empty() || !limits.max_records || !limits.max_bytes || !limits.max_chunks || !limits.max_read_views)
        throw std::runtime_error("E_TRACE_CONFIG");
    auto lease_bytes=2*run_id_.size()+256;
    if(limits.max_read_views>limits.max_bytes/lease_bytes || signals.capacity()>limits.max_bytes/sizeof(LiveSignal))throw std::runtime_error("E_TRACE_QUOTA");
    bytes_=limits.max_read_views*lease_bytes+signals.capacity()*sizeof(LiveSignal);
    if(bytes_>limits.max_bytes)throw std::runtime_error("E_TRACE_QUOTA");
    std::map<std::string,bool> names;
    for(const auto& s:signals){
        if(!s.width || s.width>1048576 || s.name.empty() || s.name.size()>4096 || s.kind.size()>64)throw std::runtime_error("E_TRACE_SIGNAL");
        auto extra=s.name.capacity()+s.kind.capacity()+2*s.name.size()+256;
        if(extra>limits.max_bytes-bytes_)throw std::runtime_error("E_TRACE_QUOTA");bytes_+=extra;
        if(!widths_.emplace(s.id,s.width).second || !names.emplace(s.name,true).second)throw std::runtime_error("E_TRACE_SIGNAL");
    }
    signals_=std::make_shared<const std::vector<LiveSignal>>(std::move(signals));
    auto initial=std::make_shared<TraceSnapshot>();initial->run_id=run_id_;initial->generation=gen;initial->signals=signals_;current_=initial;
}
void LiveTraceStore::CheckWriter(){
    if(!writer_)writer_=std::this_thread::get_id();
    if(*writer_!=std::this_thread::get_id())throw std::runtime_error("E_TRACE_WRITER");
    if(sealed_)throw std::runtime_error("E_TRACE_SEALED");
    if(notifying_)throw std::runtime_error("E_TRACE_CALLBACK_REENTRANT");
}
void LiveTraceStore::BindWriterToCurrentThread(){std::lock_guard lock(mutex_);CheckWriter();}
void LiveTraceStore::AppendBatch(std::span<const LiveTransition> batch){
    std::lock_guard lock(mutex_);CheckWriter();
    auto last=pending_.empty()?current_->tail:pending_.back().position;
    bool has=!pending_.empty() || current_->has_tail;size_t extra=0;
    for(const auto& r:batch){
        auto w=widths_.find(r.signal_id);
        if(w==widths_.end() || r.value.size()!=w->second || r.value.find_first_not_of("01xz")!=std::string::npos)
            throw std::runtime_error("E_TRACE_VALUE");
        if(has && (!PositionLess(last,r.position) || r.position.event_seq<=last.event_seq))throw std::runtime_error("E_TRACE_ORDER");
        if(has && r.position.tick==last.tick && r.position.delta<last.delta)throw std::runtime_error("E_TRACE_ORDER");
        last=r.position;has=true;
        if(Bytes(r)>limits_.max_bytes-extra)throw std::runtime_error("E_TRACE_QUOTA");extra+=Bytes(r);
    }
    size_t pending_bytes=0;for(const auto& r:pending_)pending_bytes+=Bytes(r);
    if(batch.size()>limits_.max_records-records_-pending_.size() ||
       pending_bytes>limits_.max_bytes-bytes_ || ChunkOverhead>limits_.max_bytes-bytes_-pending_bytes ||
       extra>limits_.max_bytes-bytes_-pending_bytes-ChunkOverhead || chunks_>=limits_.max_chunks)
        throw std::runtime_error("E_TRACE_QUOTA");
    // Build a new buffer first: allocation failure leaves the staged prefix intact.
    auto next=pending_;next.insert(next.end(),batch.begin(),batch.end());pending_.swap(next);
}
void LiveTraceStore::DiscardPending(){std::lock_guard lock(mutex_);CheckWriter();pending_.clear();}
TraceCommit LiveTraceStore::Commit(std::optional<EventPosition> fence,const std::function<void(const TraceCommit&)>& barrier){
    std::vector<std::function<void(const TraceCommit&)>> callbacks;TraceCommit info;
    {
        std::lock_guard lock(mutex_);CheckWriter();
        if(current_->revision==std::numeric_limits<uint64_t>::max() || chunks_>=limits_.max_chunks)throw std::runtime_error("E_TRACE_QUOTA");
        size_t extra=ChunkOverhead;for(const auto& r:pending_)extra+=Bytes(r);
        if(extra>limits_.max_bytes-bytes_)throw std::runtime_error("E_TRACE_QUOTA");
        auto tail=fence.value_or(pending_.empty()?current_->tail:pending_.back().position);
        if(!pending_.empty() && (PositionLess(tail,pending_.back().position) || tail.event_seq<pending_.back().position.event_seq))throw std::runtime_error("E_TRACE_ORDER");
        if(current_->has_tail && (!pending_.empty() || fence) && !PositionLess(current_->tail,tail))throw std::runtime_error("E_TRACE_ORDER");
        if(fence && current_->has_tail && fence->event_seq<=current_->tail.event_seq)throw std::runtime_error("E_TRACE_ORDER");
        auto next=std::make_shared<TraceSnapshot>(*current_);
        next->revision++;next->tail=tail;next->has_tail=current_->has_tail || !pending_.empty() || bool(fence);
        auto page=std::make_shared<IndexPage>();
        if(current_->index && current_->index->chunks.size()<128)*page=*current_->index;
        else page->previous=current_->index;
        page->chunks.reserve(128);
        auto chunk=std::make_shared<const Chunk>(pending_);page->chunks.push_back(chunk);next->index=page;
        info={next->revision,tail,std::to_string(next->revision)};
        for(const auto& [id,cb]:subscriptions_)callbacks.push_back(cb);
        if(barrier)barrier(info); // no visible snapshot or watermark until success
        records_+=pending_.size();bytes_+=extra;chunks_++;pending_.clear();current_=next;notifying_=true;
    }
    for(const auto& cb:callbacks)try{cb(info);}catch(...){} // notifications cannot roll back a committed prefix
    {std::lock_guard lock(mutex_);notifying_=false;}
    return info;
}
void LiveTraceStore::Seal(){std::lock_guard lock(mutex_);CheckWriter();pending_.clear();sealed_=true;}
ReadView LiveTraceStore::AcquireReadView()const{
    std::lock_guard lock(mutex_);
    leases_.erase(std::remove_if(leases_.begin(),leases_.end(),[](const auto& l){return l.expired();}),leases_.end());
    if(leases_.size()>=limits_.max_read_views)throw std::runtime_error("E_TRACE_VIEW_QUOTA");
    // A separate immutable snapshot allocation is the lease identity. Chunks/pages are shared.
    auto lease=std::make_shared<const TraceSnapshot>(*current_);leases_.push_back(lease);
    ReadView v;v.snapshot_=std::move(lease);v.view_id_=next_view_++;return v;
}
void LiveTraceStore::CheckView(const ReadView& v,uint32_t signal)const{
    if(!v.snapshot_ || v.snapshot_->run_id!=run_id_ || v.snapshot_->generation!=generation_ || v.snapshot_->signals!=signals_)
        throw std::runtime_error("E_TRACE_VIEW_EXPIRED");
    if(!widths_.count(signal))throw std::runtime_error("E_TRACE_SIGNAL");
}
TracePage LiveTraceStore::Query(const ReadView& v,uint32_t signal,uint64_t begin,uint64_t end,std::optional<TraceCursor> cursor,size_t limit)const{
    CheckView(v,signal);if(begin>end || !limit || limit>100000)throw std::runtime_error("E_TRACE_QUERY");
    uint64_t skip=0;
    if(cursor){const auto& c=*cursor;
        if(c.run_id!=run_id_ || c.generation!=generation_ || c.revision!=v.Revision() || c.view_id!=v.view_id_ ||
           c.signal_id!=signal || c.tick_begin!=begin || c.tick_end!=end)throw std::runtime_error("E_TRACE_VIEW_EXPIRED");
        skip=c.offset;
    }
    std::vector<const IndexPage*> pages;for(auto p=v.snapshot_->index;p;p=p->previous)pages.push_back(p.get());
    TracePage result;uint64_t matched=0;
    for(auto page=pages.rbegin();page!=pages.rend();++page)for(const auto& chunk:(*page)->chunks)for(const auto& r:*chunk){
        if(r.signal_id!=signal || r.position.tick<begin || r.position.tick>end)continue;
        if(matched++<skip)continue;
        if(result.records.size()==limit){result.next=TraceCursor{run_id_,generation_,v.Revision(),v.view_id_,skip+result.records.size(),signal,begin,end};return result;}
        result.records.push_back(r);
    }
    if(skip>matched)throw std::runtime_error("E_TRACE_CURSOR");return result;
}
TraceValue LiveTraceStore::ValueAtPosition(const ReadView& v,uint32_t signal,const EventPosition& position)const{
    CheckView(v,signal);
    for(auto page=v.snapshot_->index;page;page=page->previous)
        for(auto c=page->chunks.rbegin();c!=page->chunks.rend();++c)
            for(auto r=(*c)->rbegin();r!=(*c)->rend();++r)
                if(r->signal_id==signal && !PositionLess(position,r->position))
                    return {r->value,r->valid && !r->contention && (!r->state_code || !(*r->state_code&0x20)) && r->value.find_first_not_of("01")==std::string::npos,*r};
    return {std::string(widths_.at(signal),'x'),false,{}};
}
TraceValue LiveTraceStore::ValueAtTick(const ReadView& v,uint32_t signal,uint64_t tick)const{
    return ValueAtPosition(v,signal,{tick,std::numeric_limits<uint32_t>::max(),std::numeric_limits<uint64_t>::max()});
}
uint64_t LiveTraceStore::SubscribeCommit(std::function<void(const TraceCommit&)> callback)const{
    if(!callback)throw std::runtime_error("E_TRACE_CALLBACK");std::lock_guard lock(mutex_);auto id=next_subscription_++;subscriptions_[id]=std::move(callback);return id;
}
void LiveTraceStore::UnsubscribeCommit(uint64_t id)const{std::lock_guard lock(mutex_);subscriptions_.erase(id);}
TraceCommit LiveTraceStore::Watermark()const{std::lock_guard lock(mutex_);return {current_->revision,current_->tail,std::to_string(current_->revision)};}
}
