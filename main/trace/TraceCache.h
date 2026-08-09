#pragma once

#include "TraceMemoryBudget.h"
#include "TraceSource.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sigflow {
namespace trace {

// 查询结果缓存装饰器（W1-04）：基于 TraceMemoryBudget 的 LRU 字节预算。
class CachingTraceSource : public TraceSource {
public:
    explicit CachingTraceSource(std::unique_ptr<TraceSource> inner,
                                std::size_t maxBytes = 64 * 1024 * 1024);
    ~CachingTraceSource() override = default;

    bool Open(const std::string& path, std::string& error) override;
    const std::vector<SignalInfo>& Signals() const override;
    const SignalInfo* SignalById(int id) const override;
    TraceTimeRange TimeRange() const override;
    std::string Timescale() const override;
    const std::string& Path() const override;
    bool HasSidecar() const override;

    bool Query(const SignalInfo& signal, TimeValue t0, TimeValue t1,
               std::vector<Transition>& out, std::string& error) override;
    bool ValueAt(const SignalInfo& signal, TimeValue t,
                 std::string& value, std::string& error) override;

    std::size_t CacheBytes() const { return m_budget.UsedBytes(); }
    std::size_t CacheEntries() const { return m_cache.size(); }

private:
    struct CacheEntry {
        bool isQuery = false;
        std::vector<Transition> transitions;
        std::string value;
    };

    static std::size_t EstimateQueryBytes(const std::vector<Transition>& transitions);
    static std::string QueryKey(int signalId, TimeValue t0, TimeValue t1);
    static std::string ValueKey(int signalId, TimeValue t);

    std::unique_ptr<TraceSource> m_inner;
    TraceMemoryBudget m_budget;
    std::unordered_map<std::string, CacheEntry> m_cache;
};

} // namespace trace
} // namespace sigflow
