#include "PlatformProcess.h"

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
extern char** environ;
#endif

namespace {

struct OutputEvent {
    std::chrono::steady_clock::time_point timestamp;
    std::uint64_t sequence = 0;
    bool isError = false;
    wxString text;
};

struct OutputCollector {
    std::mutex mutex;
    std::vector<OutputEvent> events;
    std::uint64_t acceptedBytes = 0;
    std::uint64_t maxBytes = 0;
    std::uint64_t nextSequence = 0;
    bool truncated = false;
};

wxString StoreOutput(OutputCollector* collector, const char* buffer, std::size_t size,
                     bool isError)
{
    if (collector == nullptr || buffer == nullptr || size == 0) return wxString();
    std::lock_guard<std::mutex> lock(collector->mutex);
    const std::uint64_t remaining = collector->maxBytes == 0 ||
        collector->acceptedBytes >= collector->maxBytes
        ? 0
        : collector->maxBytes - collector->acceptedBytes;
    const std::size_t accepted = collector->maxBytes == 0
        ? size
        : static_cast<std::size_t>(std::min<std::uint64_t>(remaining, size));
    if (accepted < size) collector->truncated = true;
    if (accepted == 0) return wxString();

    wxString text = wxString::FromUTF8(buffer, accepted);
    collector->acceptedBytes += accepted;
    collector->events.push_back({ std::chrono::steady_clock::now(),
                                  collector->nextSequence++, isError, text });
    return text;
}

wxString MergeOutput(OutputCollector* collector)
{
    if (collector == nullptr) return wxString();
    std::lock_guard<std::mutex> lock(collector->mutex);
    std::stable_sort(collector->events.begin(), collector->events.end(),
        [](const OutputEvent& left, const OutputEvent& right) {
            if (left.timestamp != right.timestamp) return left.timestamp < right.timestamp;
            return left.sequence < right.sequence;
        });
    wxString output;
    for (const OutputEvent& event : collector->events) output += event.text;
    return output;
}

bool NeedsQuoting(const wxString& value)
{
    if (value.IsEmpty()) return true;
    for (wxChar character : value) {
        if (character == ' ' || character == '\t' || character == '"') return true;
    }
    return false;
}

} // namespace

wxString PlatformProcess::QuoteArgument(const wxString& value)
{
    if (!NeedsQuoting(value)) return value;
    wxString escaped = value;
    escaped.Replace("\\", "\\\\");
    escaped.Replace("\"", "\\\"");
    return "\"" + escaped + "\"";
}

wxString PlatformProcess::BuildCommandLine(const wxString& executable,
                                           const std::vector<wxString>& arguments)
{
    wxString commandLine = QuoteArgument(executable);
    for (const wxString& argument : arguments) commandLine += " " + QuoteArgument(argument);
    return commandLine;
}

#ifdef _WIN32

namespace {

void PumpStream(HANDLE handle, bool isErrorStream, OutputCollector* collector,
                const PlatformOutputCallback* onOutput)
{
    char buffer[4096];
    while (true) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        const wxString chunk = StoreOutput(collector, buffer, read, isErrorStream);
        if (onOutput != nullptr && *onOutput && !chunk.IsEmpty()) {
            (*onOutput)(chunk, isErrorStream);
        }
    }
}

} // namespace

PlatformProcessResult PlatformProcess::Run(const PlatformProcessRequest& request,
                                           const PlatformOutputCallback& onOutput)
{
    PlatformProcessResult result;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0) ||
        !CreatePipe(&stderrRead, &stderrWrite, &attributes, 0)) {
        result.errorMessage = "Unable to create job process pipes.";
        return result;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;

    const wxString commandLine = BuildCommandLine(request.executable, request.arguments);
    std::vector<wchar_t> mutableCommand(commandLine.length() + 1);
    std::memcpy(mutableCommand.data(), commandLine.wc_str(),
                (commandLine.length() + 1) * sizeof(wchar_t));
    const wxString workingDirectory = request.workingDirectory.IsEmpty()
        ? wxString() : request.workingDirectory;

    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        workingDirectory.IsEmpty() ? nullptr : workingDirectory.wc_str(),
                        &startup, &processInfo)) {
        result.errorMessage = wxString::Format("Unable to start process (Win32 error %lu).",
                                               GetLastError());
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);
        return result;
    }
    result.started = true;
    CloseHandle(processInfo.hThread);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);

    HANDLE jobObject = CreateJobObjectW(nullptr, nullptr);
    if (jobObject != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (request.memoryLimitBytes > 0) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limits.ProcessMemoryLimit = static_cast<SIZE_T>(request.memoryLimitBytes);
        }
        SetInformationJobObject(jobObject, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
        if (!AssignProcessToJobObject(jobObject, processInfo.hProcess)) {
            CloseHandle(jobObject);
            jobObject = nullptr;
        }
    }
    if (request.onStarted) {
        request.onStarted(jobObject != nullptr ? static_cast<void*>(jobObject)
                                               : static_cast<void*>(processInfo.hProcess));
    }

    OutputCollector collector;
    collector.maxBytes = request.maxOutputBytes;
    std::thread stdoutPump(PumpStream, stdoutRead, false, &collector, &onOutput);
    std::thread stderrPump(PumpStream, stderrRead, true, &collector, &onOutput);

    const DWORD timeout = request.timeoutSeconds > 0
        ? static_cast<DWORD>(request.timeoutSeconds) * 1000u : INFINITE;
    if (WaitForSingleObject(processInfo.hProcess, timeout) == WAIT_TIMEOUT) {
        result.timedOut = true;
        if (jobObject != nullptr) TerminateJobObject(jobObject, 1460);
        else TerminateProcess(processInfo.hProcess, 1460);
        WaitForSingleObject(processInfo.hProcess, 5000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    stdoutPump.join();
    stderrPump.join();
    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);
    CloseHandle(processInfo.hProcess);
    if (jobObject != nullptr) CloseHandle(jobObject);
    result.output = MergeOutput(&collector);
    result.outputTruncated = collector.truncated;
    return result;
}

