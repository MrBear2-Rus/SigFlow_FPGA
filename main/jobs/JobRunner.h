#pragma once

#include "ToolJobs.h"

#include <wx/app.h>

#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

// 后台执行一次 Job，完成后经 wxTheApp->CallAfter 回到 UI 线程（F-13：UI 线程零阻塞）。
struct JobRunOutcome {
    bool success = false;
    ToolJob job;
    JobReport report;
    wxString message;
};

class JobRunHandle {
public:
    JobRunHandle() : m_finished(std::make_shared<std::atomic<bool>>(false)) {}
    ~JobRunHandle() { Join(); }

    bool IsFinished() const { return m_finished->load(); }

    void Join()
    {
        if (!m_thread.joinable()) return;
        // 绝不能 join 自己：若 handle 的最后一个 shared_ptr 在 worker 线程上析构，
        // join() 会抛 system_error，而析构函数是 noexcept → std::terminate。
        // （RunJobAsync 已经不再把 handle 捕获进线程闭包，这里是第二道保险。）
        if (m_thread.get_id() == std::this_thread::get_id()) return;
        m_thread.join();
    }

private:
    friend std::shared_ptr<JobRunHandle> RunJobAsync(
        const ToolJob&,
        const std::function<bool(const ToolJob&, const JobExecutionOptions&, JobReport&,
                                 wxString&)>&,
        const JobExecutionOptions&,
        const std::function<void(const JobRunOutcome&)>&);

    std::thread m_thread;
    // 用 shared_ptr 而不是裸成员：线程闭包只捕获这个标志，不捕获 handle 本身，
    // 从而彻底避免"worker 线程持有 handle 最后一份引用 → 在自己线程上析构"的自连接。
    std::shared_ptr<std::atomic<bool>> m_finished;
};

inline std::shared_ptr<JobRunHandle> RunJobAsync(
    const ToolJob& job,
    const std::function<bool(const ToolJob&, const JobExecutionOptions&, JobReport&,
                             wxString&)>& execute,
    const JobExecutionOptions& options,
    const std::function<void(const JobRunOutcome&)>& onDone)
{
    const std::shared_ptr<JobRunHandle> handle = std::make_shared<JobRunHandle>();
    // 只捕获完成标志（共享所有权），**不捕获 handle**。
    const std::shared_ptr<std::atomic<bool>> finished = handle->m_finished;
    handle->m_thread = std::thread([finished, job, execute, options, onDone]() {
        JobRunOutcome outcome;
        outcome.job = job;
        try {
            outcome.success = execute(job, options, outcome.report, outcome.message);
        } catch (const std::exception& exception) {
            outcome.success = false;
            outcome.message = wxString::FromUTF8(exception.what());
        } catch (...) {
            outcome.success = false;
            outcome.message = "Unhandled exception in Job execution.";
        }
        finished->store(true);
        if (wxTheApp == nullptr) return;
        wxTheApp->CallAfter([outcome, onDone]() {
            if (onDone) onDone(outcome);
        });
    });
    return handle;
}
