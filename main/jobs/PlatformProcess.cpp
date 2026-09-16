#include "PlatformProcess.h"

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <cstdlib>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
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

// 把一块字节解码成文本，并正确处理"跨读取边界的多字节字符"与"非 UTF-8 输出"。
//
// 旧实现直接 wxString::FromUTF8(buffer, n)：一旦这 n 个字节不是合法 UTF-8
// （4096 字节的 read 很容易把 3 字节汉字劈成两半；Windows 上工具输出 CP936/GBK
// 时更是整块非法），FromUTF8 会**返回空串**，于是整个 4KB 输出被静默丢弃 ——
// 中文报错信息正好是用户最需要看到的部分。
//
// 这里做两件事：
//   1) 把末尾"被截断的多字节序列"（≤3 字节）留到下一次调用拼接，绝不丢字节；
//   2) 对真正非法的字节退化为 Latin-1 逐字节映射（不丢内容，只是编码不完美），
//      而不是丢掉整块。
wxString DecodeChunk(const char* buffer, std::size_t size, std::string& carry)
{
    std::string data;
    data.reserve(carry.size() + size);
    data += carry;
    data.append(buffer, size);
    carry.clear();
    if (data.empty()) return wxString();

    // 末尾连续"续字节"(10xxxxxx) 的个数，最多 3 个（UTF-8 序列最长 4 字节）。
    std::size_t trailing = 0;
    while (trailing < 3 && trailing < data.size() &&
           (static_cast<unsigned char>(data[data.size() - 1 - trailing]) & 0xC0) == 0x80) {
        ++trailing;
    }

    std::size_t keep = 0;
    if (trailing < data.size()) {
        const unsigned char lead =
            static_cast<unsigned char>(data[data.size() - 1 - trailing]);
        std::size_t need = 1;
        if ((lead & 0xE0) == 0xC0) need = 2;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xF8) == 0xF0) need = 4;
        if (trailing + 1 < need) keep = trailing + 1;   // 序列还没收全
    }

    const std::size_t usable = data.size() - keep;
    wxString text = usable == 0 ? wxString() : wxString::FromUTF8(data.data(), usable);

    if (usable > 0 && text.IsEmpty()) {
        // FromUTF8 对非法 UTF-8 返回空串 → 说明确实有非法字节（例如 Windows 下
        // 工具输出 CP936/GBK）。退化为 Latin-1 逐字节映射：内容不丢，只是编码不完美。
        // 必须用 From8BitData 而不是手工 wxUniChar(byte)——后者会按当前 locale 的
        // 多字节编码去解释单个字节，遇到 0xFF 之类会在 Debug 版触发 wx 断言。
        text = wxString::From8BitData(data.data(), usable);
    }

    if (keep > 0) carry.assign(data.data() + usable, keep);
    return text;
}

wxString StoreOutput(OutputCollector* collector, const char* buffer, std::size_t size,
                     bool isError, std::string& carry)
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

    wxString text = DecodeChunk(buffer, accepted, carry);
    collector->acceptedBytes += accepted;
    collector->events.push_back({ std::chrono::steady_clock::now(),
                                  collector->nextSequence++, isError, text });
    return text;
}

