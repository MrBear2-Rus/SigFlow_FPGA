#define _WIN32_WINNT 0x0601
#define WINVER 0x0601

#include "FpgaYosysExecutor.h"

#include <wx/file.h>
#include <wx/log.h>

#include <algorithm>
#include <string>

namespace {

constexpr DWORD kPipeBufferSize = 64 * 1024;
constexpr DWORD kPollIntervalMs = 50;

wxString QuoteWindowsArgument(const wxString& value)
{
    if (value.IsEmpty()) {
        return "\"\"";
    }
    if (!value.Contains(' ') && !value.Contains('\t') && !value.Contains('"')) {
        return value;
    }

    wxString quoted = "\"";
    size_t backslashes = 0;
    for (const wxChar character : value) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') {
            quoted.Append('\\', backslashes * 2 + 1);
            quoted += '"';
            backslashes = 0;
            continue;
        }
        quoted.Append('\\', backslashes);
        backslashes = 0;
        quoted += character;
    }
    quoted.Append('\\', backslashes * 2);
    quoted += '"';
    return quoted;
}

wxString BuildCommandLine(const wxString& executable, const std::vector<wxString>& args)
{
    wxString commandLine = QuoteWindowsArgument(executable);
    for (const wxString& arg : args) {
        commandLine += " ";
        commandLine += QuoteWindowsArgument(arg);
    }
    return commandLine;
}

} // namespace

YosysExecutor::YosysExecutor() = default;

YosysExecutor::~YosysExecutor()
{
    Cancel();
    Cleanup();
}

bool YosysExecutor::Execute(const wxString& executable,
                            const std::vector<wxString>& args,
                            const Config& config,
                            OutputCallback onOutput,
                            CompletionCallback onComplete)
{
    if (m_state.load() != State::Idle) {
        wxLogWarning("YosysExecutor: executor is not idle.");
        return false;
    }

    m_config = config;
    m_outputCallback = std::move(onOutput);
    m_completionCallback = std::move(onComplete);
    m_shouldTerminate = false;
    m_exitCode = -1;
    m_processId = 0;
    m_completionReason = CompletionReason::Success;
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_combinedLog.clear();
        m_logSize = 0;
    }

    if (!CreatePipes() || !CreateProcessAndJob(executable, args)) {
        m_completionReason = CompletionReason::LaunchFailed;
        Cleanup();
        return false;
    }

    if (m_hStdOutWrite) {
        CloseHandle(m_hStdOutWrite);
        m_hStdOutWrite = nullptr;
    }
    if (m_hStdErrWrite) {
        CloseHandle(m_hStdErrWrite);
        m_hStdErrWrite = nullptr;
    }

    m_startTime = std::chrono::steady_clock::now();
    m_state = State::Running;
    m_outputThread = std::thread(&YosysExecutor::OutputThreadFunc, this);
    return true;
}

void YosysExecutor::Cancel()
{
    if (m_state.load() != State::Running) {
        return;
    }
    m_shouldTerminate = true;
    KillProcessTree();
}

