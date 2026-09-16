#include "TraceQueryService.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace sigflow {
namespace trace {

struct TraceQueryService::Job {
    TraceQueryRequest request;
    std::uint64_t id = 0;
    std::atomic<bool> cancelled{false};
};

namespace {

TimeValue AddSaturated(TimeValue value, TimeValue delta)
{
    const TimeValue maxValue = std::numeric_limits<TimeValue>::max();
    return value > maxValue - delta ? maxValue : value + delta;
}

TimeValue SubSaturated(TimeValue value, TimeValue delta)
{
    return value < delta ? 0 : value - delta;
}

} // namespace

TraceQueryService::TraceQueryService()
{
    worker_ = std::thread(&TraceQueryService::WorkerLoop, this);
}

TraceQueryService::~TraceQueryService()
{
    stopping_.store(true);
    CancelAll();
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::uint64_t TraceQueryService::Submit(TraceQueryRequest request)
{
    const std::uint64_t id = nextJobId_.fetch_add(1);
    auto job = std::make_shared<Job>();
    job->request = std::move(request);
    job->id = id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load()) return 0;
        jobs_[id] = job;
        queue_.push_back(job);
    }
    condition_.notify_one();
    return id;
}

bool TraceQueryService::Cancel(std::uint64_t jobId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return false;
    it->second->cancelled.store(true);
    return true;
}

void TraceQueryService::CancelAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : jobs_) item.second->cancelled.store(true);
}

TraceQueryResult TraceQueryService::Execute(const std::shared_ptr<Job>& job)
{
    TraceQueryResult result;
    result.jobId = job->id;
    if (!job->request.source) {
        result.error = "trace query source is null";
        return result;
    }
    if (job->request.end < job->request.begin) {
        result.error = "trace query range is reversed";
        return result;
    }

    const TimeValue begin = SubSaturated(job->request.begin, job->request.margin);
    const TimeValue end = AddSaturated(job->request.end, job->request.margin);
    const std::size_t total = job->request.signalIds.size();
    result.signals.reserve(total);
    for (std::size_t index = 0; index < total; ++index) {
        if (job->cancelled.load()) {
            result.cancelled = true;
            return result;
        }
        const int signalId = job->request.signalIds[index];
        const SignalInfo* signal = job->request.source->SignalById(signalId);
        if (!signal) {
            result.error = "trace signal not found: " + std::to_string(signalId);
            return result;
        }
        TraceQuerySignalResult signalResult;
        signalResult.signalId = signalId;
        if (!job->request.source->Query(*signal, begin, end,
                                        signalResult.transitions, result.error)) {
            return result;
        }
        result.signals.push_back(std::move(signalResult));
        if (job->request.progress) {
            // 用户回调可能在任意实现里抛异常；这里是 std::thread 的入口路径，
            // 异常一旦逃出线程函数就是 std::terminate（整个进程被终止）。
            // 与 DebugAcquisition::FireProgress 的写法保持一致，就地兜住。
            try {
                job->request.progress(job->id, index + 1, total);
            }
            catch (...) {
            }
        }
    }
    result.success = !job->cancelled.load();
    result.cancelled = !result.success;
    return result;
}

void TraceQueryService::WorkerLoop()
{
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return stopping_.load() || !queue_.empty();
            });
            if (queue_.empty() && stopping_.load()) return;
            if (queue_.empty()) continue;
            job = queue_.front();
            queue_.pop_front();
        }

        TraceQueryResult result = Execute(job);
        if (job->request.completed) {
            // 同 progress：绝不能让回调异常逃出 WorkerLoop 这个线程函数。
            try {
                job->request.completed(result);
            }
            catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.erase(job->id);
        }
    }
}

} // namespace trace
} // namespace sigflow
