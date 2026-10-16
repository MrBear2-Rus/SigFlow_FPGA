// Coupled service regression. The caller owns the directory; this test never deletes files.
#include "eda-agent-gateway/WaveformService.h"
#include "eda-platform/Platform.h"
#include "eda-platform/Sha256.h"
#include <fstream>
#include <iostream>
#include <map>

class BackendFixture final : public eda::IWaveformBackend {
public:
    std::vector<eda::WaveSignal> signals{{0,"a","TOP","TOP.a","!",1},
                                         {1,"b","TOP","TOP.b","\"",1}};
    std::map<int,std::vector<eda::WaveTransition>> events{{0,{{0,"0"},{1,"1"},{2,"0"}}},{1,{}}};
    bool Open(const std::string&,std::string&) override { return true; }
    const std::vector<eda::WaveSignal>& Signals() const override { return signals; }
    std::string Timescale() const override { return "1ns"; }
    eda::WaveTimeRange TimeRange() const override { return {0,2,true}; }
    bool Query(int id,std::uint64_t,std::uint64_t,std::vector<eda::WaveTransition>& out,std::string&) override {
        out=events[id]; return true;
    }
    bool ValueAt(int,std::uint64_t,std::string& value,std::string&) override { value="0"; return true; }
};

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const auto root=eda::platform::PathFromUtf8(argv[1]);
    if(!root.is_absolute() || !std::filesystem::is_regular_file(root/"test-owned"))return 2;
    const auto path=root/"wave.vcd";
    std::ofstream(path)<<"owned fixture";
    auto backend=std::make_shared<BackendFixture>();
    eda::agent::WaveformService service;
    service.SetBackendFactory([backend]{return backend;});
    service.Register({"wave1","p1","r1","job1",path,eda::platform::Sha256FileHex(path),"eda.wave.vcd.v1"});
    int failures=0, checks=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::cout<<"FAIL "<<name<<"\n";}};
    auto query=[&](std::vector<int> ids,std::size_t transitions,std::size_t bytes,std::size_t signals=16,std::string cursor="") {
        eda::agent::WaveQueryRange request; request.signalIds=std::move(ids);request.startTick=0;request.endTick=2;
        eda::agent::WaveQueryPage page;
        auto status=service.QueryRange("wave1",request,cursor,signals,transitions,bytes,page);
        return std::make_pair(status,page);
    };
    using C=eda::agent::WaveQueryCompleteness;
    using S=eda::agent::WaveformService::Status;
    auto tooMany=query({0},2,10000);
    check(tooMany.first==S::kOk && tooMany.second.completeness==C::kPartial &&
          tooMany.second.signals.empty() && tooMany.second.omittedSignals==1 &&
          !tooMany.second.hasMore && tooMany.second.nextCursor.empty(),"last signal event truncation is partial without a false progress cursor");
    auto tooBig=query({0},10000,32);
    check(tooBig.second.completeness==C::kPartial && tooBig.second.signals.empty() &&
          tooBig.second.transitionCount==0,"last signal byte truncation never returns a cut signal as exact");
    auto boundary=query({0,1},3,10000);
    check(boundary.second.completeness==C::kExact && boundary.second.signals.size()==2 &&
          boundary.second.transitionCount==3 && !boundary.second.hasMore,"exact transition boundary can include later empty signals");
    auto first=query({0,1},10000,10000,1);
    check(first.second.completeness==C::kPartial && first.second.signals.size()==1 &&
          first.second.hasMore && first.second.omittedSignals==1,"signal page advances only across complete signals");
    auto second=query({0,1},10000,10000,1,first.second.nextCursor);
    check(second.first==S::kOk && second.second.completeness==C::kExact &&
          second.second.signals.size()==1 && second.second.signals[0].signalId==1,"valid cursor reads remaining whole signal");
    auto forged=query({0,1},10000,10000,1,"q:1:");
    check(forged.first==S::kBadCursor,"empty cursor binding is rejected");
    auto overflow=query({0,1},10000,10000,1,"q:18446744073709551615:0000000000000000");
    check(overflow.first==S::kBadCursor,"out of range cursor is rejected");
    auto changed=query({1,0},10000,10000,1,first.second.nextCursor);
    check(changed.first==S::kBadCursor,"cursor is bound to ordered request signals");
    // Metadata/initial value are part of the payload budget even for no transitions.
    auto initial=query({1},10000,1);
    check(initial.second.completeness==C::kPartial && initial.second.signals.empty(),"empty transition signal still consumes metadata bytes");
    backend->events[0].assign(10000,{18446744073709551615ULL,std::string(170,'1')});
    eda::agent::WaveQueryRange longTicks;longTicks.signalIds={0};longTicks.endTick=18446744073709551615ULL;
    eda::agent::WaveQueryPage longPage;
    service.QueryRange("wave1",longTicks,"",16,10000,2*1024*1024,longPage);
    check(longPage.completeness==C::kPartial && longPage.signals.empty(),"uint64 tick JSON overhead obeys response byte limit");
    std::ofstream(path)<<"changed fixture";
    auto expired=query({0},10000,10000);
    check(expired.first==S::kExpired,"changed artifact hash is rejected before backend use");
    std::cout<<"checks="<<checks<<" failures="<<failures<<"\n";
    return failures?1:0;
}
