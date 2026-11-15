#include "LiveTraceSource.h"
#include <limits>
#include <stdexcept>
namespace sigflow::trace {
LiveTraceSource::LiveTraceSource(std::shared_ptr<const eda::virtual_board::LiveTraceStore> store)
    :LiveTraceSource(store,store->AcquireReadView()){}
LiveTraceSource::LiveTraceSource(std::shared_ptr<const eda::virtual_board::LiveTraceStore> store,eda::virtual_board::ReadView view)
    :store_(std::move(store)),view_(std::move(view)){
    if(!view_.Valid())throw std::runtime_error("E_TRACE_VIEW_EXPIRED");
    path_="live://"+view_.RunId()+"/"+std::to_string(view_.Generation());
    for(const auto& s:view_.Signals()){
        if(s.id>uint32_t(std::numeric_limits<int>::max()))throw std::runtime_error("E_TRACE_SIGNAL");
        SignalInfo v;v.id=int(s.id);v.name=s.name;v.fullName=s.name;v.width=s.width;
        v.kind=s.width==1?SignalKind::Scalar:SignalKind::Vector;signals_.push_back(v);
    }
}
void LiveTraceSource::RefreshReadView(){BindReadView(store_->AcquireReadView());}
void LiveTraceSource::BindReadView(eda::virtual_board::ReadView view){
    std::lock_guard lock(mutex_);
    if(!view.Valid() || view.RunId()!=view_.RunId() || view.Generation()!=view_.Generation())throw std::runtime_error("E_TRACE_VIEW_EXPIRED");
    view_=std::move(view);
}
eda::virtual_board::ReadView LiveTraceSource::CurrentReadView()const{std::lock_guard lock(mutex_);return view_;}
bool LiveTraceSource::Open(const std::string& path,std::string& error){if(path==path_){error.clear();return true;}error="E_LIVE_TRACE_ALREADY_BOUND";return false;}
const SignalInfo* LiveTraceSource::SignalById(int id)const{for(const auto& s:signals_)if(s.id==id)return &s;return nullptr;}
TraceTimeRange LiveTraceSource::TimeRange()const{auto v=CurrentReadView();return {0,v.Tail().tick,v.Revision()!=0};}
std::string LiveTraceSource::CacheIdentity()const{
    auto v=CurrentReadView();return v.RunId()+":"+std::to_string(v.Generation())+":"+std::to_string(v.Revision());
}
std::shared_ptr<TraceSource> LiveTraceSource::PinReadSource(){return std::make_shared<LiveTraceSource>(store_,CurrentReadView());}
eda::virtual_board::TracePage LiveTraceSource::QueryExtended(int id,uint64_t begin,uint64_t end,
    std::optional<eda::virtual_board::TraceCursor> cursor,size_t limit)const{
    return store_->Query(CurrentReadView(),uint32_t(id),begin,end,cursor,limit);
}
bool LiveTraceSource::Query(const SignalInfo& signal,TimeValue begin,TimeValue end,std::vector<Transition>& out,std::string& error){
    try{
        auto view=CurrentReadView();std::optional<eda::virtual_board::TraceCursor> cursor;std::vector<Transition> result;
        do{
            auto page=store_->Query(view,uint32_t(signal.id),begin,end,cursor);
            for(const auto& r:page.records)result.push_back({r.position.tick,r.value});
            cursor=page.next;
        }while(cursor);
        // Preserve repeated timestamps. No delta is converted to fake ps.
        out=std::move(result);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool LiveTraceSource::ValueAt(const SignalInfo& signal,TimeValue tick,std::string& value,std::string& error){
    try{value=store_->ValueAtTick(CurrentReadView(),uint32_t(signal.id),tick).value;error.clear();return true;}
    catch(const std::exception& e){error=e.what();return false;}
}
}