bool YosysExecutor::CreatePipes()
{
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    if (!CreatePipe(&m_hStdOutRead, &m_hStdOutWrite, &attributes, kPipeBufferSize) ||
        !SetHandleInformation(m_hStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("YosysExecutor: unable to create stdout pipe (%lu).", GetLastError());
        return false;
    }
    if (!CreatePipe(&m_hStdErrRead, &m_hStdErrWrite, &attributes, kPipeBufferSize) ||
        !SetHandleInformation(m_hStdErrRead, HANDLE_FLAG_INHERIT, 0)) {
        wxLogError("YosysExecutor: unable to create stderr pipe (%lu).", GetLastError());
        return false;
    }
    return true;
}

bool YosysExecutor::CreateProcessAndJob(const wxString& executable,
                                        const std::vector<wxString>& args)
{
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = m_hStdOutWrite;
    startupInfo.hStdError = m_hStdErrWrite;

    PROCESS_INFORMATION processInfo = {};
    const wxString commandLine = BuildCommandLine(executable, args);
    std::wstring writableCommandLine = commandLine.ToStdWstring();
    std::wstring workingDirectory = m_config.workingDirectory.ToStdWstring();

    if (!CreateProcessW(executable.wc_str(), writableCommandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                        &startupInfo, &processInfo)) {
        wxLogError("YosysExecutor: CreateProcessW failed (%lu).", GetLastError());
        return false;
    }

    m_hProcess = processInfo.hProcess;
    m_processId = processInfo.dwProcessId;
    m_hJobObject = CreateJobObjectW(nullptr, nullptr);
    if (!m_hJobObject) {
        wxLogError("YosysExecutor: CreateJobObjectW failed (%lu).", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (m_config.memoryLimitBytes > 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(m_config.memoryLimitBytes);
    }
    if (!SetInformationJobObject(m_hJobObject, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        wxLogError("YosysExecutor: SetInformationJobObject failed (%lu).", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }
    if (!AssignProcessToJobObject(m_hJobObject, m_hProcess)) {
        wxLogError("YosysExecutor: AssignProcessToJobObject failed (%lu).", GetLastError());
        TerminateProcess(m_hProcess, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }
    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        wxLogError("YosysExecutor: ResumeThread failed (%lu).", GetLastError());
        TerminateJobObject(m_hJobObject, 1);
        CloseHandle(processInfo.hThread);
        return false;
    }

    CloseHandle(processInfo.hThread);
    return true;
}

void YosysExecutor::KillProcessTree()
{
    if (m_hJobObject) {
        TerminateJobObject(m_hJobObject, 1);
    } else if (m_hProcess) {
        TerminateProcess(m_hProcess, 1);
    }
}

void YosysExecutor::DrainPipe(HANDLE pipe, OutputStream stream, bool drainAll)
{
    if (!pipe) {
        return;
    }

    char buffer[4096];
    do {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            break;
        }
        DWORD bytesRead = 0;
        const DWORD toRead = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(pipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }

        const wxString text = wxString::FromUTF8(buffer, bytesRead);
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            if (m_config.logSizeLimit == 0) {
                m_combinedLog += text;
                m_logSize += bytesRead;
            } else {
                m_combinedLog += text;
                m_logSize += bytesRead;
                while (!m_combinedLog.IsEmpty() && m_logSize > m_config.logSizeLimit) {
                    const wxString firstCharacter = m_combinedLog.Left(1);
                    const wxScopedCharBuffer firstCharacterUtf8 = firstCharacter.ToUTF8();
                    m_logSize -= (std::min)(m_logSize, firstCharacterUtf8.length());
                    m_combinedLog.Remove(0, 1);
                }
            }
        }
        if (m_outputCallback) {
            m_outputCallback(stream, text);
        }
    } while (drainAll);
}

bool YosysExecutor::WriteCombinedLog()
{
    if (m_config.combinedLogPath.IsEmpty()) {
        return true;
    }

    wxString combinedLog;
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        combinedLog = m_combinedLog;
    }

    wxFile logFile(m_config.combinedLogPath, wxFile::write);
    if (!logFile.IsOpened()) {
        wxLogWarning("YosysExecutor: unable to write combined log: %s", m_config.combinedLogPath);
        return false;
    }
    const wxScopedCharBuffer utf8 = combinedLog.ToUTF8();
    const char* data = utf8.data();
    const size_t length = data ? utf8.length() : 0;
    const bool written = length == 0 ||
        logFile.Write(data, length) == static_cast<wxFileOffset>(length);
    if (!written) {
        wxLogWarning("YosysExecutor: unable to finish combined log: %s", m_config.combinedLogPath);
    }
    logFile.Close();
    return written;
}

void YosysExecutor::OutputThreadFunc()
{
    bool processEnded = false;
    while (!processEnded) {
        if (WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0) {
            processEnded = true;
        } else if (m_config.timeLimitSec > 0 &&
                   std::chrono::steady_clock::now() - m_startTime >=
                       std::chrono::seconds(m_config.timeLimitSec)) {
            m_completionReason = CompletionReason::TimedOut;
            m_shouldTerminate = true;
            KillProcessTree();
        } else if (m_shouldTerminate.load()) {
            m_completionReason = CompletionReason::Cancelled;
            KillProcessTree();
        }

        DrainPipe(m_hStdOutRead, OutputStream::StdOut, false);
        DrainPipe(m_hStdErrRead, OutputStream::StdErr, false);
        if (!processEnded) {
            Sleep(kPollIntervalMs);
        }
    }

    DrainPipe(m_hStdOutRead, OutputStream::StdOut, true);
    DrainPipe(m_hStdErrRead, OutputStream::StdErr, true);

    DWORD exitCode = 1;
    if (GetExitCodeProcess(m_hProcess, &exitCode)) {
        m_exitCode = static_cast<int>(exitCode);
    }
    if (m_completionReason == CompletionReason::Success && m_shouldTerminate) {
        m_completionReason = CompletionReason::Cancelled;
    } else if (m_completionReason == CompletionReason::Success && m_exitCode != 0) {
        m_completionReason = CompletionReason::NonZeroExit;
    }

    const bool combinedLogWritten = WriteCombinedLog();

    Result result;
    result.reason = m_completionReason.load();
    result.exitCode = m_exitCode.load();
    result.processId = m_processId.load();
    result.combinedLogWritten = combinedLogWritten;
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        result.combinedLog = m_combinedLog;
    }
    m_state = State::Completed;
    if (m_completionCallback) {
        m_completionCallback(result);
    }
}

void YosysExecutor::CloseHandles()
{
    if (m_hStdOutRead) { CloseHandle(m_hStdOutRead); m_hStdOutRead = nullptr; }
    if (m_hStdOutWrite) { CloseHandle(m_hStdOutWrite); m_hStdOutWrite = nullptr; }
    if (m_hStdErrRead) { CloseHandle(m_hStdErrRead); m_hStdErrRead = nullptr; }
    if (m_hStdErrWrite) { CloseHandle(m_hStdErrWrite); m_hStdErrWrite = nullptr; }
    if (m_hProcess) { CloseHandle(m_hProcess); m_hProcess = nullptr; }
    if (m_hJobObject) { CloseHandle(m_hJobObject); m_hJobObject = nullptr; }
}

void YosysExecutor::Cleanup()
{
    if (m_outputThread.joinable()) {
        m_outputThread.join();
    }
    CloseHandles();
    m_state = State::Idle;
}
