#pragma once

#include <eda/api/Types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>

namespace eda {
namespace agent {

// SF-02：事件存储（进程内、有界）。
// 至少一次投递语义：消费者按 event_id 去重；状态事实以资源查询为准。
// 保留窗口：最多 maxEvents 条且不超过 maxAge 时长（两者任一超出即裁剪）。
// 游标过期（早于最旧保留事件）时返回 false，调用方回 410 CURSOR_EXPIRED。
class EventStore {
public:
    static constexpr std::uint64_t kDefaultMaxAgeSeconds = 24ull * 60 * 60;  // SF-02：24 小时

    EventStore(std::size_t maxEvents = 10000,
               std::uint64_t maxAgeSeconds = kDefaultMaxAgeSeconds)
        : maxEvents_(maxEvents == 0 ? 1 : maxEvents), maxAgeSeconds_(maxAgeSeconds) {}

    // 追加一个事件；返回其 event_id 与单调 sequence（实例流内）。
    Json Append(const std::string& type, const std::string& projectId,
                const std::string& traceId, const Json& data);

    // 读取 after 之后的事件（不含 after）；返回快照 + next_cursor(最新 seq) + high_watermark。
    // cursorExpired 为 true 表示 after 早于最旧保留事件（应回 410）。
    Json Read(const std::string& projectId, std::uint64_t after, std::size_t limit,
              bool& cursorExpired) const;

    // SF-02：长轮询——等待"该 project 出现晚于 after 的事件"或超时（毫秒）。返回是否有新事件。
    bool WaitForEvents(const std::string& projectId, std::uint64_t after, int waitMs);

    // SF-02：把事件流以 JSONL 追加落盘（重启可重放）。失败返回 false（不抛）。
    bool LoadFromFile(const std::filesystem::path& path);
    bool AppendToFile(const std::filesystem::path& path, const Json& event) const;

    std::uint64_t HighWatermark() const;
    std::uint64_t OldestSequence() const;
    std::size_t Size() const;
    // 最旧保留事件的接收时刻（无事件时为 now）。用于按时间裁剪。
    std::chrono::steady_clock::time_point OldestReceivedAt() const;

private:
    // 调用方必须已持有 mutex_：按条数上限与保留时长裁剪队首。
    void TrimLocked();

    mutable std::mutex mutex_;
    std::condition_variable cv_;      // 新事件到达时唤醒长轮询
    std::size_t maxEvents_;
    std::uint64_t maxAgeSeconds_;
    std::atomic<std::uint64_t> nextSequence_{0};  // 下一个将分配的 sequence（>=1）
    std::deque<Json> events_;         // 每项含 event_id/sequence/project_id/type/...
    std::deque<std::chrono::steady_clock::time_point> receivedAt_;  // 与 events_ 一一对应
};

} // namespace agent
} // namespace eda
