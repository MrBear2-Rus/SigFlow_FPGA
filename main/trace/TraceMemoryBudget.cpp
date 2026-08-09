#include "TraceMemoryBudget.h"

namespace sigflow {
namespace trace {

TraceMemoryBudget::TraceMemoryBudget(std::size_t maxBytes)
    : m_maxBytes(maxBytes)
{
}

void TraceMemoryBudget::SetMaxBytes(std::size_t maxBytes)
{
    m_maxBytes = maxBytes;
    EvictToLimit();
}

bool TraceMemoryBudget::Contains(const std::string& key) const
{
    return m_entries.find(key) != m_entries.end();
}

void TraceMemoryBudget::Touch(const std::string& key)
{
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
}

std::vector<std::string> TraceMemoryBudget::Insert(const std::string& key, std::size_t bytes)
{
    const auto existing = m_entries.find(key);
    if (existing != m_entries.end()) {
        m_usedBytes -= existing->second.bytes;
        m_lru.erase(existing->second.lru);
        m_entries.erase(existing);
    }

    m_lru.push_front(key);
    m_entries[key] = Entry{bytes, m_lru.begin()};
    m_usedBytes += bytes;
    return EvictToLimit();
}

void TraceMemoryBudget::Erase(const std::string& key)
{
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    m_usedBytes -= it->second.bytes;
    m_lru.erase(it->second.lru);
    m_entries.erase(it);
}

void TraceMemoryBudget::Clear()
{
    m_entries.clear();
    m_lru.clear();
    m_usedBytes = 0;
}

std::vector<std::string> TraceMemoryBudget::EvictToLimit()
{
    std::vector<std::string> evicted;
    while (m_usedBytes > m_maxBytes && m_lru.size() > 1) {
        const std::string victim = m_lru.back();
        m_lru.pop_back();
        const auto it = m_entries.find(victim);
        if (it != m_entries.end()) {
            m_usedBytes -= it->second.bytes;
            m_entries.erase(it);
            evicted.push_back(victim);
        }
    }
    return evicted;
}

} // namespace trace
} // namespace sigflow
