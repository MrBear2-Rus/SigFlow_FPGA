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
    ~JobRunHandle() { Join(); }

    bool IsFinished() const { return m_finished.load(); }

    void Join()
    {
        if (!m_thread.joinable()) return;
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
    std::atomic<bool> m_finished{false};
};

inline std::shared_ptr<JobRunHandle> RunJobAsync(
    const ToolJob& job,
    const std::function<bool(const ToolJob&, const JobExecutionOptions&, JobReport&,
                             wxString&)>& execute,
    const JobExecutionOptions& options,
    const std::function<void(const JobRunOutcome&)>& onDone)
{
    const std::shared_ptr<JobRunHandle> handle = std::make_shared<JobRunHandle>();
    handle->m_thread = std::thread([handle, job, execute, options, onDone]() {
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
        handle->m_finished.store(true);
        if (wxTheApp == nullptr) return;
        wxTheApp->CallAfter([outcome, onDone]() {
            if (onDone) onDone(outcome);
        });
    });
    return handle;
}

// 把后台线程产生的进程输出合并后一次性刷到终端控件（避免逐块 CallAfter 洪水）。
class JobOutputPump : public std::enable_shared_from_this<JobOutputPump> {
public:
    explicit JobOutputPump(wxWeakRef<TerminalCtrl> terminal) : m_terminal(terminal) {}

    void Push(const wxString& chunk, bool isError)
    {
        (void)isError;
        if (chunk.IsEmpty()) return;
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending += chunk;
            if (!m_scheduled) {
                m_scheduled = true;
                schedule = true;
            }
        }
        if (!schedule || wxTheApp == nullptr) return;
        const std::shared_ptr<JobOutputPump> self = shared_from_this();
        wxTheApp->CallAfter([self]() { self->Flush(); });
    }

    void Flush()
    {
        wxString text;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            text = m_pending;
            m_pending.clear();
            m_scheduled = false;
        }
        TerminalCtrl* terminal = m_terminal.get();
        if (terminal == nullptr) return;
        if (!text.IsEmpty()) terminal->AppendProcessOutput(text);
    }

private:
    wxWeakRef<TerminalCtrl> m_terminal;
    std::mutex m_mutex;
    wxString m_pending;
    bool m_scheduled = false;
};
