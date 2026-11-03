// =============================================================================
// sigflow-tool —— SigFlow 的 C++ 工具进程入口
//
// 设计约束（见 README「具体架构」）：
//   * 无状态：进程不保存跨调用的任何状态，队列/并发/会话都在 Node 侧；
//   * 一次性：一个 job、一次查询、一次解析各起一个进程；
//   * 只读输入：只读 Node 显式传入的路径，只写 --work-dir 下的内容；
//   * 输出：stdout 每行一个 JSON（JSONL 事件），退出码表达结果。
//
// run 已接单任务 Runtime；工具 provider 随后续迁移接入静态工厂。
// =============================================================================

#include <cstdio>
#include <cstring>
#include <string>
#include "eda-core/RunCommand.h"
#ifndef SIGFLOW_TEST_PROVIDER
#include "plugins/synth-yosys/YosysProvider.h"
#include "plugins/pnr-nextpnr/NextpnrProvider.h"
#include "plugins/pack-gowin/GowinPackProvider.h"
#include "plugins/sim-verilator/VerilatorProviders.h"
#include "plugins/cst-gowin-cst/BoardCommands.h"
#include "analysis/AnalysisCommand.h"
#endif
#include <csignal>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#ifdef SIGFLOW_TEST_PROVIDER
#include "RuntimeFixtureProvider.h"
#endif

#ifndef SIGFLOW_TOOL_VERSION
#define SIGFLOW_TOOL_VERSION "0.0.0"
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
std::atomic<bool> outputFailed{false};
void OnSignal(int signal) {
    interrupted = signal;
}
void Emit(const eda::Json& event) {
    if (outputFailed)
        throw std::runtime_error("JSONL output already failed");
    const auto data = event.dump(-1, ' ', false, eda::Json::error_handler_t::replace) + "\n";
    size_t offset = 0;
    auto started = std::chrono::steady_clock::now();
    while (offset < data.size()) {
        auto n = ::write(STDOUT_FILENO, data.data() + offset, data.size() - offset);
        if (n > 0) {
            offset += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
            pollfd descriptor{STDOUT_FILENO, POLLOUT, 0};
            poll(&descriptor, 1, 50);
            continue;
        }
        outputFailed = true;
        throw std::runtime_error("JSONL output channel closed or blocked");
    }
}

struct CommandHelp {
    const char* name;
    const char* usage;
};

const CommandHelp kCommands[] = {
    {"run",
     "run --job-type <synth|pnr|pack|flash|sim.build|sim.run> "
     "--work-dir <dir> --job-id <id> --project <dir> --project-id <id> --params <json-file|->"},
    {"wave", "wave signals --vcd <file>\n"
             "       wave query   --vcd <file> --signals <a,b,...> --begin <t> --end <t>\n"
             "       wave serve   (stdin/stdout JSONL；每用户一个常驻子进程)"},
    {"structure", "structure --file <path>"},
    {"lint", "lint --file <path> --project-root <dir>"},
    {"validate", "validate cst  --cst <file> --pins <json>\n"
                 "       validate pins --design <json> --pins <json> --netlist <json>"},
    {"target", "target show --id <profile-id>"},
    {"capture", "capture run --work-dir <dir> --contract <json>"},
    {"devices", "devices list"},
};

void PrintUsage(std::FILE* stream) {
    std::fprintf(stream,
                 "sigflow-tool %s —— SigFlow C++ 工具进程\n"
                 "\n"
                 "用法: sigflow-tool <command> [options]\n"
                 "\n"
                 "命令:\n",
                 SIGFLOW_TOOL_VERSION);
    for (const CommandHelp& command : kCommands) {
        std::fprintf(stream, "  %s\n", command.usage);
    }
    std::fprintf(stream, "\n"
                         "全局选项:\n"
                         "  -h, --help     显示本帮助\n"
                         "  --version      显示版本\n"
                         "\n"
                         "输出约定: stdout 每行一个 JSON 对象（type 字段区分事件），"
                         "退出码 0=成功 2=用法错误 3=命令尚未迁移 4=执行失败。\n");
}

