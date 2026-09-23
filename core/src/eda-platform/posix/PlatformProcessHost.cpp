#include <eda/api/process.hpp>

#include "ProcessCollector.h"

#if defined(_WIN32)
#error "eda-platform/posix backend compiled on a Windows target"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace eda {
namespace {

bool SetNonBlocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) >= 0;
}

void PumpDescriptors(int stdoutFd, int stderrFd, platform::ProcessCollector* collector,
                     const ProcessOutputCallback* onOutput, std::string* carries) {
    pollfd descriptors[2] = {{stdoutFd, POLLIN, 0}, {stderrFd, POLLIN, 0}};
    char buffer[4096];
    int openCount = 2;
    while (openCount > 0) {
        const int ready = poll(descriptors, 2, 200);
        if (ready <= 0) continue;
        for (int index = 0; index < 2; ++index) {
            if (descriptors[index].fd < 0) continue;
            if ((descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
            for (;;) {
                const ssize_t read = ::read(descriptors[index].fd, buffer, sizeof(buffer));
                if (read > 0) {
                    std::string chunk;
                    if (collector->Append(buffer, static_cast<std::size_t>(read), index == 1,
                                          chunk) &&
                        !chunk.empty() && onOutput != nullptr && *onOutput) {
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

class PosixProcessHost final : public IProcessHost {
public:
    ProcessResult Run(const ProcessSpec& spec, const ProcessOutputCallback& onOutput) override;
    void Cancel() override;

private:
    std::mutex handleMutex_;
    std::atomic<pid_t> pid_{-1};
    std::atomic<bool> cancelRequested_{false};
};

ProcessResult PosixProcessHost::Run(const ProcessSpec& spec,
                                    const ProcessOutputCallback& onOutput) {
    ProcessResult result;
    // 不清除 cancelRequested_：宿主可能在首次 Run 前已 Cancel（见 Win32 后端同处注释）。

    if (!spec.quoteArguments) {
        result.errorMessage = "Raw command line is not supported on POSIX.";
        return result;
    }

    // 所有字符串转换都在 fork 之前完成：fork 后的多线程子进程只允许 async-signal-safe 调用。
    const std::string executable = spec.executable.string();
    std::vector<std::string> argvStorage;
    argvStorage.reserve(spec.arguments.size() + 1);
    argvStorage.push_back(executable);
    for (const auto& argument : spec.arguments) {
        argvStorage.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(argvStorage.size() + 1);
    for (std::string& item : argvStorage) argv.push_back(item.data());
    argv.push_back(nullptr);

    std::vector<std::string> envStorage;
    std::vector<char*> envp;
    if (!spec.environment.empty()) {
        std::map<std::string, std::string> merged;
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            const std::string line(*entry);
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos || equals == 0) continue;
            merged[line.substr(0, equals)] = line.substr(equals + 1);
        }
        for (const auto& pair : spec.environment) {
            merged[pair.first] = pair.second;
        }
        envStorage.reserve(merged.size());
        for (const auto& pair : merged) envStorage.push_back(pair.first + "=" + pair.second);
        envp.reserve(envStorage.size() + 1);
        for (std::string& item : envStorage) envp.push_back(item.data());
        envp.push_back(nullptr);
    }

    const std::string workingDirectory = spec.workingDirectory.string();

    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
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
    // FD_CLOEXEC：避免写端跨 exec 泄漏给其它并发子进程，使对方读端永不 EOF。
    for (int fd : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]}) {
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
        if (!envp.empty()) {
            execvpe(argv[0], argv.data(), envp.data());
        } else {
            execvp(argv[0], argv.data());
        }
        _exit(127);
    }

    // 父进程也 setpgid，消除"子进程尚未建立进程组即 killpg"的竞态。
    setpgid(child, child);
    {
        std::lock_guard<std::mutex> lock(handleMutex_);
        pid_.store(child);
    }

    result.started = true;
    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    SetNonBlocking(stdoutPipe[0]);
    SetNonBlocking(stderrPipe[0]);

    platform::ProcessCollector collector(spec.maxOutputBytes);
    std::string carries[2];
    std::thread pump(PumpDescriptors, stdoutPipe[0], stderrPipe[0], &collector, &onOutput,
                     carries);

    const int timeoutMs = spec.timeoutSeconds > 0 ? spec.timeoutSeconds * 1000 : -1;
    int status = 0;
    bool finished = false;
    bool cancelled = false;
    int elapsedMs = 0;
    while (true) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            finished = true;
            break;
        }
        if (cancelRequested_.load()) {
            cancelled = true;
            break;
        }
        if (timeoutMs >= 0) {
            elapsedMs += 100;
            if (elapsedMs >= timeoutMs) break;
        }
        usleep(100000);
    }
    const bool timedOut = !finished && !cancelled;
    if (!finished) {
        // killpg 可能因进程组不存在(ESRCH)失败，退回 kill(pid)。
        if (killpg(child, SIGTERM) != 0) kill(child, SIGTERM);
        usleep(200000);
        if (killpg(child, SIGKILL) != 0) kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    pump.join();
    {
        std::lock_guard<std::mutex> lock(handleMutex_);
        pid_.store(-1);
    }

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }
    result.output = collector.Combined();
    result.errorOutput = collector.Errors();
    result.outputTruncated = collector.Truncated();

    if (cancelled) {
        result.outcome = ProcessOutcome::Cancelled;
    } else if (timedOut) {
        result.outcome = ProcessOutcome::TimedOut;
    } else if (result.exitCode == 0) {
        result.outcome = ProcessOutcome::Success;
    } else {
        result.outcome = ProcessOutcome::NonZeroExit;
    }
    return result;
}

void PosixProcessHost::Cancel() {
    cancelRequested_.store(true);
    const pid_t pid = pid_.load();
    if (pid > 0) {
        if (killpg(pid, SIGKILL) != 0) kill(pid, SIGKILL);
    }
}

} // namespace

std::unique_ptr<IProcessHost> CreatePlatformProcessHost() {
    return std::make_unique<PosixProcessHost>();
}

} // namespace eda
