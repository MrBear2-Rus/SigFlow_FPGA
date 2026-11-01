#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Types.h"

namespace eda {

enum class ProcessOutcome {
    Success,
    NonZeroExit,
    Cancelled,
    TimedOut,
    LaunchFailed,
};

struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    int timeoutSeconds = 0;                 // <=0：不设超时
    std::uint64_t maxOutputBytes = 0;       // 0：不限制
    bool quoteArguments = true;             // false：executable 字段当完整命令行原样传递
    std::vector<std::pair<std::string, std::string>> environment; // 覆盖项
};

struct ProcessResult {
    ProcessOutcome outcome = ProcessOutcome::LaunchFailed;
    bool started = false;
    int exitCode = -1;
    std::string output;         // stdout + stderr（按到达顺序合并）
    std::string errorOutput;    // 仅 stderr
    bool outputTruncated = false;
    std::string errorMessage;
};

using ProcessOutputCallback = std::function<void(const std::string& chunk, bool isError)>;

// 统一进程执行入口：同步执行（应在 worker 线程调用），输出流式回调；
// 超时/取消时终止整棵进程树。Cancel 可从其它线程调用。
class IProcessHost {
public:
    virtual ~IProcessHost() = default;

    virtual ProcessResult Run(const ProcessSpec& spec,
                              const ProcessOutputCallback& onOutput) = 0;
    virtual void Cancel() = 0;
};

// 平台实现工厂：Windows = CreateProcessW + Job Object；POSIX = fork/exec + setpgid/killpg。
std::unique_ptr<IProcessHost> CreatePlatformProcessHost();

} // namespace eda