const CommandHelp* FindCommand(const char* name) {
    for (const CommandHelp& command : kCommands) {
        if (std::strcmp(command.name, name) == 0) {
            return &command;
        }
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(stderr);
        return 2;
    }

    const std::string first = argv[1];
    if (first == "-h" || first == "--help") {
        PrintUsage(stdout);
        return 0;
    }
    if (first == "--version") {
        std::printf("sigflow-tool %s\n", SIGFLOW_TOOL_VERSION);
        return 0;
    }

    const CommandHelp* command = FindCommand(argv[1]);
    if (command == nullptr) {
        std::fprintf(stderr, "sigflow-tool: 未知命令 '%s'\n\n", argv[1]);
        PrintUsage(stderr);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);
    struct sigaction action {};
    action.sa_handler = OnSignal;
    sigemptyset(&action.sa_mask);
    // Interrupt a blocked --params stdin read as well as running providers.
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    int flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags >= 0)
        fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
    if (first == "run" && argc == 3 &&
        (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
        PrintUsage(stdout);
        std::fprintf(
            stdout,
            "\nrun options: --timeout-sec <nonnegative int> --max-output-bytes <uint64>\n"
            "             --data-dir <path> --tool-root <path>\n"
            "Node must create an empty Job directory before submission. params limit: 2 MiB.\n");
        return 0;
    }
    if (first == "run") {
        try {
            auto factory = [](const std::string& type) -> std::unique_ptr<eda::IJobProvider> {
#ifdef SIGFLOW_TEST_PROVIDER
                if (type == "synth")
                    return std::make_unique<RuntimeFixtureProvider>();
#else
                if (type == "synth")
                    return std::make_unique<eda::synth::YosysProvider>();
                if (type == "pnr")
                    return std::make_unique<eda::pnr::NextpnrProvider>();
                if (type == "pack")
                    return std::make_unique<eda::pack::GowinPackProvider>();
                if (type == "sim.build")
                    return std::make_unique<eda::sim::BuildProvider>();
                if (type == "sim.run")
                    return std::make_unique<eda::sim::RunProvider>();
#endif
                return {};
            };
            const int result = eda::web::RunCommand(
                argc, argv, factory, Emit,
                [] {
                    pollfd channel{STDOUT_FILENO, POLLOUT, 0};
                    poll(&channel, 1, 0);
                    return interrupted != 0 || (channel.revents & (POLLERR | POLLHUP | POLLNVAL));
                },
                SIGFLOW_DEFAULT_DATA_DIR, SIGFLOW_TOOL_RUNTIME_DIR);
            if (interrupted) {
                auto signal = interrupted;
                std::signal(signal, SIG_DFL);
                std::raise(signal);
            }
            return result;
        } catch (const std::invalid_argument& e) {
            if (interrupted) {
                auto signal = interrupted;
                std::signal(signal, SIG_DFL);
                std::raise(signal);
            }
            try {
                Emit(eda::Json{{"type", "diagnostic"},
                               {"severity", "error"},
                               {"code", "INVALID_ARGUMENT"},
                               {"message", e.what()},
                               {"file", ""},
                               {"line", 0}});
            } catch (...) {
            }
            return 2;
        } catch (const std::exception& e) {
            if (interrupted) {
                auto signal = interrupted;
                std::signal(signal, SIG_DFL);
                std::raise(signal);
            }
            try {
                Emit(eda::Json{{"type", "diagnostic"},
                               {"severity", "error"},
                               {"code", "EXECUTION_FAILED"},
                               {"message", e.what()},
                               {"file", ""},
                               {"line", 0}});
            } catch (...) {
            }
            return 4;
        }
    }
#ifndef SIGFLOW_TEST_PROVIDER
    if (first == "structure" || first == "lint")
        return eda::analysis::AnalysisCommand(argc, argv, Emit);
    if (first == "target" || first == "validate")
        return eda::web::BoardCommand(argc, argv, Emit, SIGFLOW_DEFAULT_DATA_DIR);
#endif
    try {
        Emit(eda::Json{{"type", "diagnostic"},
                       {"severity", "error"},
                       {"code", "NOT_IMPLEMENTED"},
                       {"message", "command not migrated: " + first},
                       {"file", ""},
                       {"line", 0}});
    } catch (...) {
    }
    return 3;
}
