#include "ProcessRunner.h"
#include <windows.h>
#include <wx/log.h>

ProcessRunner::ProcessRunner()
{
}

ProcessRunner::~ProcessRunner()
{
    if (m_isRunning) {
        Terminate();
    }
    Cleanup();
}

bool ProcessRunner::RunAsync(const wxString& command, const wxString& workingDir)
{
    if (m_isRunning) {
        wxLogError("ProcessRunner: 已有进程在运行");
        return false;
    }

    Cleanup();

    // 创建 stdout 管道
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&m_hStdOutRead, &m_hStdOutWrite, &sa, 0)) {
        wxLogError("ProcessRunner: 创建 stdout 管道失败");
        return false;
    }

    // 确保读取端不被继承
    if (!SetHandleInformation(m_hStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("ProcessRunner: SetHandleInformation 失败");
        return false;
    }

    // 创建 stderr 管道
    if (!CreatePipe(&m_hStdErrRead, &m_hStdErrWrite, &sa, 0)) {
        wxLogError("ProcessRunner: 创建 stderr 管道失败");
        return false;
    }

    if (!SetHandleInformation(m_hStdErrRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("ProcessRunner: SetHandleInformation (stderr) 失败");
        return false;
    }

    // 设置启动信息
    STARTUPINFO si;
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.hStdOutput = m_hStdOutWrite;
    si.hStdError = m_hStdErrWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    // 隐藏窗口
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // 创建进程
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    // 复制命令字符串（CreateProcessW 可能会修改它）
    wxString cmdLine = command;
    std::wstring cmdLineW = cmdLine.ToStdWstring();

    // 工作目录
    const wchar_t* workDir = nullptr;
    std::wstring workDirW;
    if (!workingDir.IsEmpty()) {
        workDirW = workingDir.ToStdWstring();
        workDir = workDirW.c_str();
    }

    BOOL success = CreateProcessW(
        nullptr,                    // 不指定模块名，使用命令行
        &cmdLineW[0],              // 命令行
        nullptr,                   // 进程安全属性
        nullptr,                   // 线程安全属性
        TRUE,                      // 继承句柄
        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,  // 创建标志
        nullptr,                   // 使用父进程环境
        workDir,                   // 工作目录
        &si,                       // 启动信息
        &pi                        // 进程信息
    );

    if (!success) {
        DWORD error = GetLastError();
        wxLogError("ProcessRunner: CreateProcess 失败，错误码: %lu", error);
        Cleanup();
        return false;
    }

    // 保存句柄
    m_hProcess = pi.hProcess;
    m_hThread = pi.hThread;
    m_isRunning = true;
    m_exitCode = -1;
    m_shouldTerminate = false;

    // 关闭子进程不需要的管道写入端（子进程已经继承了写入端）
    CloseHandle(m_hStdOutWrite);
    m_hStdOutWrite = nullptr;
    CloseHandle(m_hStdErrWrite);
    m_hStdErrWrite = nullptr;

    // 启动后台线程读取输出
    m_outputThread = std::thread(&ProcessRunner::ReadOutputThread, this);

    return true;
}

bool ProcessRunner::RunBatchFile(const wxString& batchFilePath, const wxString& workingDir)
{
    // 构造命令：cmd /C "batchfile"
    wxString cmd = wxString::Format("cmd /C \"%s\"", batchFilePath);
    return RunAsync(cmd, workingDir);
}

bool ProcessRunner::Terminate()
{
    if (!m_isRunning || !m_hProcess) {
        return false;
    }

    m_shouldTerminate = true;

    // 尝试优雅终止（发送 Ctrl+C）
    // 注意：这对于批处理脚本可能无效，所以我们也使用 TerminateProcess

    if (!TerminateProcess(m_hProcess, 1)) {
        DWORD error = GetLastError();
        wxLogWarning("ProcessRunner: TerminateProcess 失败，错误码: %lu", error);
        return false;
    }

    // 等待线程结束
    if (m_outputThread.joinable()) {
        m_outputThread.join();
    }

    m_isRunning = false;
    return true;
}

bool ProcessRunner::WaitForCompletion(DWORD timeoutMs)
{
    if (!m_hProcess) {
        return false;
    }

    DWORD waitResult = WaitForSingleObject(m_hProcess, timeoutMs);
    
    if (waitResult == WAIT_OBJECT_0) {
        // 进程结束
        DWORD exitCode;
        if (GetExitCodeProcess(m_hProcess, &exitCode)) {
            m_exitCode = static_cast<int>(exitCode);
        }
        
        // 等待输出线程结束
        if (m_outputThread.joinable()) {
            m_outputThread.join();
        }
        
        m_isRunning = false;
        return true;
    }
    
    return false; // 超时或错误
}

void ProcessRunner::ReadOutputThread()
{
    const int BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    DWORD bytesRead;
    bool processEnded = false;

    // 设置管道为非阻塞模式
    DWORD mode = PIPE_NOWAIT;
    // 注意：Windows 管道不支持真正的非阻塞 I/O，我们使用轮询

    while (!m_shouldTerminate && !processEnded) {
        // 检查进程是否结束
        DWORD waitResult = WaitForSingleObject(m_hProcess, 0);
        if (waitResult == WAIT_OBJECT_0) {
            processEnded = true;
            // 进程刚结束，再读取一次剩余的输出
        }

        // 读取 stdout
        bool hasData = false;
        while (true) {
            bytesRead = 0;
            BOOL success = ReadFile(m_hStdOutRead, buffer, BUFFER_SIZE - 1, &bytesRead, nullptr);
            if (success && bytesRead > 0) {
                hasData = true;
                buffer[bytesRead] = '\0';
                wxString output(buffer, wxConvUTF8);
                
                std::lock_guard<std::mutex> lock(m_mutex);
                m_outputLines.push_back(output);
                
                if (m_outputCallback) {
                    m_outputCallback(output, false);
                }
            } else {
                break;
            }
        }

        // 读取 stderr
        while (true) {
            bytesRead = 0;
            BOOL success = ReadFile(m_hStdErrRead, buffer, BUFFER_SIZE - 1, &bytesRead, nullptr);
            if (success && bytesRead > 0) {
                hasData = true;
                buffer[bytesRead] = '\0';
                wxString output(buffer, wxConvUTF8);
                
                std::lock_guard<std::mutex> lock(m_mutex);
                m_outputLines.push_back(output);
                
                if (m_outputCallback) {
                    m_outputCallback(output, true);
                }
            } else {
                break;
            }
        }

        if (!hasData && !processEnded) {
            // 没有数据且进程未结束，短暂休眠
            Sleep(50);
        }
    }

    // 最后读取一次所有剩余输出
    DWORD bytesAvailable;
    while (PeekNamedPipe(m_hStdOutRead, nullptr, 0, nullptr, &bytesAvailable, nullptr) && bytesAvailable > 0) {
        bytesRead = 0;
        if (ReadFile(m_hStdOutRead, buffer, BUFFER_SIZE - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            wxString output(buffer, wxConvUTF8);
            
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLines.push_back(output);
            
            if (m_outputCallback) {
                m_outputCallback(output, false);
            }
        }
    }

    while (PeekNamedPipe(m_hStdErrRead, nullptr, 0, nullptr, &bytesAvailable, nullptr) && bytesAvailable > 0) {
        bytesRead = 0;
        if (ReadFile(m_hStdErrRead, buffer, BUFFER_SIZE - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            wxString output(buffer, wxConvUTF8);
            
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLines.push_back(output);
            
            if (m_outputCallback) {
                m_outputCallback(output, true);
            }
        }
    }

    // 获取退出码
    DWORD exitCode;
    if (GetExitCodeProcess(m_hProcess, &exitCode)) {
        m_exitCode = static_cast<int>(exitCode);
    }

    m_isRunning = false;

    // 调用完成回调
    if (m_completionCallback) {
        m_completionCallback(m_exitCode);
    }
}

void ProcessRunner::Cleanup()
{
    // 等待线程结束
    if (m_outputThread.joinable()) {
        if (m_outputThread.get_id() != std::this_thread::get_id()) {
            m_outputThread.join();
        } else {
            m_outputThread.detach();
        }
    }

    // 关闭句柄
    if (m_hStdOutRead) {
        CloseHandle(m_hStdOutRead);
        m_hStdOutRead = nullptr;
    }
    if (m_hStdOutWrite) {
        CloseHandle(m_hStdOutWrite);
        m_hStdOutWrite = nullptr;
    }
    if (m_hStdErrRead) {
        CloseHandle(m_hStdErrRead);
        m_hStdErrRead = nullptr;
    }
    if (m_hStdErrWrite) {
        CloseHandle(m_hStdErrWrite);
        m_hStdErrWrite = nullptr;
    }
    if (m_hThread) {
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }

    m_isRunning = false;
    m_exitCode = -1;
}
