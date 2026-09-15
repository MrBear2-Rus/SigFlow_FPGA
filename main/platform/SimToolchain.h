#pragma once

#include <string>
#include <vector>
#include <functional>

#include "../jobs/PlatformProcess.h"   // PlatformOutputCallback

namespace sigflow::platform {

// 仿真编译策略：Windows = vcvars64.bat + cl + .bat 脚本；POSIX = g++ 直接编译。
// 引擎只负责编排（缓存目录、进度、清理、日志），平台差异全部收在这里。
class SimToolchain {
public:
    struct SharedLibRequest {
        std::string cacheDir;             // 输出/工作目录（UTF-8）
        std::string objDir;               // Verilator --Mdir
        std::string outputPath;           // <top>.dll / <top>.so
        std::string stubSourcePath;       // sc_time_stub.cpp
        std::string verilatorIncludeDir;  // verilated*.h/.cpp 目录
        std::vector<std::string> extraSources;   // 额外 .cpp（如 sim_main.cpp），可空
        std::string workingDirectory;     // 子进程初始工作目录（UTF-8），可空
        std::function<void(void*)> onStarted;
        std::function<void()> onFinished;
    };
    struct ExecutableRequest {
        std::string cacheDir;
        std::string objDir;
        std::string outputPath;           // sim_runner.exe / sim_runner
        std::string mainSourcePath;       // sim_main.cpp
        std::string stubSourcePath;
        std::string verilatorIncludeDir;
        std::string workingDirectory;     // 子进程初始工作目录（UTF-8），可空
        std::function<void(void*)> onStarted;
        std::function<void()> onFinished;
    };

    // 生成并执行编译；返回是否成功。日志通过 sink 流式回传（可选）。
    static bool CompileSharedLibrary(const SharedLibRequest& req,
                                     const PlatformOutputCallback& sink,
                                     std::string& error);
    static bool CompileExecutable(const ExecutableRequest& req,
                                  const PlatformOutputCallback& sink,
                                  std::string& error);
};

} // namespace sigflow::platform
