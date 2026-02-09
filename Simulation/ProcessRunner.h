#pragma once

#include <wx/wx.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

// 进程输出回调
using OutputCallback = std::function<void(const wxString& output, bool isError)>;
using CompletionCallback = std::function<void(int exitCode)>;

/**
 * Windows CreateProcess 异步进程执行器
 * 支持：
 * 1. 异步执行不阻塞 UI
 * 2. 实时捕获 stdout/stderr
 * 3. 进度回调
 * 4. 中途取消
 */
class ProcessRunner
{
public:
    ProcessRunner();
    ~ProcessRunner();

    // 设置回调
    void SetOutputCallback(OutputCallback callback) { m_outputCallback = callback; }
    void SetCompletionCallback(CompletionCallback callback) { m_completionCallback = callback; }

    /**
     * 异步执行命令
     * @param command 要执行的命令（如：cmd /C "xxx.bat"）
     * @param workingDir 工作目录
     * @return 是否成功启动
     */
    bool RunAsync(const wxString& command, const wxString& workingDir = wxEmptyString);

    /**
     * 执行批处理文件（自动构造 cmd /C 命令）
     * @param batchFilePath 批处理文件路径
     * @param workingDir 工作目录
     * @return 是否成功启动
     */
    bool RunBatchFile(const wxString& batchFilePath, const wxString& workingDir = wxEmptyString);

    // 检查是否正在运行
    bool IsRunning() const { return m_isRunning; }

    // 强制终止进程
    bool Terminate();

    // 获取退出码（进程结束后有效）
    int GetExitCode() const { return m_exitCode; }

    // 等待进程结束（阻塞，用于需要同步等待的场景）
    bool WaitForCompletion(DWORD timeoutMs = INFINITE);

private:
    // 后台读取输出的线程函数
    void ReadOutputThread();

    // 清理资源
    void Cleanup();

private:
    // Windows 进程句柄
    HANDLE m_hProcess = nullptr;
    HANDLE m_hThread = nullptr;
    HANDLE m_hStdOutRead = nullptr;
    HANDLE m_hStdOutWrite = nullptr;
    HANDLE m_hStdErrRead = nullptr;
    HANDLE m_hStdErrWrite = nullptr;

    // 状态
    std::atomic<bool> m_isRunning{ false };
    std::atomic<int> m_exitCode{ -1 };
    std::atomic<bool> m_shouldTerminate{ false };

    // 回调
    OutputCallback m_outputCallback;
    CompletionCallback m_completionCallback;

    // 后台线程
    std::thread m_outputThread;
    std::mutex m_mutex;

    // 收集的输出（用于结束后的完整日志）
    std::vector<wxString> m_outputLines;
};