void PlatformProcess::Terminate(void* handle)
{
    if (handle == nullptr) return;
    HANDLE target = static_cast<HANDLE>(handle);
    if (!TerminateJobObject(target, 1)) TerminateProcess(target, 1);
}

#else // POSIX 实现（供 Linux/麒麟移植；当前仓库无 Linux 构建，尚未在真机编译验证）

namespace {

bool SetNonBlocking(int descriptor)
{
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) >= 0;
}

void PumpDescriptors(int stdoutFd, int stderrFd, OutputCollector* collector,
                     const PlatformOutputCallback* onOutput)
{
    pollfd descriptors[2] = { { stdoutFd, POLLIN, 0 }, { stderrFd, POLLIN, 0 } };
    char buffer[4096];
    int openCount = 2;
    while (openCount > 0) {
        const int ready = poll(descriptors, 2, 200);
        if (ready <= 0) continue;
        for (int index = 0; index < 2; ++index) {
            if (descriptors[index].fd < 0) continue;
            if ((descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
            while (true) {
                const ssize_t read = ::read(descriptors[index].fd, buffer, sizeof(buffer));
                if (read > 0) {
                    const wxString chunk = StoreOutput(collector, buffer,
                                                       static_cast<std::size_t>(read), index == 1);
                    if (onOutput != nullptr && *onOutput && !chunk.IsEmpty()) {
                        (*onOutput)(chunk, index == 1);
                    }
                    continue;
                }
                if (read == 0 || (read < 0 && errno != EAGAIN && errno != EINTR)) {
                    close(descriptors[index].fd);
                    descriptors[index].fd = -1;
                    --openCount;
                }
                break;
            }
        }
    }
}

} // namespace

PlatformProcessResult PlatformProcess::Run(const PlatformProcessRequest& request,
                                           const PlatformOutputCallback& onOutput)
{
    PlatformProcessResult result;
    int stdoutPipe[2] = { -1, -1 };
    int stderrPipe[2] = { -1, -1 };
    if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        result.errorMessage = "Unable to create job process pipes.";
        return result;
    }

    const std::string executable = request.executable.utf8_string();
    std::vector<std::string> storage;
    storage.push_back(executable);
    for (const wxString& argument : request.arguments) storage.push_back(argument.utf8_string());
    std::vector<char*> argv;
    for (std::string& item : storage) argv.push_back(item.data());
    argv.push_back(nullptr);

    const std::string workingDirectory = request.workingDirectory.utf8_string();
    const pid_t child = fork();
    if (child < 0) {
        result.errorMessage = "Unable to fork a job process.";
        return result;
    }
    if (child == 0) {
        setpgid(0, 0);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        if (!workingDirectory.empty()) {
            if (chdir(workingDirectory.c_str()) != 0) _exit(127);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }

    result.started = true;
    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    SetNonBlocking(stdoutPipe[0]);
    SetNonBlocking(stderrPipe[0]);
    if (request.onStarted) {
        request.onStarted(reinterpret_cast<void*>(static_cast<intptr_t>(child)));
    }

    OutputCollector collector;
    collector.maxBytes = request.maxOutputBytes;
    std::thread pump(PumpDescriptors, stdoutPipe[0], stderrPipe[0], &collector, &onOutput);

    const int timeoutMs = request.timeoutSeconds > 0 ? request.timeoutSeconds * 1000 : -1;
    int status = 0;
    bool finished = false;
    int elapsedMs = 0;
    while (true) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            finished = true;
            break;
        }
        if (timeoutMs >= 0) {
            elapsedMs += 100;
            if (elapsedMs >= timeoutMs) break;
        }
        usleep(100000);
    }
    if (!finished) {
        result.timedOut = true;
        killpg(child, SIGTERM);
        usleep(200000);
        killpg(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    pump.join();

    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    result.output = MergeOutput(&collector);
    result.outputTruncated = collector.truncated;
    return result;
}

void PlatformProcess::Terminate(void* handle)
{
    if (handle == nullptr) return;
    const pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(handle));
    if (pid > 0) killpg(pid, SIGKILL);
}

#endif
