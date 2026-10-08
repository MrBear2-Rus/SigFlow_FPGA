#pragma once

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace eda {
namespace agent {

// SF-02：Job 生命周期事件源。
//
// Gateway 只负责在**自己的提交路径**上发事件；外部（宿主线程/Job worker）产生的状态变化
// 需要有人订阅后转成规范事件。该类承担这件事，且完全 wx 无关：
//   - 宿主定期或在收到通知时调用 Poll(service)，传入 IJobService*；
//   - 对每个由本 Gateway 登记归属的 Job，比较其当前状态与上次投递的状态；
//   - 仅在**状态真正变化**时投递一个 job/state-changed 事件；
//   - 终态（Succeeded/Failed/Cancelled/TimedOut）额外投递一次 job/finished；
//   - 同一 (job_id, state) 不重复投递（防抖/去重），因此轮询幂等。
//
// 线程安全：Poll 可被宿主线程调用；内部状态有互斥保护。
class JobEventSource {
public:
    struct Emitted {
        std::string type;      // job/state-changed | job/finished
        std::string jobId;
        std::string projectId;
        std::string state;
        int exitCode = 0;
        std::string capability;
        std::string snapshotId;
        std::string revision;
    };

    // 由 Gateway 提交路径登记归属与绑定信息（与 jobBindings 同源）。
    void Track(const std::string& jobId, const std::string& projectId,
               const std::string& capability, const std::string& snapshotId,
               const std::string& revision);
    // 工程关闭/归属清理。
    void Forget(const std::string& jobId);
    void ForgetProject(const std::string& projectId);
    std::size_t TrackedCount() const;

    // 用当前 Job 状态生成新事件。已投递过的 (job_id,state) 不会重复生成。
    // 未知 Job（服务已不再持有该记录）不会被当作终态误报，只是跳过。
    // 传非 const 指针：IJobService::get 非 const 成员。
    std::vector<Emitted> Poll(IJobService* service);

private:
    struct Binding {
        std::string projectId;
        std::string capability;
        std::string snapshotId;
        std::string revision;
        std::string lastState;   // 已投递的最后状态；空表示尚未投递
        bool finishedEmitted = false;
    };

    mutable std::mutex mutex_;
    std::map<std::string, Binding> tracked_;
};

} // namespace agent
} // namespace eda
