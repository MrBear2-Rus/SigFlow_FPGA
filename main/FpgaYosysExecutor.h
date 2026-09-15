#pragma once

#include <wx/string.h>

#include <atomic>
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
        int processId = 0;
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
    bool WriteCombinedLog();

    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_shouldTerminate{false};
    std::atomic<int> m_exitCode{-1};
    std::atomic<CompletionReason> m_completionReason{CompletionReason::Success};

    Config m_config;

    mutable std::mutex m_handleMutex;
    void* m_handle = nullptr;

    mutable std::mutex m_logMutex;
    wxString m_combinedLog;
    size_t m_logSize = 0;

    std::thread m_worker;
    OutputCallback m_outputCallback;
    CompletionCallback m_completionCallback;
};
