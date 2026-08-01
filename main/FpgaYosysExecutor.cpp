#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#include <windows.h>
#include <psapi.h>

#include "FpgaYosysExecutor.h"

#include <wx/log.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <string>

#pragma comment(lib, "psapi.lib")

namespace {

// ---------------------------------------------------------------------------
// 将命令参数拼成 CreateProcessW 可用的命令行字符串
// 规则：路径含空格时加引号；参数含空格时加引号
// ---------------------------------------------------------------------------
wxString BuildCommandLineImpl(const wxString& executable,
                              const std::vector<wxString>& args)
{
    wxString cmdLine;

    // 可执行文件路径
    if (executable.Contains(' ') || executable.Contains('\t')) {
        cmdLine = L"\"" + executable + L"\"";
    } else {
        cmdLine = executable;
    }

    for (const wxString& arg : args) {
        cmdLine += L" ";
        if (arg.Contains(' ') || arg.Contains('\t')) {
            wxString escaped = arg;
            escaped.Replace(L"\"", L"\\\"");
            cmdLine += L"\"" + escaped + L"\"";
        } else {
            cmdLine += arg;
        }
    }

    return cmdLine;
}

constexpr DWORD kPipeBufferSize = 64 * 1024;
constexpr DWORD kPollIntervalMs = 50;

} // anonymous namespace

// ============================================================================
// 辅助：获取当前时间戳字符串（微秒精度）
// ============================================================================
namespace {

wxString TimestampForLog()
{
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return wxString::Format("[T+%lldms]", ms);
}

// 写入调试日志（同时输出到 OutputDebugString 和文件）
#pragma warning(push)
#pragma warning(disable: 4717)
void AsyncDbgLog(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const wxString msg = wxString::FormatV(format, args);
    va_end(args);

    OutputDebugStringW(msg.wc_str());
    static std::mutex s_mtx;
    std::lock_guard<std::mutex> lock(s_mtx);
    FILE* f = _wfopen(L"yosys_async_debug.log", L"ab");
    if (f) {
        const wxScopedCharBuffer utf8 = msg.ToUTF8();
        fwrite(utf8.data(), 1, utf8.length(), f);
        fwrite("\r\n", 1, 2, f);
        fclose(f);
    }
}
#pragma warning(pop)

} // anonymous namespace

// ============================================================================
// YosysExecutor
// ============================================================================

YosysExecutor::YosysExecutor() = default;

YosysExecutor::~YosysExecutor()
{
    AsyncDbgLog("[YosysExecutor] ~dtor enter (state=%d)", static_cast<int>(m_state.load()));
    if (m_state != State::Idle) {
        m_shouldTerminate = true;
        KillProcessTree();
    }
    Cleanup();
    AsyncDbgLog("[YosysExecutor] ~dtor exit");
}

