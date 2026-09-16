#pragma once

// JobOutputPump：把后台线程产生的进程输出合并后一次性刷到终端控件。
//
// 单独放在这里（而不是 jobs/JobRunner.h）的原因：它依赖 GUI 类型 TerminalCtrl，
// 会让 jobs 头文件把整个 GUI 头也拖进来，导致无 GUI 的 job_tests 无法只包含
// JobRunner.h。JobRunHandle / RunJobAsync 本身是纯 jobs 层的，不应该有这个依赖。

#include "../TerminalCtrl.h"

#include <wx/app.h>
#include <wx/string.h>
#include <wx/weakref.h>

#include <memory>
#include <mutex>

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
        if (!schedule) return;
        if (wxTheApp == nullptr) {
            // 应用正在退出：直接就地刷新，并复位 m_scheduled。
            // 旧实现只是 return，m_scheduled 永远停在 true ——
            // 之后任何 Push 都不会再安排 CallAfter，缓冲的输出再也不会显示。
            Flush();
            return;
        }
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