wxString MergeOutputWhere(OutputCollector* collector, bool isError)
{
    if (collector == nullptr) return wxString();
    std::lock_guard<std::mutex> lock(collector->mutex);
    std::stable_sort(collector->events.begin(), collector->events.end(),
        [](const OutputEvent& left, const OutputEvent& right) {
            if (left.timestamp != right.timestamp) return left.timestamp < right.timestamp;
            return left.sequence < right.sequence;
        });
    wxString output;
    for (const OutputEvent& event : collector->events) {
        if (event.isError == isError) output += event.text;
    }
    return output;
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
    wxString escaped;
    std::size_t backslashes = 0;
    for (wxChar character : value) {
        if (character == '\\') {
            ++backslashes;
            escaped += character;
            continue;
        }
        if (character == '"') {
            // 引号前的反斜杠全部翻倍，再补一个转义引号
            escaped += wxString(backslashes + 1, '\\');
            escaped += character;
        } else {
            escaped += character;
        }
        backslashes = 0;
    }
    // 结尾反斜杠翻倍，避免转义外层收尾引号
    escaped += wxString(backslashes, '\\');
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

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const
    {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

std::vector<wchar_t> BuildEnvironmentBlock(
    const std::vector<std::pair<wxString, wxString>>& overrides)
{
    if (overrides.empty()) return {};
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> merged;
    LPWCH parent = GetEnvironmentStringsW();
    if (parent != nullptr) {
        for (const wchar_t* entry = parent; *entry != L'\0'; entry += wcslen(entry) + 1) {
            const std::wstring line(entry);
            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring::npos || equals == 0) continue; // 跳过 "=C:" 之类
            merged[line.substr(0, equals)] = line.substr(equals + 1);
        }
        FreeEnvironmentStringsW(parent);
    }
    for (const auto& pair : overrides) {
        merged[pair.first.ToStdWstring()] = pair.second.ToStdWstring();
    }
    std::vector<wchar_t> block;
    for (const auto& pair : merged) {
        block.insert(block.end(), pair.first.begin(), pair.first.end());
        block.push_back(L'=');
        block.insert(block.end(), pair.second.begin(), pair.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void PumpStream(HANDLE handle, bool isErrorStream, OutputCollector* collector,
                const PlatformOutputCallback* onOutput, std::string* carry)
{
    char buffer[4096];
    while (true) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        const wxString chunk = StoreOutput(collector, buffer, read, isErrorStream, *carry);
        if (onOutput != nullptr && *onOutput && !chunk.IsEmpty()) {
            (*onOutput)(chunk, isErrorStream);
        }
    }
}

// 串行化进程创建。
// CreateProcessW(bInheritHandles=TRUE) 会继承父进程所有可继承句柄，而 Windows 没有
// "只继承这两个句柄"的简单开关（要 STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST）。
// 这里采用"建管道时不可继承、创建前临时打开两个写端继承位、创建后立刻关闭"的写法，
// 并用一把全局锁保证并发的 Run() 之间不会互相看到对方临时打开的可继承句柄。
std::mutex& ProcessCreateMutex()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace

PlatformProcessResult PlatformProcess::Run(const PlatformProcessRequest& request,
                                           const PlatformOutputCallback& onOutput)
{
    PlatformProcessResult result;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    // 先建成"不可继承"，只在 CreateProcessW 前后的互斥窗口里临时打开两个写端的继承位。
    // 原因：CreateProcessW(bInheritHandles=TRUE) 会继承父进程**所有**可继承句柄。
    // 若两个 Run() 并发（不同 Job 类型可并行），B 的子进程会拿到 A 的 stdout/stderr 写端，
    // 于是 A 的读端永远等不到 EOF，stdoutPump.join() 阻塞到 B 结束 ——
    // 表现为 A 任务一直卡在 Running，且 Cancel 也解不开。
    attributes.bInheritHandle = FALSE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0)) {
        result.errorMessage = "Unable to create job process pipes.";
        return result;
    }
    if (!CreatePipe(&stderrRead, &stderrWrite, &attributes, 0)) {
        // 第二根管道失败时必须关掉第一根，否则句柄泄漏。
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        result.errorMessage = "Unable to create job process pipes.";
        return result;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
    // STARTF_USESTDHANDLES 会把三个句柄全部替换掉；不给 stdin 的话子进程拿到的是无效句柄。
    HANDLE nulInput = CreateFileW(L"NUL", GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                  OPEN_EXISTING, 0, nullptr);
    startup.hStdInput = (nulInput != INVALID_HANDLE_VALUE) ? nulInput
                                                           : GetStdHandle(STD_INPUT_HANDLE);

    const wxString commandLine = request.quoteArguments
        ? BuildCommandLine(request.executable, request.arguments)
        : request.executable;
    std::vector<wchar_t> mutableCommand(commandLine.length() + 1);
    std::memcpy(mutableCommand.data(), commandLine.wc_str(),
                (commandLine.length() + 1) * sizeof(wchar_t));
    const wxString workingDirectory = request.workingDirectory.IsEmpty()
        ? wxString() : request.workingDirectory;
    std::vector<wchar_t> environmentBlock = BuildEnvironmentBlock(request.environment);

    PROCESS_INFORMATION processInfo{};
    BOOL created = FALSE;
    DWORD createError = 0;
    {
        // 进程创建必须串行化：否则"临时打开继承位"会被并发的 Run() 观察到，
        // 再次引入上面描述的写端泄漏。
        std::lock_guard<std::mutex> createLock(ProcessCreateMutex());
        SetHandleInformation(stdoutWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(stderrWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                 environmentBlock.empty() ? nullptr : environmentBlock.data(),
                                 workingDirectory.IsEmpty() ? nullptr : workingDirectory.wc_str(),
                                 &startup, &processInfo);
        if (!created) createError = GetLastError();
        SetHandleInformation(stdoutWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderrWrite, HANDLE_FLAG_INHERIT, 0);
    }
    if (nulInput != INVALID_HANDLE_VALUE) CloseHandle(nulInput);

    if (!created) {
        result.errorMessage = wxString::Format("Unable to start process (Win32 error %lu).",
                                               createError);
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
    // 每个流各自持有一份"跨读取边界的残余字节"，stdout/stderr 不能混用。
    std::string stdoutCarry;
    std::string stderrCarry;
    std::thread stdoutPump(PumpStream, stdoutRead, false, &collector, &onOutput, &stdoutCarry);
    std::thread stderrPump(PumpStream, stderrRead, true, &collector, &onOutput, &stderrCarry);

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
    if (request.onFinished) request.onFinished();
    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);
    CloseHandle(processInfo.hProcess);
    if (jobObject != nullptr) CloseHandle(jobObject);
    result.output = MergeOutput(&collector);
    result.errorOutput = MergeOutputWhere(&collector, true);
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
                     const PlatformOutputCallback* onOutput, std::string* carries)
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
                                                       static_cast<std::size_t>(read), index == 1,
                                                       carries[index]);
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
    if (!request.quoteArguments) {
        result.errorMessage = "Raw command line is not supported on POSIX.";
        return result;
    }

    // 所有 wxString → UTF-8 的转换都必须在 **fork 之前** 完成。
    // fork() 之后的多线程进程里，子进程只允许调用 async-signal-safe 函数；
    // 而 wxString::ToUTF8()/setenv() 都会分配内存并加锁。若 fork 瞬间别的线程正持有
    // 分配器锁，子进程会卡死在 exec 之前 —— 父进程只能等到超时再 SIGKILL。
    const std::string executable = std::string(request.executable.ToUTF8().data());
    std::vector<std::string> argvStorage;
    argvStorage.reserve(request.arguments.size() + 1);
    argvStorage.push_back(executable);
    for (const wxString& argument : request.arguments) {
        argvStorage.push_back(std::string(argument.ToUTF8().data()));
    }
    std::vector<char*> argv;
    argv.reserve(argvStorage.size() + 1);
    for (std::string& item : argvStorage) argv.push_back(item.data());
    argv.push_back(nullptr);

    // 环境变量同理：在父进程里合成 envp（environ + 覆盖项），子进程只调 execvpe。
    std::vector<std::string> envStorage;
    std::vector<char*> envp;
    if (!request.environment.empty()) {
        std::map<std::string, std::string> merged;
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            const std::string line(*entry);
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos || equals == 0) continue;
            merged[line.substr(0, equals)] = line.substr(equals + 1);
        }
        for (const auto& pair : request.environment) {
            merged[std::string(pair.first.ToUTF8().data())] =
                std::string(pair.second.ToUTF8().data());
        }
        envStorage.reserve(merged.size());
        for (const auto& pair : merged) envStorage.push_back(pair.first + "=" + pair.second);
        envp.reserve(envStorage.size() + 1);
        for (std::string& item : envStorage) envp.push_back(item.data());
        envp.push_back(nullptr);
    }

    const std::string workingDirectory = std::string(request.workingDirectory.ToUTF8().data());

    int stdoutPipe[2] = { -1, -1 };
    int stderrPipe[2] = { -1, -1 };
    const auto closePipe = [](int (&fds)[2]) {
        for (int fd : fds) {
            if (fd >= 0) close(fd);
        }
        fds[0] = fds[1] = -1;
    };
    if (pipe(stdoutPipe) != 0) {
        result.errorMessage = "Unable to create job process pipes.";
        return result;
    }
    if (pipe(stderrPipe) != 0) {
        closePipe(stdoutPipe);
        result.errorMessage = "Unable to create job process pipes.";
        return result;
    }
    // FD_CLOEXEC：否则写端会跨 exec 泄漏给其它并发子进程，
    // 使对方的读端永远等不到 EOF，pump.join() 挂住 —— 与 Windows 侧同一类问题。
    for (int fd : { stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1] }) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }

    const pid_t child = fork();
    if (child < 0) {
        closePipe(stdoutPipe);
        closePipe(stderrPipe);
        result.errorMessage = "Unable to fork a job process.";
        return result;
    }
    if (child == 0) {
        // 子进程：只调用 async-signal-safe 的函数。
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
        // execvpe 会搜索 PATH（execv 不会），同时能带上自定义环境。
        // 裸命令名（例如 SimToolchain 传的 "g++"）依赖这一点：用 execv 会 ENOENT/_exit(127)，
        // 表现为"编译器错误码 127、输出为空"。Windows 的 CreateProcessW 本来就搜索 PATH，
        // 所以这个坑只在 Linux/POSIX 出现。
        if (!envp.empty()) {
            execvpe(argv[0], argv.data(), envp.data());
        } else {
            execvp(argv[0], argv.data());
        }
        _exit(127);
    }

    // 父进程也要 setpgid：只靠子进程自己调用 setpgid(0,0) 存在竞态窗口，
    // 若在子进程执行前就 killpg，会因进程组尚不存在而失败(ESRCH)，
    // 于是 waitpid(...,0) 会一直阻塞到工具自然退出 —— 取消/超时看起来"没反应"。
    // 重复调用是幂等的；EACCES 表示子进程已经 exec，可安全忽略。
    setpgid(child, child);

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
    std::string carries[2];   // [0]=stdout, [1]=stderr
    std::thread pump(PumpDescriptors, stdoutPipe[0], stderrPipe[0], &collector, &onOutput, carries);

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
        // killpg 可能因为进程组尚未建立(ESRCH)或已消失而失败，
        // 此时退回 kill(pid) —— 否则 waitpid(...,0) 会一直阻塞到工具自然退出，
        // 用户看到的是"取消/超时没反应"。
        if (killpg(child, SIGTERM) != 0) kill(child, SIGTERM);
        usleep(200000);
        if (killpg(child, SIGKILL) != 0) kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    pump.join();
    if (request.onFinished) request.onFinished();

    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    result.output = MergeOutput(&collector);
    result.errorOutput = MergeOutputWhere(&collector, true);
    result.outputTruncated = collector.truncated;
    return result;
}

void PlatformProcess::Terminate(void* handle)
{
    if (handle == nullptr) return;
    const pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(handle));
    if (pid > 0) {
        // 同超时路径：先杀整个进程组（含子进程树），失败再退回单进程。
        if (killpg(pid, SIGKILL) != 0) kill(pid, SIGKILL);
    }
}

#endif