// ---------------------------------------------------------------------------
// Execute — 非阻塞启动
// ---------------------------------------------------------------------------
bool YosysExecutor::Execute(const wxString& executable,
                            const std::vector<wxString>& args,
                            const Config& config,
                            OutputCallback onOutput,
                            CompletionCallback onComplete)
{
    AsyncDbgLog("[YosysExecutor] Execute >>> ENTRY  %s", TimestampForLog().wc_str());
    AsyncDbgLog("[YosysExecutor] Execute: executable=\"%s\" args=%zu",
               executable.wc_str(), args.size());

    if (m_state != State::Idle) {
        wxLogWarning("YosysExecutor: already running or completed.");
        return false;
    }

    // 快照配置
    m_config = config;
    m_outputCallback = std::move(onOutput);
    m_completionCallback = std::move(onComplete);
    m_shouldTerminate = false;
    m_exitCode = -1;
    m_completionReason = CompletionReason::Success;
    m_combinedLog.clear();
    m_logSize = 0;

    // 构造命令行
    const wxString commandLine = BuildCommandLineImpl(executable, args);
    AsyncDbgLog("[YosysExecutor] Execute: commandLine=\"%s\"", commandLine.wc_str());

    // 创建管道
    AsyncDbgLog("[YosysExecutor] Execute: creating pipes... %s", TimestampForLog().wc_str());
    if (!CreatePipes()) {
        AsyncDbgLog("[YosysExecutor] Execute: FAILED CreatePipes %s", TimestampForLog().wc_str());
        Cleanup();
        return false;
    }
    AsyncDbgLog("[YosysExecutor] Execute: pipes created OK %s", TimestampForLog().wc_str());

    // 创建进程 + Job Object
    AsyncDbgLog("[YosysExecutor] Execute: creating process... %s", TimestampForLog().wc_str());
    if (!CreateProcessAndJob(commandLine)) {
        AsyncDbgLog("[YosysExecutor] Execute: FAILED CreateProcessAndJob %s", TimestampForLog().wc_str());
        Cleanup();
        return false;
    }
    AsyncDbgLog("[YosysExecutor] Execute: process created OK %s", TimestampForLog().wc_str());

    m_state = State::Running;
    m_startTime = std::chrono::steady_clock::now();

    // 关闭父进程持有的写入端，否则 ReadFile 无法检测管道断裂
    if (m_hStdOutWrite) {
        CloseHandle(m_hStdOutWrite);
        m_hStdOutWrite = nullptr;
    }
    if (m_hStdErrWrite) {
        CloseHandle(m_hStdErrWrite);
        m_hStdErrWrite = nullptr;
    }

    // 启动后台输出线程
    AsyncDbgLog("[YosysExecutor] Execute: launching output thread... %s", TimestampForLog().wc_str());
    m_outputThread = std::thread(&YosysExecutor::OutputThreadFunc, this);
    AsyncDbgLog("[YosysExecutor] Execute: output thread launched %s", TimestampForLog().wc_str());

    AsyncDbgLog("[YosysExecutor] Execute <<< RETURN true (async) %s", TimestampForLog().wc_str());
    return true;
}

// ---------------------------------------------------------------------------
// Cancel — 取消运行中的进程
// ---------------------------------------------------------------------------
void YosysExecutor::Cancel()
{
    AsyncDbgLog("[YosysExecutor] Cancel >>> ENTRY  %s", TimestampForLog().wc_str());

    State expected = State::Running;
    if (!m_state.compare_exchange_strong(expected, State::Completed)) {
        AsyncDbgLog("[YosysExecutor] Cancel: not in Running state (state=%d), skip",
                   static_cast<int>(expected));
        return; // 不在运行中，无需取消
    }

    AsyncDbgLog("[YosysExecutor] Cancel: state atomically switched Running->Completed  %s",
               TimestampForLog().wc_str());

    m_shouldTerminate = true;

    AsyncDbgLog("[YosysExecutor] Cancel: calling KillProcessTree()  %s", TimestampForLog().wc_str());
    KillProcessTree();
    AsyncDbgLog("[YosysExecutor] Cancel: KillProcessTree() done  %s", TimestampForLog().wc_str());

    // 等待输出线程结束
    AsyncDbgLog("[YosysExecutor] Cancel: joining output thread...  %s", TimestampForLog().wc_str());
    if (m_outputThread.joinable()) {
        m_outputThread.join();
        AsyncDbgLog("[YosysExecutor] Cancel: output thread joined  %s", TimestampForLog().wc_str());
    } else {
        AsyncDbgLog("[YosysExecutor] Cancel: output thread not joinable  %s", TimestampForLog().wc_str());
    }

    // 获取最终退出码
    if (m_hProcess) {
        DWORD code = 0;
        if (GetExitCodeProcess(m_hProcess, &code)) {
            m_exitCode = static_cast<int>(code);
            AsyncDbgLog("[YosysExecutor] Cancel: exit code = %d  %s",
                       static_cast<int>(m_exitCode), TimestampForLog().wc_str());
        }
    }

    // 最终读取一次剩余输出
    AsyncDbgLog("[YosysExecutor] Cancel: draining remaining pipe data  %s", TimestampForLog().wc_str());
    DrainPipe(m_hStdOutRead, true);
    DrainPipe(m_hStdErrRead, true);

    // 清理资源，不触发回调（回调已在输出线程中完成）
    AsyncDbgLog("[YosysExecutor] Cancel: calling Cleanup()  %s", TimestampForLog().wc_str());
    Cleanup();
    AsyncDbgLog("[YosysExecutor] Cancel <<< EXIT  %s", TimestampForLog().wc_str());
}

