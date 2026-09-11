#pragma once

#include <wx/string.h>

#include <functional>
#include <cstdint>
#include <vector>

// 平台抽象层：Job 层唯一允许触碰"进程"的地方。
// Windows 实现：CreateProcessW + 管道 + Job Object（超时/取消杀整棵进程树）。
// POSIX 实现：fork/exec + pipe + setpgid/killpg + waitpid（供 B 流 Linux 移植使用）。
struct PlatformProcessRequest {
    wxString executable;
    std::vector<wxString> arguments;
    wxString workingDirectory;
    int timeoutSeconds = 0; // <=0 表示不设超时
    std::uint64_t maxOutputBytes = 0; // 0 表示不限制；执行器默认设置上限
    std::uint64_t memoryLimitBytes = 0; // 0 表示不限制（Windows Job Object）
    // 进程创建成功后回调可终止句柄（Job Object / 进程组），供 Cancel 使用。
    std::function<void(void*)> onStarted;
};

struct PlatformProcessResult {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    wxString output; // stdout + stderr 合并
    wxString errorMessage;
    bool outputTruncated = false;
};

using PlatformOutputCallback = std::function<void(const wxString& chunk, bool isError)>;

class PlatformProcess {
public:
    // 同步执行；实时回调输出（可为空）；超时或 Terminate 时终止整棵进程树。
    static PlatformProcessResult Run(const PlatformProcessRequest& request,
                                     const PlatformOutputCallback& onOutput = nullptr);

    // 终止一个由 Run 登记的可终止句柄（Windows 为 Job Object 句柄，POSIX 为进程组 id）。
    static void Terminate(void* handle);

    // 命令行组装：仅在必要时加引号，避免把 "/c" 之类的开关也套上引号。
    static wxString QuoteArgument(const wxString& value);
    static wxString BuildCommandLine(const wxString& executable,
                                     const std::vector<wxString>& arguments);
};
