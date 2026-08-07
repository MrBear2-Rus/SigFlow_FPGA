#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wx/string.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class YosysExecutor {
public:
    struct Config {
        int timeLimitSec = 0;
        size_t memoryLimitBytes = 0;
        size_t logSizeLimit = 0;
        wxString workingDirectory;
        wxString combinedLogPath;
    };

    enum class State {
        Idle,
        Running,
        Completed,
    };

    enum class CompletionReason {
        Success,
        NonZeroExit,
        Cancelled,
        TimedOut,
        LaunchFailed,
    };

    enum class OutputStream {
        StdOut,
        StdErr,
    };

    struct Result {
        CompletionReason reason = CompletionReason::Success;
        int exitCode = -1;
        DWORD processId = 0;
        bool combinedLogWritten = true;
        wxString combinedLog;
    };

    using OutputCallback = std::function<void(OutputStream stream, const wxString& text)>;
    using CompletionCallback = std::function<void(const Result& result)>;

    YosysExecutor();
    ~YosysExecutor();

    bool Execute(const wxString& executable,
                 const std::vector<wxString>& args,
                 const Config& config,
                 OutputCallback onOutput,
                 CompletionCallback onComplete);

    // This only requests termination; the completion callback reports the final state.
    void Cancel();

    State GetState() const { return m_state.load(); }

private:
    bool CreatePipes();
    bool CreateProcessAndJob(const wxString& executable, const std::vector<wxString>& args);
    void KillProcessTree();
    void DrainPipe(HANDLE hPipe, OutputStream stream, bool drainAll);
    bool WriteCombinedLog();
    void OutputThreadFunc();
    void CloseHandles();
    void Cleanup();

    HANDLE m_hProcess = nullptr;
    HANDLE m_hStdOutRead = nullptr;
    HANDLE m_hStdOutWrite = nullptr;
    HANDLE m_hStdErrRead = nullptr;
    HANDLE m_hStdErrWrite = nullptr;
    HANDLE m_hJobObject = nullptr;

    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_shouldTerminate{false};
    std::atomic<int> m_exitCode{-1};
    std::atomic<DWORD> m_processId{0};
    std::atomic<CompletionReason> m_completionReason{CompletionReason::Success};

    Config m_config;
    std::chrono::steady_clock::time_point m_startTime;

    mutable std::mutex m_logMutex;
    wxString m_combinedLog;
    size_t m_logSize = 0;

    std::thread m_outputThread;
    OutputCallback m_outputCallback;
    CompletionCallback m_completionCallback;
};
