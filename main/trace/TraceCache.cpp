#include "TraceCache.h"

#include <utility>

namespace sigflow {
namespace trace {

CachingTraceSource::CachingTraceSource(std::unique_ptr<TraceSource> inner,
                                       std::size_t maxBytes)
    : m_inner(std::move(inner)),
      m_budget(maxBytes)
{
}

bool CachingTraceSource::Open(const std::string& path, std::string& error)
{
    m_cache.clear();
    m_budget.Clear();
    return m_inner->Open(path, error);
}

const std::vector<SignalInfo>& CachingTraceSource::Signals() const
{
    return m_inner->Signals();
}

const SignalInfo* CachingTraceSource::SignalById(int id) const
{
    return m_inner->SignalById(id);
}

TraceTimeRange CachingTraceSource::TimeRange() const
{
    return m_inner->TimeRange();
}

std::string CachingTraceSource::Timescale() const
{
    return m_inner->Timescale();
}

const std::string& CachingTraceSource::Path() const
{
    return m_inner->Path();
}

bool CachingTraceSource::HasSidecar() const
{
    return m_inner->HasSidecar();
}

std::string CachingTraceSource::QueryKey(int signalId, TimeValue t0, TimeValue t1)
{
    return std::to_string(signalId) + ":" + std::to_string(t0) + ":" + std::to_string(t1);
}

std::string CachingTraceSource::ValueKey(int signalId, TimeValue t)
{
    return std::to_string(signalId) + "@" + std::to_string(t);
}

std::size_t CachingTraceSource::EstimateQueryBytes(const std::vector<Transition>& transitions)
{
    std::size_t total = 40 * transitions.size();
    for (const Transition& transition : transitions) {
        total += transition.value.size();
    }
    return total;
}

bool CachingTraceSource::Query(const SignalInfo& signal, TimeValue t0, TimeValue t1,
                               std::vector<Transition>& out, std::string& error)
{
    const std::string key = QueryKey(signal.id, t0, t1);
    const auto hit = m_cache.find(key);
    if (hit != m_cache.end() && hit->second.isQuery) {
        m_budget.Touch(key);
        out = hit->second.transitions;
        return true;
    }

    std::vector<Transition> fetched;
    if (!m_inner->Query(signal, t0, t1, fetched, error)) return false;

    CacheEntry entry;
    entry.isQuery = true;
    entry.transitions = fetched;
    const std::vector<std::string> evicted = m_budget.Insert(key, EstimateQueryBytes(fetched));
    for (const std::string& victim : evicted) m_cache.erase(victim);
    m_cache[key] = std::move(entry);
    out = std::move(fetched);
    return true;
}

bool CachingTraceSource::ValueAt(const SignalInfo& signal, TimeValue t,
                                 std::string& value, std::string& error)
{
    const std::string key = ValueKey(signal.id, t);
    const auto hit = m_cache.find(key);
    if (hit != m_cache.end() && !hit->second.isQuery) {
        m_budget.Touch(key);
        value = hit->second.value;
        return true;
    }

    std::string fetched;
    if (!m_inner->ValueAt(signal, t, fetched, error)) return false;

    CacheEntry entry;
    entry.isQuery = false;
    entry.value = fetched;
    const std::vector<std::string> evicted = m_budget.Insert(key, fetched.size() + 32);
    for (const std::string& victim : evicted) m_cache.erase(victim);
    m_cache[key] = std::move(entry);
    value = std::move(fetched);
    return true;
}

} // namespace trace
} // namespace sigflow
