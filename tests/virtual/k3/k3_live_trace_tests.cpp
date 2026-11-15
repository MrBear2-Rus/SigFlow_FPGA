#include "eda-virtual-board/LiveTraceStore.h"
#include "eda-platform/Platform.h"
#include "LiveTraceSource.h"
#include "TraceCache.h"
#include "WaveformBackendTraceSource.h"
#include "VcdWaveformBackend.h"
#include <eda/api/Types.h>
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <limits>
namespace vb=eda::virtual_board;using eda::Json;namespace fs=std::filesystem;
namespace {
std::atomic<int> checks{0};
void Check(bool yes,const std::string& message){++checks;if(!yes)throw std::runtime_error(message);}
template<class F>void Error(F call,const std::string& code){bool caught=false;try{call();}catch(const std::exception& e){caught=std::string(e.what()).find(code)!=std::string::npos;}Check(caught,code);}
std::shared_ptr<vb::LiveTraceStore> Store(const std::string& id="test",vb::TraceLimits limits={}){
    return std::make_shared<vb::LiveTraceStore>(id,1,std::vector<vb::LiveSignal>{{1,"led_pad",1,"pad"},{2,"vector",4,"port"}},limits);
}
void Cases(const fs::path& root){
    std::ifstream f(root/"tests/virtual/k3/fixtures/live-trace-cases.json");Json fixture;f>>fixture;
    auto store=Store(fixture.at("run_id"));auto live=std::make_unique<sigflow::trace::LiveTraceSource>(store);auto* live_ptr=live.get();
    sigflow::trace::CachingTraceSource cached(std::move(live));auto signal=*cached.SignalById(1);
    Check(cached.TimeDomain()==sigflow::trace::TraceTimeDomain::Physical && cached.Timescale()=="1ps","cache preserves physical live time domain");
    std::vector<vb::ReadView> retained;size_t next=0;std::atomic<int> notified{0};
    std::atomic<bool> callback_failed=false;
    auto subscription=store->SubscribeCommit([&](const vb::TraceCommit& c){try{
        if(c.trace_revision!=uint64_t(++notified))callback_failed=true;
        auto v=store->AcquireReadView();if(v.Revision()!=c.trace_revision)callback_failed=true;
    }catch(...){callback_failed=true;}});
    for(const auto& expected:fixture.at("views")){
        auto tail=std::stoull(expected.at("tail_event_seq").get<std::string>());std::vector<vb::LiveTransition> batch;
        while(next<fixture.at("records").size()){
            const auto& source=fixture.at("records")[next];auto seq=std::stoull(source.at("event_seq").get<std::string>());if(seq>tail)break;
            batch.push_back({{std::stoull(source.at("tick").get<std::string>()),source.at("delta"),seq},1,source.at("value"),{},true,false});++next;
        }
        store->AppendBatch(batch);auto commit=store->Commit();Check(commit.trace_revision==std::stoull(expected.at("revision").get<std::string>()),"fixture revision");
        auto view=store->AcquireReadView();retained.push_back(view);auto page=store->Query(view,1,100,100);Json seqs=Json::array();
        for(const auto& r:page.records)seqs.push_back(std::to_string(r.position.event_seq));Check(seqs==expected.at("expected_seqs"),"all same-tick events in order");
        Check(store->ValueAtTick(view,1,100).value==expected.at("value_at_tick_100").get<std::string>(),"fixture ValueAtTick");
        live_ptr->RefreshReadView();std::vector<sigflow::trace::Transition> projected;std::string error,value;
        Check(cached.Query(signal,100,100,projected,error) && projected.size()==seqs.size(),"revision-aware query cache");
        Check(cached.ValueAt(signal,100,value,error) && value==expected.at("value_at_tick_100").get<std::string>(),"revision-aware value cache");
        Check(cached.Query(signal,100,100,projected,error),"same revision cache hit");
        for(const auto& t:projected)Check(t.time==100,"delta is never fake ps");
    }
    auto stats=cached.Stats();Check(stats.queryMisses==3 && stats.queryHits==3 && stats.valueMisses==3,"cache identity includes revision");
    Check(notified==3 && !callback_failed,"exactly one watermark notification per successful commit");
    Check(store->ValueAtTick(retained.at(0),1,100).value=="0" && store->Query(retained.at(0),1,100,100).records.size()==1,"old read view immutable");
    Check(store->ValueAtPosition(retained.back(),1,{100,1,11}).value=="1","precise position excludes later same-tick events");
    auto paged=store->Query(retained.at(1),1,100,100,{},1);Check(bool(paged.next),"cursor generated");
    Error([&]{store->Query(retained.back(),1,100,100,paged.next,1);},"E_TRACE_VIEW_EXPIRED");
    auto old=retained.at(1);Error([&]{auto empty=old;empty.Reset();store->Query(empty,1,100,100);},"E_TRACE_VIEW_EXPIRED");
    auto before=store->Watermark();std::vector<vb::LiveTransition> bad={{{100,3,14},1,"0"},{{100,4,15},1,"00"}};
    Error([&]{store->AppendBatch(bad);},"E_TRACE_VALUE");Check(store->Watermark().trace_revision==before.trace_revision,"invalid batch publishes nothing");
    Error([&]{store->AppendBatch(std::vector<vb::LiveTransition>{{{101,0,1},1,"0"}});},"E_TRACE_ORDER");
    std::vector<vb::LiveTransition> valid={{{101,0,14},1,"0"}};store->AppendBatch(valid);
    Error([&]{store->Commit({},[](const auto&){throw std::runtime_error("marker-failure");});},"marker-failure");
    Check(store->Watermark().trace_revision==3,"failed marker barrier publishes nothing");store->DiscardPending();
    bool wrong_writer=false;std::thread intruder([&]{try{store->AppendBatch(valid);}catch(const std::exception& e){wrong_writer=std::string(e.what())=="E_TRACE_WRITER";}});intruder.join();Check(wrong_writer,"single-writer enforcement");
    store->UnsubscribeCommit(subscription);store->Seal();
    Check(store->Query(old,1,100,100).records.size()==3,"Stop preserves reader ownership");
    Error([&]{store->AppendBatch(valid);},"E_TRACE_SEALED");
    vb::TraceLimits limits;limits.max_records=2;limits.max_read_views=2;auto small=Store("limited",limits);
    Error([&]{small->AppendBatch(std::vector<vb::LiveTransition>{{{1,0,1},1,"0"},{{1,1,2},1,"1"},{{1,2,3},1,"0"}});},"E_TRACE_QUOTA");
    auto v1=small->AcquireReadView(),v2=small->AcquireReadView();Error([&]{small->AcquireReadView();},"E_TRACE_VIEW_QUOTA");v1.Reset();Check(small->AcquireReadView().Valid(),"released view lease reclaimed");
    auto vector_store=Store("vector-values");vector_store->AppendBatch(std::vector<vb::LiveTransition>{{{1,0,1},2,"0101"}});vector_store->Commit();
    Check(vector_store->ValueAtTick(vector_store->AcquireReadView(),2,1).value=="0101","packed vector values preserved");
    std::cout<<"PASS fixture revisions 1/2/3, immutable views, exact positions, cursor expiry, quotas, cache and atomic rejection\n";
}
void Concurrent(){
    auto store=Store("concurrency");std::atomic<bool> done=false,failed=false;std::mutex errors_mutex;std::string errors;
    auto failure=[&](const std::exception& e){failed=true;std::lock_guard lock(errors_mutex);errors=e.what();};
    std::promise<void> ready;auto start=ready.get_future().share();std::atomic<int> notifications=0,reader_passes=0;std::atomic<uint64_t> max_seen=0;
    store->SubscribeCommit([&](const auto&){++notifications;});
    constexpr int batches=1000;
    std::thread writer([&]{try{
        store->BindWriterToCurrentThread();ready.set_value();
        for(int i=0;i<batches;++i){uint64_t seq=uint64_t(i)*3+1,tick=100+i;
            std::vector<vb::LiveTransition> valid={{{tick,0,seq},1,"0"},{{tick,1,seq+1},1,"1"},{{tick,2,seq+2},1,"0"}};
            auto illegal=valid;illegal.back().signal_id=999;Error([&]{store->AppendBatch(illegal);},"E_TRACE_VALUE");
            store->AppendBatch(valid);store->Commit();if(i%10==0)std::this_thread::yield();
        }store->Seal();
    }catch(const std::exception& e){failure(e);}done=true;});
    std::vector<std::thread> readers;
    for(int id=0;id<4;++id)readers.emplace_back([&]{try{
        start.wait();int after_stop=0;
        do{
            auto view=store->AcquireReadView();auto revision=view.Revision();std::optional<vb::TraceCursor> cursor;uint64_t expected_seq=1;
            do{
                auto page=store->Query(view,1,0,std::numeric_limits<uint64_t>::max(),cursor,37);
                for(const auto& r:page.records){Check(r.position.event_seq==expected_seq++,"no gap/duplicate/half batch in pinned page traversal");}
                cursor=page.next;
            }while(cursor);
            Check(expected_seq-1==revision*3,"reader sees complete committed prefix");
            if(revision){Check(store->ValueAtTick(view,1,view.Tail().tick).value=="0","concurrent tail lookup");max_seen.store(std::max(max_seen.load(),revision));}
            ++reader_passes;if(done)++after_stop;
        }while(!done || after_stop<3);
    }catch(const std::exception& e){failure(e);}});
    writer.join();for(auto& reader:readers)reader.join();
    Check(!failed,"concurrency: "+errors);Check(notifications==batches && max_seen==batches && reader_passes>=12,"all writers/readers completed and read after Stop");
    std::cout<<"PASS concurrency: 1 writer, 4 readers, 1000 atomic batches, 3000 transitions; reader passes="<<reader_passes<<"\n";
}
void Offline(const fs::path& work){
    auto file=work/"offline.vcd";std::ofstream(file)<<"$timescale 1ns $end\n$scope module TOP $end\n$var wire 1 ! clk $end\n$upscope $end\n$enddefinitions $end\n$dumpvars\n0!\n$end\n#10\n1!\n#20\n0!\n";
    auto source=std::make_unique<sigflow::trace::WaveformBackendTraceSource>(std::make_shared<eda::wave::VcdWaveformBackend>());
    sigflow::trace::CachingTraceSource cached(std::move(source));std::string error,value;Check(cached.Open(eda::platform::PathToUtf8(file),error),"offline VCD open");
    auto signal=cached.Signals().at(0);std::vector<sigflow::trace::Transition> out;
    for(int i=0;i<2;++i){Check(cached.Query(signal,0,20,out,error) && out.size()==3,"offline VCD queries");Check(cached.ValueAt(signal,15,value,error) && value=="1","offline VCD ValueAt");}
    Check(cached.Stats().queryHits==1 && cached.Stats().valueHits==1,"offline cache hit behavior retained");
    Check(cached.Open(eda::platform::PathToUtf8(file),error) && cached.CacheEntries()==0,"offline reopen clears cache");
    auto tiny_source=std::make_unique<sigflow::trace::WaveformBackendTraceSource>(std::make_shared<eda::wave::VcdWaveformBackend>());
    sigflow::trace::CachingTraceSource tiny(std::move(tiny_source),1);Check(tiny.Open(eda::platform::PathToUtf8(file),error),"small-budget offline source opens");
    Check(tiny.Query(tiny.Signals().at(0),0,20,out,error) && tiny.CacheEntries()==0 && tiny.CacheBytes()==0,"oversized query cannot bypass cache memory budget");
    std::cout<<"PASS existing offline VCD query/value cache semantics\n";
}
}
int main(int argc,char** argv){try{if(argc!=3)return 2;auto root=eda::platform::PathFromUtf8(argv[1]),work=eda::platform::PathFromUtf8(argv[2]);fs::create_directories(work);Cases(root);Concurrent();Offline(work);std::cout<<"PASS "<<checks<<" LiveTrace checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