// ============================================================================
// 私有方法
// ============================================================================

// ---------------------------------------------------------------------------
// CreatePipes — 创建 stdout/stderr 管道
// ---------------------------------------------------------------------------
bool YosysExecutor::CreatePipes()
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // stdout 管道
    if (!CreatePipe(&m_hStdOutRead, &m_hStdOutWrite, &sa, kPipeBufferSize)) {
        wxLogError("YosysExecutor: CreatePipe (stdout) failed, error %lu", GetLastError());
        return false;
    }
    if (!SetHandleInformation(m_hStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("YosysExecutor: SetHandleInformation (stdout read) failed, error %lu",
                   GetLastError());
        return false;
    }

    // stderr 管道
    if (!CreatePipe(&m_hStdErrRead, &m_hStdErrWrite, &sa, kPipeBufferSize)) {
        wxLogError("YosysExecutor: CreatePipe (stderr) failed, error %lu", GetLastError());
        return false;
    }
    if (!SetHandleInformation(m_hStdErrRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("YosysExecutor: SetHandleInformation (stderr read) failed, error %lu",
                   GetLastError());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CreateProcessAndJob — 创建进程（挂起）并创建 Job Object
// ---------------------------------------------------------------------------
bool YosysExecutor::CreateProcessAndJob(const wxString& commandLine)
{
    // 启动信息
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = m_hStdOutWrite;
    si.hStdError = m_hStdErrWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // 进程信息
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // 工作目录
    const wchar_t* workDir = nullptr;
    std::wstring workDirW;
    if (!m_config.workingDirectory.IsEmpty()) {
        workDirW = m_config.workingDirectory.ToStdWstring();
        workDir = workDirW.c_str();
    }

    // 命令行（CreateProcessW 要求可写缓冲区）
    std::wstring cmdLineW = commandLine.ToStdWstring();

    // 创建挂起进程，以便在开始执行前 AssignProcessToJobObject
    BOOL ok = CreateProcessW(
        nullptr,               // lpApplicationName
        &cmdLineW[0],          // lpCommandLine (writable)
        nullptr,               // lpProcessAttributes
        nullptr,               // lpThreadAttributes
        TRUE,                  // bInheritHandles
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        nullptr,               // lpEnvironment
        workDir,               // lpCurrentDirectory
        &si,                   // lpStartupInfo
        &pi                    // lpProcessInformation
    );

    if (!ok) {
        DWORD err = GetLastError();
        wxLogError("YosysExecutor: CreateProcessW failed, error %lu", err);
        m_completionReason = CompletionReason::LaunchFailed;
        return false;
    }

    m_hProcess = pi.hProcess;
    AsyncDbgLog("[YosysExecutor] CreateProcessAndJob: PID=%lu  %s",
               pi.dwProcessId, TimestampForLog().wc_str());

    // 创建 Job Object 并分配进程
    m_hJobObject = CreateJobObjectW(nullptr, nullptr);
    if (!m_hJobObject) {
        wxLogError("YosysExecutor: CreateJobObjectW failed, error %lu", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        m_hProcess = nullptr;
        m_completionReason = CompletionReason::LaunchFailed;
        return false;
    }

    // 设置 Job Object 扩展限制：关闭 Job 句柄时杀死所有进程
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
    jobInfo.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(m_hJobObject,
                                 JobObjectExtendedLimitInformation,
                                 &jobInfo, sizeof(jobInfo))) {
        wxLogWarning("YosysExecutor: SetInformationJobObject failed, error %lu", GetLastError());
        // 非致命，继续
    }

    // 将进程分配到 Job Object
    if (!AssignProcessToJobObject(m_hJobObject, m_hProcess)) {
        wxLogError("YosysExecutor: AssignProcessToJobObject failed, error %lu", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        CloseHandle(m_hJobObject);
        m_hProcess = nullptr;
        m_hJobObject = nullptr;
        m_completionReason = CompletionReason::LaunchFailed;
        return false;
    }

    // 恢复主线程（进程开始执行）
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    return true;
}

// ---------------------------------------------------------------------------
// KillProcessTree — 终止 Job Object 内的所有进程
// ---------------------------------------------------------------------------
void YosysExecutor::KillProcessTree()
{
    if (m_hJobObject) {
        TerminateJobObject(m_hJobObject, 1);
    } else if (m_hProcess) {
        // 没有 Job Object 时直接终止主进程
        TerminateProcess(m_hProcess, 1);
    }
}

// ---------------------------------------------------------------------------
// DrainPipe — 非阻塞管道读取
//   drainAll = true  : 读尽管道中所有数据
//   drainAll = false : 只读一个 chunk
// ---------------------------------------------------------------------------
void YosysExecutor::DrainPipe(HANDLE hPipe, bool drainAll)
{
    if (!hPipe) return;

    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    DWORD bytesRead;
    DWORD bytesAvailable;

    do {
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
            break; // 管道已断或无效
        }
        if (bytesAvailable == 0) {
            break;
        }

        const DWORD toRead = (std::min)(bytesAvailable,
                                      static_cast<DWORD>(BUFFER_SIZE - 1));
        if (!ReadFile(hPipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }

        buffer[bytesRead] = '\0';
        wxString text(buffer, wxConvUTF8);

        // 写入 combined log
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            if (m_config.logSizeLimit == 0 || m_logSize < m_config.logSizeLimit) {
                m_combinedLog += text;
                m_logSize += text.length();
                if (m_config.logSizeLimit > 0 && m_logSize > m_config.logSizeLimit) {
                    // 保留末尾 logSizeLimit 字节
                    m_combinedLog = m_combinedLog.Right(m_config.logSizeLimit);
                    m_logSize = m_combinedLog.length();
                }
            }
        }

        // 回调输出
        if (m_outputCallback) {
            m_outputCallback(text);
        }
    } while (drainAll);
}

// ---------------------------------------------------------------------------
// OutputThreadFunc — 后台输出轮询线程
// ---------------------------------------------------------------------------
void YosysExecutor::OutputThreadFunc()
{
    AsyncDbgLog("[YosysExecutor] OutputThreadFunc: THREAD STARTED (thread_id=%llx)  %s",
               std::hash<std::thread::id>{}(std::this_thread::get_id()),
               TimestampForLog().wc_str());

    bool processEnded = false;
    int pollCycle = 0;

    while (!m_shouldTerminate && !processEnded) {
        // 检查进程是否已退出
        if (WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0) {
            processEnded = true;
            AsyncDbgLog("[YosysExecutor] OutputThreadFunc: process exited detected  %s",
                       TimestampForLog().wc_str());
        }

        // 检查超时
        if (m_config.timeLimitSec > 0 && !processEnded) {
            const auto elapsed = std::chrono::steady_clock::now() - m_startTime;
            if (elapsed >= std::chrono::seconds(m_config.timeLimitSec)) {
                AsyncDbgLog("[YosysExecutor] OutputThreadFunc: TIMEOUT (%d sec)  %s",
                           m_config.timeLimitSec, TimestampForLog().wc_str());
                m_completionReason = CompletionReason::TimedOut;
                KillProcessTree();
                // 等待进程结束，最多等 5 秒；超时则强制终止
                if (WaitForSingleObject(m_hProcess, 5000) != WAIT_OBJECT_0) {
                    TerminateProcess(m_hProcess, 1);
                    WaitForSingleObject(m_hProcess, 1000);
                }
                processEnded = true;
            }
        }

        // 读取 stdout
        DrainPipe(m_hStdOutRead, false);
        // 读取 stderr
        DrainPipe(m_hStdErrRead, false);

        if (!processEnded) {
            ++pollCycle;
            if (pollCycle % 200 == 0) {  // 每 200 轮 ≈ 10 秒打印一次心跳
                AsyncDbgLog("[YosysExecutor] OutputThreadFunc: polling... cycle=%d  %s",
                           pollCycle, TimestampForLog().wc_str());
            }
            Sleep(kPollIntervalMs);
        }
    }

    AsyncDbgLog("[YosysExecutor] OutputThreadFunc: process ended, final drain  %s",
               TimestampForLog().wc_str());

    // 进程结束后的最终读取
    DrainPipe(m_hStdOutRead, true);
    DrainPipe(m_hStdErrRead, true);

    // 获取退出码
    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_hProcess, &exitCode)) {
        m_exitCode = static_cast<int>(exitCode);
    }

    // 确定最终完成原因
    if (m_completionReason == CompletionReason::TimedOut) {
        // 保持超时原因（已在超时检测分支中设定）
    } else if (m_shouldTerminate) {
        m_completionReason = CompletionReason::Cancelled;
    } else if (m_exitCode != 0) {
        m_completionReason = CompletionReason::NonZeroExit;
    } else {
        m_completionReason = CompletionReason::Success;
    }

    AsyncDbgLog("[YosysExecutor] OutputThreadFunc: reason=%d exitCode=%d  %s",
               static_cast<int>(m_completionReason.load()), static_cast<int>(m_exitCode), TimestampForLog().wc_str());

    m_state = State::Completed;

    // 触发完成回调
    if (m_completionCallback) {
        AsyncDbgLog("[YosysExecutor] OutputThreadFunc: invoking completion callback from THREAD  %s",
                   TimestampForLog().wc_str());
        Result result;
        result.reason = m_completionReason;
        result.exitCode = m_exitCode;
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            result.combinedLog = m_combinedLog;
        }
        m_completionCallback(result);
        AsyncDbgLog("[YosysExecutor] OutputThreadFunc: completion callback returned  %s",
                   TimestampForLog().wc_str());
    }

    AsyncDbgLog("[YosysExecutor] OutputThreadFunc: THREAD EXITING  %s", TimestampForLog().wc_str());
}

