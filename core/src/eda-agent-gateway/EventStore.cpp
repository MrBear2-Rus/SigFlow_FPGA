#include "EventStore.h"

#include "eda-platform/Platform.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

namespace eda {
namespace agent {
namespace {

std::uint64_t SequenceOf(const Json& event) {
    if (event.contains("sequence")) {
        return event["sequence"].is_string() ? std::stoull(event["sequence"].get<std::string>())
                                             : event["sequence"].get<std::uint64_t>();
    }
    return 0;
}

} // namespace

Json EventStore::Append(const std::string& type, const std::string& projectId,
                        const std::string& traceId, const Json& data) {
    Json event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t sequence = nextSequence_.fetch_add(1) + 1;
        event["event_id"] = "ev-" + std::to_string(sequence);
        event["sequence"] = std::to_string(sequence);  // 64 位用十进制字符串（spec §5.1）
        event["project_id"] = projectId;
        event["type"] = type;
        event["trace_id"] = traceId;
        event["timestamp"] = platform::UtcTimestamp();
        event["data"] = data;
        events_.push_back(event);
        receivedAt_.push_back(std::chrono::steady_clock::now());
        TrimLocked();
    }
    cv_.notify_all();  // 唤醒长轮询等待者
    return event;
}

// 调用方必须已持有 mutex_。保留窗口 = 条数上限 ∧ 时长上限（spec：10,000 条或 24 小时）。
void EventStore::TrimLocked() {
    while (events_.size() > maxEvents_) {
        events_.pop_front();
        receivedAt_.pop_front();
    }
    if (maxAgeSeconds_ == 0) return;
    const auto cutoff = std::chrono::steady_clock::now() -
                        std::chrono::seconds(static_cast<std::int64_t>(maxAgeSeconds_));
    while (!events_.empty() && receivedAt_.front() < cutoff) {
        events_.pop_front();
        receivedAt_.pop_front();
    }
}

std::chrono::steady_clock::time_point EventStore::OldestReceivedAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receivedAt_.empty()) return std::chrono::steady_clock::now();
    return receivedAt_.front();
}

Json EventStore::Read(const std::string& projectId, std::uint64_t after, std::size_t limit,
                      bool& cursorExpired) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // 按保留时长裁剪（读路径也要生效，否则静默期后过期游标不会被判为过期）。
    const_cast<EventStore*>(this)->TrimLocked();
    cursorExpired = false;
    if (!events_.empty() && after + 1 < SequenceOf(events_.front())) cursorExpired = true;

    Json list = Json::array();
    const std::size_t cap = limit == 0 ? events_.size() : limit;
    std::uint64_t lastSequence = after;
    for (const auto& event : events_) {
        const std::uint64_t sequence = SequenceOf(event);
        if (sequence <= after) continue;
        if (!projectId.empty() && event.value("project_id", std::string()) != projectId) continue;
        list.push_back(event);
        lastSequence = sequence;
        if (list.size() >= cap) break;
    }

    Json result;
    result["events"] = list;
    result["next_cursor"] = std::to_string(lastSequence);
    result["high_watermark"] = std::to_string(nextSequence_.load());
    return result;
}

bool EventStore::WaitForEvents(const std::string& projectId, std::uint64_t after, int waitMs) {
    const auto hasNew = [this, &projectId, after]() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (SequenceOf(event) <= after) continue;
            if (projectId.empty() || event.value("project_id", std::string()) == projectId) {
                return true;
            }
        }
        return false;
    };
    if (hasNew()) return true;
    if (waitMs <= 0) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(waitMs), [this, &projectId, after]() {
        for (const auto& event : events_) {
            if (SequenceOf(event) <= after) continue;
            if (projectId.empty() || event.value("project_id", std::string()) == projectId) {
                return true;
            }
        }
        return false;
    });
}

bool EventStore::AppendToFile(const std::filesystem::path& path, const Json& event) const {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) return false;
    out << event.dump() << "\n";
    return static_cast<bool>(out);
}

bool EventStore::LoadFromFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string line;
    std::uint64_t maxSequence = 0;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        Json event;
        try {
            event = Json::parse(line);
        } catch (const std::exception&) {
            continue;  // 跳过损坏行，保证可重放其余事件
        }
        const std::uint64_t sequence = SequenceOf(event);
        maxSequence = std::max(maxSequence, sequence);
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
        // 重放事件没有可信的原始接收时刻；按重放顺序赋当前时刻，保留时长从重启后重新计时。
        receivedAt_.push_back(std::chrono::steady_clock::now());
        TrimLocked();
    }
    nextSequence_.store(maxSequence);  // 保证后续分配从已落盘的最大 seq 继续
    return true;
}

std::uint64_t EventStore::HighWatermark() const { return nextSequence_.load(); }

std::uint64_t EventStore::OldestSequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<EventStore*>(this)->TrimLocked();
    if (events_.empty()) return nextSequence_.load() + 1;
    return SequenceOf(events_.front());
}

std::size_t EventStore::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<EventStore*>(this)->TrimLocked();
    return events_.size();
}

} // namespace agent
} // namespace eda
