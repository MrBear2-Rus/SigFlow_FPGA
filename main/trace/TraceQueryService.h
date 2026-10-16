#pragma once

#include "TraceSource.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sigflow {
namespace trace {

struct TraceQuerySignalResult {
    int signalId = -1;
    std::vector<Transition> transitions;
};

struct TraceQueryResult {
    std::uint64_t jobId = 0;
    bool success = false;
    bool cancelled = false;
    std::string error;
    std::vector<TraceQuerySignalResult> signals;
};

struct TraceQueryRequest {
    std::shared_ptr<TraceSource> source;
    std::vector<int> signalIds;
    TimeValue begin = 0;
    TimeValue end = 0;
    TimeValue margin = 0;
    std::function<void(const TraceQueryResult&)> completed;
    std::function<void(std::uint64_t, std::size_t, std::size_t)> progress;
};

struct TraceQueryServiceStats {
    std::uint64_t submitted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    std::size_t peakQueueDepth = 0;
};

class TraceQueryService {
public:
    explicit TraceQueryService(std::size_t maxQueuedJobs = 8);
    ~TraceQueryService();

    TraceQueryService(const TraceQueryService&) = delete;
    TraceQueryService& operator=(const TraceQueryService&) = delete;

    std::uint64_t Submit(TraceQueryRequest request);
    bool Cancel(std::uint64_t jobId);
    void CancelAll();
    std::size_t QueueDepth() const;
    TraceQueryServiceStats Stats() const;
    void ResetStats();

private:
    struct Job;
    void WorkerLoop();
    static TraceQueryResult Execute(const std::shared_ptr<Job>& job);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> jobs_;
    TraceQueryServiceStats stats_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> nextJobId_{1};
    const std::size_t maxQueuedJobs_;
};

} // namespace trace
} // namespace sigflow
