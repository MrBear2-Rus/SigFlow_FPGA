// =============================================================================
// sigflow-tool —— SigFlow 的 C++ 工具进程入口
//
// 设计约束（见 README「具体架构」）：
//   * 无状态：进程不保存跨调用的任何状态，队列/并发/会话都在 Node 侧；
//   * 一次性：一个 job、一次查询、一次解析各起一个进程；
//   * 只读输入：只读 Node 显式传入的路径，只写 --work-dir 下的内容；
//   * 输出：stdout 每行一个 JSON（JSONL 事件），退出码表达结果。
//
// 本文件目前只有命令分发骨架：各子命令的实现随外层代码迁入后逐个接上。
// =============================================================================

#include <cstdio>
#include <cstring>
#include <string>

#ifndef SIGFLOW_TOOL_VERSION
#define SIGFLOW_TOOL_VERSION "0.0.0"
#endif

namespace {

struct CommandHelp {
    const char* name;
    const char* usage;
};

const CommandHelp kCommands[] = {
    {"run", "run --job-type <synth|pnr|pack|flash|sim.build|sim.run> "
            "--work-dir <dir> --params <json-file|->"},
    {"wave", "wave signals --vcd <file>\n"
             "       wave query   --vcd <file> --signals <a,b,...> --begin <t> --end <t>\n"
             "       wave serve   (stdin/stdout JSONL；每用户一个常驻子进程)"},
    {"structure", "structure --file <path>"},
    {"lint", "lint --file <path>"},
    {"validate", "validate cst  --cst <file> --pins <json>\n"
                 "       validate pins --design <json> --pins <json>"},
    {"target", "target show --id <profile-id>"},
    {"capture", "capture run --work-dir <dir> --contract <json>"},
    {"devices", "devices list"},
};

void PrintUsage(std::FILE* stream)
{
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
    std::fprintf(stream,
                 "\n"
                 "全局选项:\n"
                 "  -h, --help     显示本帮助\n"
                 "  --version      显示版本\n"
                 "\n"
                 "输出约定: stdout 每行一个 JSON 对象（type 字段区分事件），"
                 "退出码 0=成功 2=用法错误 3=命令尚未迁移 4=执行失败。\n");
}

const CommandHelp* FindCommand(const char* name)
{
    for (const CommandHelp& command : kCommands) {
        if (std::strcmp(command.name, name) == 0) {
            return &command;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
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

    // 迁移中：子命令实现随外层代码迁入后逐个接上。
    std::fprintf(stderr,
                 "sigflow-tool: 命令 '%s' 的实现尚未迁入 web/backend/cpp/src/cli\n",
                 command->name);
    return 3;
}
