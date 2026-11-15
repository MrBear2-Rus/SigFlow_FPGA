#pragma once
#include "TraceSource.h"
#include "eda-virtual-board/LiveTraceStore.h"
#include <mutex>

namespace sigflow::trace {
class LiveTraceSource final:public TraceSource {
public:
    explicit LiveTraceSource(std::shared_ptr<const eda::virtual_board::LiveTraceStore>);
    LiveTraceSource(std::shared_ptr<const eda::virtual_board::LiveTraceStore>,eda::virtual_board::ReadView);
    void RefreshReadView();
    void BindReadView(eda::virtual_board::ReadView);
    eda::virtual_board::ReadView CurrentReadView() const;
    bool Open(const std::string&,std::string&) override;
    const std::vector<SignalInfo>& Signals()const override{return signals_;}
    const SignalInfo* SignalById(int)const override;
    TraceTimeRange TimeRange()const override;
    std::string Timescale()const override{return "1ps";}
    TraceTimeDomain TimeDomain()const override{return TraceTimeDomain::Physical;}
    const std::string& Path()const override{return path_;}
    bool HasSidecar()const override{return false;}
    std::string CacheIdentity()const override;
    std::shared_ptr<TraceSource> PinReadSource()override;
    bool Query(const SignalInfo&,TimeValue,TimeValue,std::vector<Transition>&,std::string&)override;
    bool ValueAt(const SignalInfo&,TimeValue,std::string&,std::string&)override;
    eda::virtual_board::TracePage QueryExtended(int,uint64_t,uint64_t,
        std::optional<eda::virtual_board::TraceCursor> cursor = {},size_t limit=10000)const;
private:
    std::shared_ptr<const eda::virtual_board::LiveTraceStore> store_;
    mutable std::mutex mutex_;
    eda::virtual_board::ReadView view_;
    std::vector<SignalInfo> signals_;
    std::string path_;
};
}
