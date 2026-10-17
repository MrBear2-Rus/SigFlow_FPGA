#include "WaveformService.h"

#include "eda-platform/Sha256.h"
#include <eda/api/Types.h>

#include <algorithm>
#include <limits>

namespace eda {
namespace agent {

WaveformService::WaveformService() = default;

void WaveformService::SetBackendFactory(WaveformBackendFactory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    factory_ = std::move(factory);
}

void WaveformService::Register(const WaveArtifactRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(record.artifactId);
    if (it != cache_.end() && it->second.sha256 != record.sha256) {
        cache_.erase(it);
    }
    records_[record.artifactId] = record;
}

void WaveformService::RemoveProject(const std::string& projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = records_.begin(); it != records_.end();) {
        if (it->second.projectId == projectId) {
            cache_.erase(it->first);
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
}

bool WaveformService::Lookup(const std::string& artifactId, WaveArtifactRecord& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = records_.find(artifactId);
    if (it == records_.end()) return false;
    out = it->second;
    return true;
}

std::string WaveformService::EncodeCursor(std::size_t offset) {
    return "o:" + std::to_string(offset);
}

bool WaveformService::DecodeCursor(const std::string& cursor, std::size_t& offset) {
    if (cursor.empty()) {
        offset = 0;
        return true;
    }
    if (cursor.rfind("o:", 0) != 0) return false;
    try {
        const unsigned long long v = std::stoull(cursor.substr(2));
        offset = static_cast<std::size_t>(v);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

std::shared_ptr<IWaveformBackend> WaveformService::OpenLocked(const WaveArtifactRecord& record,
                                                              std::string& error) {
    const auto cached = cache_.find(record.artifactId);
    if (cached != cache_.end() && cached->second.sha256 == record.sha256 &&
        cached->second.backend) {
        return cached->second.backend;
    }
    if (!factory_) {
        error = "no waveform backend factory configured";
        return nullptr;
    }
    auto backend = factory_();
    if (!backend) {
        error = "waveform backend factory returned null";
        return nullptr;
    }
    if (!backend->Open(record.path.string(), error)) {
        return nullptr;
    }
    cache_[record.artifactId] = Cache{backend, record.sha256};
    return backend;
}

WaveformService::Status WaveformService::ListSignals(const std::string& artifactId,
                                                     const std::string& cursor,
                                                     std::size_t limit,
                                                     WaveSignalPage& out) {
    std::size_t offset = 0;
    if (!DecodeCursor(cursor, offset)) return Status::kBadCursor;

    WaveArtifactRecord record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(artifactId);
        if (it == records_.end()) return Status::kNotFound;
        record = it->second;

        // artifact 文件消失 → 410；hash 变化 → 视为过期（需重新登记）。
        if (!std::filesystem::is_regular_file(record.path)) return Status::kExpired;
        if (!record.sha256.empty()) {
            const std::string current = platform::Sha256FileHex(record.path);
            if (current.empty() || current != record.sha256) return Status::kExpired;
        }

        std::string error;
        auto backend = OpenLocked(record, error);
        if (!backend) return Status::kUnavailable;

        const auto& all = backend->Signals();
        out.total = all.size();
        out.timescale = backend->Timescale();
        out.timeRange = backend->TimeRange();
        if (offset > out.total) offset = out.total;
        if (limit == 0) limit = all.size() == 0 ? 1 : all.size();
        const std::size_t begin = offset;
        const std::size_t end = std::min(out.total, begin + limit);
        out.signals.assign(all.begin() + static_cast<std::ptrdiff_t>(begin),
                           all.begin() + static_cast<std::ptrdiff_t>(end));
        out.hasMore = end < out.total;
        out.nextCursor = out.hasMore ? EncodeCursor(end) : std::string();
    }
    return Status::kOk;
}

WaveformService::Status WaveformService::QueryRange(const std::string& artifactId,
                                                    const WaveQueryRange& request,
                                                    const std::string& cursor,
                                                    std::size_t maxSignals,
                                                    std::size_t maxTransitions,
                                                    std::size_t maxBytes,
                                                    WaveQueryPage& out) {
    out = WaveQueryPage{};
    // cursor: "q:<offset>:<queryhash>"；空表示从头。
    std::size_t offset = 0;
    std::string queryHash;
    if (!cursor.empty()) {
        if (cursor.rfind("q:", 0) != 0) return Status::kBadCursor;
        const std::size_t sep = cursor.find(':', 2);
        if (sep == std::string::npos) return Status::kBadCursor;
        const auto digits = cursor.substr(2, sep - 2);
        if (digits.empty() || digits.size() > 20 ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return Status::kBadCursor;
        try {
            const auto parsed = std::stoull(digits);
            if (parsed > std::numeric_limits<std::size_t>::max()) return Status::kBadCursor;
            offset = static_cast<std::size_t>(parsed);
        } catch (const std::exception&) {
            return Status::kBadCursor;
        }
        queryHash = cursor.substr(sep + 1);
        if (offset == 0 || queryHash.size() != 16) return Status::kBadCursor;
    }

    // 绑定查询：hash = artifactId|sha256|start|end|信号集合。
    const auto makeQueryHash = [&](const std::string& sha) {
        std::string material = artifactId + "|" + sha + "|" + std::to_string(request.startTick) +
                               "|" + std::to_string(request.endTick) + "|";
        for (int id : request.signalIds) material += std::to_string(id) + ",";
        return eda::platform::Sha256Hex(material.data(), material.size()).substr(0, 16);
    };

    WaveArtifactRecord record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(artifactId);
        if (it == records_.end()) return Status::kNotFound;
        record = it->second;
        if (!std::filesystem::is_regular_file(record.path)) return Status::kExpired;
        if (!record.sha256.empty()) {
            const std::string current = platform::Sha256FileHex(record.path);
            if (current.empty() || current != record.sha256) return Status::kExpired;
        }
        const std::string expectedHash = makeQueryHash(record.sha256);
        if (!cursor.empty() && queryHash != expectedHash) return Status::kBadCursor;

        std::string error;
        auto backend = OpenLocked(record, error);
        if (!backend) return Status::kUnavailable;

        if (request.endTick < request.startTick) return Status::kBadCursor;

        // 解析信号集合：按请求顺序，去重，超过 16 路则显式分页。
        std::vector<int> ids;
        for (int id : request.signalIds) {
            const bool known = backend->Signals().end() !=
                               std::find_if(backend->Signals().begin(), backend->Signals().end(),
                                            [id](const WaveSignal& s) { return s.id == id; });
            if (!known) continue;
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        out.timescale = backend->Timescale();
        if (maxSignals == 0 || (!cursor.empty() && offset >= ids.size())) return Status::kBadCursor;

        std::size_t transitionsUsed = 0;
        // Reserve bounded envelope/range/cursor overhead as well as row data.
        std::size_t bytesUsed = maxBytes == 0 ? 1024 : std::min<std::size_t>(1024,maxBytes);
        std::size_t processed = 0;
        for (std::size_t i = offset; i < ids.size(); ++i) {
            if (out.signals.size() >= maxSignals) {
                break;
            }
            const int id = ids[i];
            WaveSignalQueryResult result;
            result.signalId = id;

            // 区间起点前的初值：取 startTick-1 时刻的值；startTick==0 视为无前值。
            if (request.startTick > 0) {
                std::string value;
                if (backend->ValueAt(id, request.startTick - 1, value, error) && !value.empty()) {
                    result.initialValue = value;
                    result.hasInitial = true;
                }
            }

            std::vector<WaveTransition> transitions;
            if (!backend->Query(id, request.startTick, request.endTick, transitions, error)) {
                return Status::kUnavailable;
            }
            // Only publish a whole signal. A partial signal has no resumable
            // event cursor, so retaining its prefix would lose transitions.
            const std::size_t remainingTransitions = maxTransitions - transitionsUsed;
            if (transitions.size() > remainingTransitions) break;
            const auto budget = maxBytes == 0 ? std::numeric_limits<std::size_t>::max() : maxBytes;
            std::size_t signalBytes = Json{{"signal_id",id},
                {"initial_value",result.hasInitial?Json(result.initialValue):Json(nullptr)},
                {"initial_known",result.hasInitial},{"transitions",Json::array()}}.dump().size()+1;
            bool fits = signalBytes <= budget - bytesUsed;
            for (const auto& t : transitions) {
                const auto cost = Json{{"tick",std::to_string(t.time)},{"value",t.value}}.dump().size()+1;
                if (!fits || cost > budget - bytesUsed - signalBytes) { fits = false; break; }
                signalBytes += cost;
            }
            if (!fits) break;
            result.transitions = std::move(transitions);
            bytesUsed += signalBytes;
            transitionsUsed += result.transitions.size();
            out.signals.push_back(std::move(result));
            ++processed;
        }

        out.transitionCount = transitionsUsed;
        out.omittedSignals = ids.size() - (offset + processed);
        out.completeness = out.omittedSignals ? WaveQueryCompleteness::kPartial : WaveQueryCompleteness::kExact;
        // A zero-progress page asks the caller to narrow its window, never to
        // retry an identical cursor forever. partial is independent of hasMore.
        out.hasMore = out.omittedSignals > 0 && processed > 0;
        if (out.hasMore) {
            out.nextCursor = "q:" + std::to_string(offset + processed) + ":" + expectedHash;
        }
    }
    return Status::kOk;
}

} // namespace agent
} // namespace eda
