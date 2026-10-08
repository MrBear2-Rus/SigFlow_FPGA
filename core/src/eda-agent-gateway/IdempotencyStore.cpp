#include "IdempotencyStore.h"

#include "eda-platform/Sha256.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace eda {
namespace agent {
namespace {

// 稳定序列化：对象键排序，数组保序（请求语义敏感）。
void StableDump(const Json& value, std::ostringstream& out) {
    if (value.is_object()) {
        std::vector<std::string> keys;
        keys.reserve(value.size());
        for (auto it = value.begin(); it != value.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        out << '{';
        bool first = true;
        for (const auto& key : keys) {
            if (!first) out << ',';
            first = false;
            out << '"' << key << "\":";
            // value is const. find() avoids const operator[]'s debug assertion and keeps
            // the stable serializer non-mutating even if the JSON implementation changes.
            const auto item = value.find(key);
            if (item == value.end()) {
                throw std::logic_error("JSON object changed during stable serialization");
            }
            StableDump(*item, out);
        }
        out << '}';
    } else if (value.is_array()) {
        out << '[';
        bool first = true;
        for (const auto& item : value) {
            if (!first) out << ',';
            first = false;
            StableDump(item, out);
        }
        out << ']';
    } else if (value.is_string()) {
        out << '"' << value.get<std::string>() << '"';
    } else {
        out << value.dump();
    }
}

} // namespace

std::string IdempotencyStore::HashRequest(const Json& request) {
    std::ostringstream out;
    StableDump(request, out);
    const std::string serialized = out.str();
    return platform::Sha256Hex(serialized.data(), serialized.size());
}

IdempotencyStore::Outcome IdempotencyStore::Begin(const std::string& scope,
                                                  const std::string& key,
                                                  const std::string& requestHash,
                                                  Entry& outEntry) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string fullKey = MakeKey(scope, key);
    const auto it = entries_.find(fullKey);
    if (it == entries_.end()) {
        Entry entry;
        entry.requestHash = requestHash;
        entry.completed = false;
        entries_[fullKey] = entry;
        outEntry = entry;
        return Outcome::Fresh;
    }
    outEntry = it->second;
    if (it->second.requestHash != requestHash) return Outcome::Conflict;
    if (!it->second.completed) return Outcome::InFlight;
    return Outcome::Replay;
}

bool IdempotencyStore::Complete(const std::string& scope, const std::string& key,
                                const std::string& resourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(MakeKey(scope, key));
    if (it == entries_.end()) return false;
    it->second.resourceId = resourceId;
    it->second.completed = true;
    return true;
}

bool IdempotencyStore::Abandon(const std::string& scope, const std::string& key,
                               const std::string& requestHash) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(MakeKey(scope, key));
    if (it == entries_.end() || it->second.completed ||
        it->second.requestHash != requestHash) {
        return false;
    }
    entries_.erase(it);
    return true;
}

bool IdempotencyStore::Lookup(const std::string& scope, const std::string& key,
                              Entry& outEntry) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(MakeKey(scope, key));
    if (it == entries_.end()) return false;
    outEntry = it->second;
    return true;
}

bool IdempotencyStore::RestoreCompleted(const std::string& scope, const std::string& key,
                                        const std::string& requestHash,
                                        const std::string& resourceId) {
    if (scope.empty() || key.empty() || requestHash.empty() || resourceId.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string compound = MakeKey(scope, key);
    const auto existing = entries_.find(compound);
    if (existing != entries_.end()) {
        return existing->second.completed && existing->second.requestHash == requestHash &&
               existing->second.resourceId == resourceId;
    }
    entries_.emplace(compound, Entry{requestHash, resourceId, true});
    return true;
}

} // namespace agent
} // namespace eda
