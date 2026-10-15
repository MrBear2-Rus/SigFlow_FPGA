#pragma once

#include "WaveProtocolDecoder.h"

#include <cstddef>
#include <list>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace sigflow::wave {

// Bounded host-side cache for decoded transactions. The caller supplies a
// capture identity and a configuration hash; changing either naturally yields
// a new entry. It is intentionally independent of TraceViewPanel so a
// background worker can populate it before the UI is updated.
struct ProtocolDecodeCacheKey {
    std::string captureId;
    std::string configurationHash;

    std::string StableId() const { return captureId + "\n" + configurationHash; }
};

class ProtocolDecodeCache {
public:
    explicit ProtocolDecodeCache(std::size_t maxBytes = 16u * 1024u * 1024u)
        : maxBytes_(maxBytes) {}

    bool Find(const ProtocolDecodeCacheKey& key, std::vector<ProtocolTransaction>& out);
    void Store(const ProtocolDecodeCacheKey& key, const std::vector<ProtocolTransaction>& events);
    void InvalidateCapture(const std::string& captureId);
    void Clear();

    std::size_t EntryCount() const { return entries_.size(); }
    std::size_t UsedBytes() const { return usedBytes_; }
    std::size_t MaxBytes() const { return maxBytes_; }

private:
    struct Entry {
        std::vector<ProtocolTransaction> events;
        std::size_t bytes = 0;
        std::list<std::string>::iterator order;
    };

    static std::size_t EstimateBytes(const std::string& key,
                                     const std::vector<ProtocolTransaction>& events);
    void Erase(const std::string& stableId);
    void Trim();

    std::size_t maxBytes_;
    std::size_t usedBytes_ = 0;
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> entries_;
};

inline std::size_t ProtocolDecodeCache::EstimateBytes(
    const std::string& key, const std::vector<ProtocolTransaction>& events)
{
    std::size_t bytes = key.size();
    for (const auto& event : events) {
        bytes += event.protocol.size() + event.detail.size() + sizeof(event);
        bytes += event.bytes.size() * sizeof(std::uint8_t);
        bytes += event.words.size() * sizeof(std::uint64_t);
    }
    return bytes;
}

inline void ProtocolDecodeCache::Erase(const std::string& stableId)
{
    const auto found = entries_.find(stableId);
    if (found == entries_.end()) return;
    usedBytes_ -= found->second.bytes;
    order_.erase(found->second.order);
    entries_.erase(found);
}

inline void ProtocolDecodeCache::Trim()
{
    while (usedBytes_ > maxBytes_ && !order_.empty()) Erase(order_.front());
}

inline bool ProtocolDecodeCache::Find(const ProtocolDecodeCacheKey& key,
                                      std::vector<ProtocolTransaction>& out)
{
    const std::string stableId = key.StableId();
    const auto found = entries_.find(stableId);
    if (found == entries_.end()) return false;
    order_.splice(order_.end(), order_, found->second.order);
    out = found->second.events;
    return true;
}

inline void ProtocolDecodeCache::Store(const ProtocolDecodeCacheKey& key,
                                       const std::vector<ProtocolTransaction>& events)
{
    const std::string stableId = key.StableId();
    Erase(stableId);
    order_.push_back(stableId);
    Entry entry;
    entry.events = events;
    entry.bytes = EstimateBytes(stableId, events);
    entry.order = std::prev(order_.end());
    usedBytes_ += entry.bytes;
    entries_.emplace(stableId, std::move(entry));
    Trim();
}

inline void ProtocolDecodeCache::InvalidateCapture(const std::string& captureId)
{
    const std::string prefix = captureId + "\n";
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            const std::string id = it->first;
            ++it;
            Erase(id);
        } else {
            ++it;
        }
    }
}

inline void ProtocolDecodeCache::Clear()
{
    entries_.clear();
    order_.clear();
    usedBytes_ = 0;
}

} // namespace sigflow::wave
