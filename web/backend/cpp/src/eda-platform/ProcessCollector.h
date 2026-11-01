#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace eda {
namespace platform {

// 进程输出收集器：按到达时间/序号保序，供 eda-platform 两个后端复用。
class ProcessCollector {
public:
    explicit ProcessCollector(std::uint64_t maxBytes = 0) : maxBytes_(maxBytes) {}

    // 追加一块原始字节；成功时 out 为该块文本，返回 true。
    bool Append(const char* data, std::size_t size, bool isError, std::string& out) {
        if (data == nullptr || size == 0) {
            out.clear();
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t accepted = size;
        if (maxBytes_ != 0) {
            if (acceptedBytes_ >= maxBytes_) {
                truncated_ = true;
                out.clear();
                return false;
            }
            accepted = static_cast<std::size_t>(
                std::min<std::uint64_t>(maxBytes_ - acceptedBytes_, size));
            if (accepted < size) {
                truncated_ = true;
            }
        }
        if (accepted == 0) {
            out.clear();
            return false;
        }
        out.assign(data, accepted);
        acceptedBytes_ += accepted;
        events_.push_back(OutputEvent{
            std::chrono::steady_clock::now(), nextSequence_++, isError, out});
        return true;
    }

    std::string Combined() const { return Merge(false, false); }
    std::string Errors() const { return Merge(true, false); }

    bool Truncated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return truncated_;
    }

private:
    struct OutputEvent {
        std::chrono::steady_clock::time_point timestamp;
        std::uint64_t sequence = 0;
        bool isError = false;
        std::string text;
    };

    std::string Merge(bool onlyErrors, bool /*unused*/) const {
        std::vector<OutputEvent> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = events_;
        }
        std::stable_sort(copy.begin(), copy.end(),
                         [](const OutputEvent& a, const OutputEvent& b) {
                             if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                             return a.sequence < b.sequence;
                         });
        std::string out;
        for (const auto& event : copy) {
            if (!onlyErrors || event.isError) {
                out += event.text;
            }
        }
        return out;
    }

    mutable std::mutex mutex_;
    std::uint64_t maxBytes_ = 0;
    std::uint64_t acceptedBytes_ = 0;
    std::uint64_t nextSequence_ = 0;
    bool truncated_ = false;
    std::vector<OutputEvent> events_;
};

} // namespace platform
} // namespace eda
