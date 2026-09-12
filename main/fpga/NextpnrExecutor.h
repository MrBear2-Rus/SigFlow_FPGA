// NextpnrExecutor.h
// nextpnr 异步执行器 — 仿照 FpgaYosysExecutor 架构。
// 职责：
//   1. 进程管理：CreateProcess + Job Object + 双管道 + 独立 I/O 线程
//   2. 前置校验：运行时环境检查 + JSON/CST 校验 + 命令行拼装
//   3. 跑后分析：日志解析 + 报告生成 + 产物校验
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wx/string.h>
#include <wx/datetime.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "NextpnrLogParser.h"
#include "NextpnrReport.h"
#include "CstValidator.h"

// ---------------------------------------------------------------------------
// 运行时检查
// ---------------------------------------------------------------------------
struct NextpnrRuntimeCheck {
    wxString name;
    wxString path;
    bool passed = false;
    wxString message;
};

struct NextpnrRuntimeReport {
    wxString executablePath;
    wxString shareDirectory;
    std::vector<NextpnrRuntimeCheck> checks;
    bool valid = false;

    wxString FormatForTerminal() const;
};

NextpnrRuntimeReport ValidateNextpnrRuntime(const wxString& executablePath,
                                             const wxString& shareDirectory);

// ---------------------------------------------------------------------------
// NextpnrExecuteRequest — Prepare 需要的输入
// ---------------------------------------------------------------------------
struct NextpnrExecuteRequest {
    wxString projectPath;         // 项目根目录（用于 CST 自动搜索）
    wxString topModule;           // 顶层模块名
    wxString jsonPath;            // Yosys JSON 网表路径
    wxString configuredCstPath;   // 来自 project config 的 CST 路径（可为空）
    wxString deviceName;          // 例如 "GW1NR-LV9QN88PC6/I5"
    wxString familyName;          // 例如 "GW1N-9C"
    wxString executablePath;      // nextpnr-himbaechel.exe 路径
    wxString outputDirectory;     // 产物目录（.pnr.json + .analysis.json）
};

// ---------------------------------------------------------------------------
// NextpnrJobResult — Finalize 返回的产物
// ---------------------------------------------------------------------------
struct NextpnrJobResult {
    bool succeeded = false;
    int exitCode = -1;
    wxDateTime startTime;
    wxDateTime endTime;
    double elapsedSec = 0.0;
    wxString pnrJsonPath;
    wxString analysisJsonPath;
    wxString terminalSummary;
};

// ---------------------------------------------------------------------------
// NextpnrExecutor
// ---------------------------------------------------------------------------
class NextpnrExecutor {
public:
    // ---- 进程管理（仿照 YosysExecutor）----
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

    NextpnrExecutor();
    ~NextpnrExecutor();

    // 非阻塞启动进程，完成后触发 onComplete 回调
    bool Execute(const wxString& executable,
                 const std::vector<wxString>& args,
                 const Config& config,
                 OutputCallback onOutput,
                 CompletionCallback onComplete);

    // 终止进程树，完成回调会报告 Cancelled
    void Cancel();

    State GetState() const { return m_state.load(); }

    // ---- nextpnr 业务逻辑 ----
    // 校验输入 + 构建命令行参数，成功后 GetArguments() 可用
    bool Prepare(NextpnrExecuteRequest& req, wxString& errorMsg);
    const std::vector<wxString>& GetArguments() const { return m_arguments; }

    // 进程完成后调用：日志解析 + 报告生成 + 产物校验
    NextpnrJobResult Finalize(int exitCode, const wxString& combinedLog);

private:
    // ---- 进程管理 ----
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

    // ---- nextpnr 业务 ----
    NextpnrExecuteRequest m_request;
    std::vector<wxString> m_arguments;
    wxDateTime m_jobStartTime;

    NextpnrLogParser m_logParser;
    NextpnrReport m_report;

    bool ValidateInputs(NextpnrExecuteRequest& req, wxString& errorMsg);
    std::vector<wxString> BuildArguments(const NextpnrExecuteRequest& req);
    bool ValidateArtifact(const wxString& pnrJsonPath);
    void RunAnalysis(const wxString& combinedLog, NextpnrJobResult& result);
};