// ---------------------------------------------------------------------------
// Cleanup — 释放所有资源
// ---------------------------------------------------------------------------
void YosysExecutor::Cleanup()
{
    AsyncDbgLog("[YosysExecutor] Cleanup >>> ENTRY  %s", TimestampForLog().wc_str());

    // 等待输出线程结束
    if (m_outputThread.joinable()) {
        AsyncDbgLog("[YosysExecutor] Cleanup: joining output thread...  %s", TimestampForLog().wc_str());
        m_outputThread.join();
        AsyncDbgLog("[YosysExecutor] Cleanup: output thread joined  %s", TimestampForLog().wc_str());
    }

    // 关闭句柄
    if (m_hStdOutRead) {
        CloseHandle(m_hStdOutRead);
        m_hStdOutRead = nullptr;
        AsyncDbgLog("[YosysExecutor] Cleanup: hStdOutRead closed  %s", TimestampForLog().wc_str());
    }
    if (m_hStdOutWrite) {
        CloseHandle(m_hStdOutWrite);
        m_hStdOutWrite = nullptr;
    }
    if (m_hStdErrRead) {
        CloseHandle(m_hStdErrRead);
        m_hStdErrRead = nullptr;
        AsyncDbgLog("[YosysExecutor] Cleanup: hStdErrRead closed  %s", TimestampForLog().wc_str());
    }
    if (m_hStdErrWrite) {
        CloseHandle(m_hStdErrWrite);
        m_hStdErrWrite = nullptr;
    }
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
        AsyncDbgLog("[YosysExecutor] Cleanup: hProcess closed  %s", TimestampForLog().wc_str());
    }
    if (m_hJobObject) {
        // 关闭 Job Object 句柄 → 由于 KILL_ON_JOB_CLOSE，所有子进程被终止
        CloseHandle(m_hJobObject);
        m_hJobObject = nullptr;
        AsyncDbgLog("[YosysExecutor] Cleanup: hJobObject closed  %s", TimestampForLog().wc_str());
    }

    m_state = State::Idle;
    m_shouldTerminate = false;
    m_exitCode = -1;

    AsyncDbgLog("[YosysExecutor] Cleanup <<< EXIT  %s", TimestampForLog().wc_str());
}