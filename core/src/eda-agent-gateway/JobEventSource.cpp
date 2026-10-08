#include "JobEventSource.h"

#include "ReportNormalizer.h"   // JobStateName：与报告归一化共用同一套 9 态命名

#include <utility>

namespace eda {
namespace agent {
namespace {

bool IsTerminalState(JobState state) {
    return state == JobState::Succeeded || state == JobState::Failed ||
           state == JobState::Cancelled || state == JobState::TimedOut;
}

} // namespace

void JobEventSource::Track(const std::string& jobId, const std::string& projectId,
                           const std::string& capability, const std::string& snapshotId,
                           const std::string& revision) {
    if (jobId.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Binding& binding = tracked_[jobId];
    binding.projectId = projectId;
    binding.capability = capability;
    binding.snapshotId = snapshotId;
    binding.revision = revision;
    // 保留已有 lastState/finishedEmitted：重复 Track 不应导致事件重放。
}

void JobEventSource::Forget(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    tracked_.erase(jobId);
}

void JobEventSource::ForgetProject(const std::string& projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        it = (it->second.projectId == projectId) ? tracked_.erase(it) : std::next(it);
    }
}

std::size_t JobEventSource::TrackedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracked_.size();
}

std::vector<JobEventSource::Emitted> JobEventSource::Poll(IJobService* service) {
    std::vector<Emitted> emitted;
    if (service == nullptr) return emitted;

    // 在锁内先取一份待查询清单，避免持锁调用外部服务（服务可能回调/阻塞）。
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ids.reserve(tracked_.size());
        for (const auto& entry : tracked_) ids.push_back(entry.first);
    }

    for (const std::string& jobId : ids) {
        const auto record = service->get(jobId);
        if (!record.has_value()) continue;   // 服务已不持有：跳过，不猜终态
        const std::string currentState = JobStateName(record->state);

        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = tracked_.find(jobId);
        if (it == tracked_.end()) continue;  // Poll 期间被 Forget
        Binding& binding = it->second;
        if (binding.lastState == currentState) continue;  // 去重：同状态不重复投递

        Emitted event;
        event.jobId = jobId;
        event.projectId = binding.projectId;
        event.state = currentState;
        event.exitCode = record->exitCode;
        event.capability = binding.capability;
        event.snapshotId = binding.snapshotId;
        event.revision = binding.revision;
        event.type = "job/state-changed";

        binding.lastState = currentState;
        emitted.push_back(event);

        // 终态：额外投递一次 job/finished（每个 Job 至多一次）。
        if (IsTerminalState(record->state) && !binding.finishedEmitted) {
            binding.finishedEmitted = true;
            Emitted finished = event;
            finished.type = "job/finished";
            emitted.push_back(std::move(finished));
        }
    }
    return emitted;
}

} // namespace agent
} // namespace eda
