#include <eda/api/process.hpp>

#include "ProcessCollector.h"

#if !defined(_WIN32)
#error "eda-platform/win32 backend compiled on a non-Windows target"
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eda {
namespace {

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), size);
    return out;
}

bool NeedsQuoting(const std::wstring& value) {
    if (value.empty()) return true;
    for (wchar_t c : value) {
        if (c == L' ' || c == L'\t' || c == L'"') return true;
    }
    return false;
}

std::wstring QuoteArgument(const std::wstring& value) {
    if (!NeedsQuoting(value)) return value;
    std::wstring escaped;
    std::size_t backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
            escaped += c;
            continue;
        }
        if (c == L'"') {
            escaped.append(backslashes + 1, L'\\');
            escaped += c;
        } else {
            escaped += c;
        }
        backslashes = 0;
    }
    escaped.append(backslashes, L'\\');
    return L"\"" + escaped + L"\"";
}

std::wstring BuildCommandLine(const std::wstring& executable,
                              const std::vector<std::wstring>& arguments) {
    std::wstring line = QuoteArgument(executable);
    for (const auto& argument : arguments) {
        line += L" " + QuoteArgument(argument);
    }
    return line;
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

std::vector<wchar_t> BuildEnvironmentBlock(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    if (overrides.empty()) return {};
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> merged;
    LPWCH parent = GetEnvironmentStringsW();
    if (parent != nullptr) {
        for (const wchar_t* entry = parent; *entry != L'\0'; entry += wcslen(entry) + 1) {
            const std::wstring line(entry);
            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring::npos || equals == 0) continue;
            merged[line.substr(0, equals)] = line.substr(equals + 1);
        }
        FreeEnvironmentStringsW(parent);
    }
    for (const auto& pair : overrides) {
        merged[Utf8ToWide(pair.first)] = Utf8ToWide(pair.second);
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

// 串行化进程创建：CreateProcessW(bInheritHandles=TRUE) 会继承父进程所有可继承句柄，
// 并发的 Run() 之间若不互斥，会把对方的管道写端继承过去，导致对方读端永不 EOF。
std::mutex& ProcessCreateMutex() {
    static std::mutex mutex;
    return mutex;
}

void PumpStream(HANDLE handle, bool isError, platform::ProcessCollector* collector,
                const ProcessOutputCallback* onOutput) {
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        std::string chunk;
        if (collector->Append(buffer, read, isError, chunk) && !chunk.empty() &&
            onOutput != nullptr && *onOutput) {
            (*onOutput)(chunk, isError);
        }
    }
}

class Win32ProcessHost final : public IProcessHost {
public:
    ProcessResult Run(const ProcessSpec& spec, const ProcessOutputCallback& onOutput) override;
    void Cancel() override;

private:
    std::mutex handleMutex_;
    HANDLE job_ = nullptr;
    std::atomic<bool> cancelRequested_{false};
};

ProcessResult Win32ProcessHost::Run(const ProcessSpec& spec,
                                    const ProcessOutputCallback& onOutput) {
    ProcessResult result;
    // 注意：不在此处清除 cancelRequested_。宿主可能在本 host 首次 Run 之前就已 Cancel
    // （CoreJobContext::processHost 的取消竞态），清除会让取消丢失。host 按"每 Job 一个"使用。

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
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
    HANDLE nulInput = CreateFileW(L"NUL", GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                  OPEN_EXISTING, 0, nullptr);
    startup.hStdInput = (nulInput != INVALID_HANDLE_VALUE) ? nulInput
                                                           : GetStdHandle(STD_INPUT_HANDLE);

    const std::wstring executable = Utf8ToWide(spec.executable.string());
    std::vector<std::wstring> arguments;
    arguments.reserve(spec.arguments.size());
    for (const auto& argument : spec.arguments) {
        arguments.push_back(Utf8ToWide(argument));
    }
    std::wstring commandLine =
        spec.quoteArguments ? BuildCommandLine(executable, arguments) : executable;
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const std::wstring workingDirectory =
        spec.workingDirectory.empty() ? std::wstring() : Utf8ToWide(spec.workingDirectory.string());
    std::vector<wchar_t> environmentBlock = BuildEnvironmentBlock(spec.environment);

    PROCESS_INFORMATION processInfo{};
    BOOL created = FALSE;
    DWORD createError = 0;
    {
        std::lock_guard<std::mutex> createLock(ProcessCreateMutex());
        SetHandleInformation(stdoutWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(stderrWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                 environmentBlock.empty() ? nullptr : environmentBlock.data(),
                                 workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                 &startup, &processInfo);
        if (!created) createError = GetLastError();
        SetHandleInformation(stdoutWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderrWrite, HANDLE_FLAG_INHERIT, 0);
    }
    if (nulInput != INVALID_HANDLE_VALUE) CloseHandle(nulInput);

    if (!created) {
        result.errorMessage = "Unable to start process (Win32 error " +
                              std::to_string(createError) + ").";
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);
        return result;
    }
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(job, processInfo.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lock(handleMutex_);
        job_ = job;
    }

    ResumeThread(processInfo.hThread);
    CloseHandle(processInfo.hThread);
    result.started = true;

    platform::ProcessCollector collector(spec.maxOutputBytes);
    std::thread stdoutPump(PumpStream, stdoutRead, false, &collector, &onOutput);
    std::thread stderrPump(PumpStream, stderrRead, true, &collector, &onOutput);

    const DWORD timeoutMs = spec.timeoutSeconds > 0
        ? static_cast<DWORD>(spec.timeoutSeconds) * 1000u : INFINITE;
    const ULONGLONG start = GetTickCount64();
    bool timedOut = false;
    for (;;) {
        if (cancelRequested_.load()) break;
        DWORD slice = 100;
        if (timeoutMs != INFINITE) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) {
                timedOut = true;
                break;
            }
            slice = static_cast<DWORD>(std::min<ULONGLONG>(100, timeoutMs - elapsed));
        }
        if (WaitForSingleObject(processInfo.hProcess, slice) == WAIT_OBJECT_0) break;
    }

    const bool cancelled = cancelRequested_.load();
    if (timedOut || cancelled) {
        if (job != nullptr) {
            TerminateJobObject(job, 1460);
        } else {
            TerminateProcess(processInfo.hProcess, 1460);
        }
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
    {
        std::lock_guard<std::mutex> lock(handleMutex_);
        job_ = nullptr;
    }
    if (job != nullptr) CloseHandle(job);

    result.output = collector.Combined();
    result.errorOutput = collector.Errors();
    result.outputTruncated = collector.Truncated();

    if (cancelled) {
        result.outcome = ProcessOutcome::Cancelled;
    } else if (timedOut) {
        result.outcome = ProcessOutcome::TimedOut;
    } else if (exitCode == 0) {
        result.outcome = ProcessOutcome::Success;
    } else {
        result.outcome = ProcessOutcome::NonZeroExit;
    }
    return result;
}

void Win32ProcessHost::Cancel() {
    cancelRequested_.store(true);
    HANDLE job = nullptr;
    {
        std::lock_guard<std::mutex> lock(handleMutex_);
        job = job_;
    }
    if (job != nullptr) {
        TerminateJobObject(job, 1);
    }
}

} // namespace

std::unique_ptr<IProcessHost> CreatePlatformProcessHost() {
    return std::make_unique<Win32ProcessHost>();
}

} // namespace eda
