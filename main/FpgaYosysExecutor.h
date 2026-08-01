#pragma once

#include <wx/string.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// YosysExecutor — 异步 Yosys 进程执行器
//
// 职责：
//   1. 以参数数组直接启动 Yosys 进程（禁止经 cmd /c 拼接字符串）
//   2. 独立异步读取 stdout/stderr, 按时间顺序写入 combined log 和回调
//   3. 支持可配置的时限、日志上限
//   4. 取消或超时时终止进程树（Job Object），记录实际退出码
//   5. 区分启动失败、非零退出和取消三种终态
//
// 线程安全：
//   - Execute() / Cancel() 可从任意线程调用
//   - 输出回调和完成回调在后台线程中触发，调用方不得在此回调中执行 UI 操作
//     （需自行 wxQueueEvent 到 UI 线程）
// ---------------------------------------------------------------------------
class YosysExecutor {
public:
    // ---- 配置 ----
    struct Config {
        int timeLimitSec = 0;     // 0 = 不限时
        size_t logSizeLimit = 0;  // 0 = 不限（字节），超限保留末尾
        wxString workingDirectory;
    };

    // ---- 执行器状态 ----
    enum class State {
        Idle,
        Running,
        Completed,
    };

    // ---- 完成原因 ----
    enum class CompletionReason {
        Success,       // exit code == 0
        NonZeroExit,   // exit code != 0
        Cancelled,     // 用户调用 Cancel()
        TimedOut,      // 超出 timeLimitSec
        LaunchFailed,  // 进程启动失败（CreateProcess 失败）
    };

    // ---- 结果 ----
    struct Result {
        CompletionReason reason = CompletionReason::Success;
        int exitCode = -1;
        wxString combinedLog;  // 完整输出日志
    };

    // ---- 回调类型 ----
    using OutputCallback = std::function<void(const wxString& text)>;
    using CompletionCallback = std::function<void(const Result& result)>;

    YosysExecutor();
    ~YosysExecutor();

    // 非阻塞启动进程
    // 返回 false 表示启动失败（LaunchFailed），此时不会触发任何回调
    bool Execute(const wxString& executable,
                 const std::vector<wxString>& args,
                 const Config& config,
                 OutputCallback onOutput,
                 CompletionCallback onComplete);

    // 取消正在运行的进程：终止进程树，输出线程读取剩余数据后触发完成回调
    void Cancel();

    State GetState() const { return m_state; }

private:
    // ---- 内部辅助 ----
    static wxString BuildCommandLine(const wxString& executable,
                                     const std::vector<wxString>& args);

    bool CreatePipes();
    bool CreateProcessAndJob(const wxString& commandLine);
    void AssignProcessToJob();
    void KillProcessTree();
    void DrainPipe(HANDLE hPipe, bool drainAll);

    // 后台线程：轮询输出 + 等待进程结束 + 触发完成回调
    void OutputThreadFunc();

    // 清理所有句柄并等待线程退出
    void Cleanup();

    // ---- Windows 句柄 ----
    HANDLE m_hProcess = nullptr;
    HANDLE m_hStdOutRead = nullptr;
    HANDLE m_hStdOutWrite = nullptr;
    HANDLE m_hStdErrRead = nullptr;
    HANDLE m_hStdErrWrite = nullptr;
    HANDLE m_hJobObject = nullptr;

    // ---- 原子状态 ----
    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_shouldTerminate{false};
    std::atomic<int> m_exitCode{-1};
    std::atomic<CompletionReason> m_completionReason{CompletionReason::Success};

    // ---- 配置快照 ----
    Config m_config;
    std::chrono::steady_clock::time_point m_startTime;

    // ---- 输出日志 ----
    mutable std::mutex m_logMutex;
    wxString m_combinedLog;
    size_t m_logSize = 0;

    // ---- 后台线程 ----
    std::thread m_outputThread;

    // ---- 回调 ----
    OutputCallback m_outputCallback;
    CompletionCallback m_completionCallback;
};