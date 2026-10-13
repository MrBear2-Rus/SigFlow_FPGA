#include "VerilatorSimulator.h"

#include "Platform.h"
#include "eda-core/Toolchain.h"

#include <eda/api/abi.h>
#include <eda/api/plugin_registry.h>
#include <eda/api/process.hpp>
#include <eda/api/toolchain.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace eda {
namespace sim {
namespace {

std::string ParamString(const Json& params, const char* key, const std::string& fallback = {}) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return fallback;
}

bool RegularNonEmptyFile(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > 0;
}

// `verilator --binary --trace` generates an automatic `--main` harness when
// no C++ source is supplied.  That harness enables tracing but never opens a
// trace file, so sim.run can succeed without producing a waveform.  Supply a
// deliberately small, generated harness that owns a VCD destination instead.
bool WriteTraceMain(const std::filesystem::path& path, const std::string& topModule,
                    std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        error = "unable to create generated simulation main: " + platform::PathToUtf8(path);
        return false;
    }
    file << "#include \"verilated.h\"\n"
            "#include \"verilated_vcd_c.h\"\n"
            "#include \"V" << topModule << ".h\"\n"
            "#include <filesystem>\n\n"
            "double sc_time_stamp() { return 0.0; }\n\n"
            "int main(int argc, char** argv) {\n"
            "  VerilatedContext context;\n"
            "  context.commandArgs(argc, argv);\n"
            "  context.traceEverOn(true);\n"
            "  V" << topModule << " top{&context, \"TOP\"};\n"
            "  VerilatedVcdC trace;\n"
            "  top.trace(&trace, 99);\n"
            "  std::filesystem::create_directories(\"waveform\");\n"
            "  trace.open(\"waveform/wave.vcd\");\n"
            "  top.eval();\n"
            "  trace.dump(context.time());\n"
            "  top.final();\n"
            "  trace.close();\n"
            "  return 0;\n"
            "}\n";
    file.close();
    if (!file) {
        error = "unable to write generated simulation main: " + platform::PathToUtf8(path);
        return false;
    }
    return true;
}

// 按 PATH 环境变量展开目录（去空白、去成对引号，语义与 DefaultToolchain 一致）。
std::vector<std::filesystem::path> PathDirectories() {
    std::vector<std::filesystem::path> directories;
    const std::string value = platform::EnvUtf8("PATH");
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t position = value.find(platform::PathListSeparator(), start);
        std::string entry =
            value.substr(start, position == std::string::npos ? std::string::npos : position - start);
        const std::size_t first = entry.find_first_not_of(" \t");
        const std::size_t last = entry.find_last_not_of(" \t");
        entry = first == std::string::npos ? std::string() : entry.substr(first, last - first + 1);
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        if (!entry.empty()) directories.push_back(platform::PathFromUtf8(entry));
        if (position == std::string::npos) break;
        start = position + 1;
    }
    return directories;
}

// Makefile/shell 只认正斜杠；反斜杠路径会被 sh 当转义吞掉。
std::string SlashPath(const std::filesystem::path& path) {
    std::string utf8 = platform::PathToUtf8(path);
    std::replace(utf8.begin(), utf8.end(), '\\', '/');
    return utf8;
}

// "存在"不等于"可用"：Windows 商店的 python3 别名是 0 字节占位程序，
// 必须实跑一次才算数。
bool ProbeCommand(IProcessHost& host, const std::filesystem::path& executable) {
    if (executable.empty()) return false;
    ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {"--version"};
    spec.timeoutSeconds = 20;
    const ProcessResult result = host.Run(spec, nullptr);
    return result.started && result.outcome == ProcessOutcome::Success && result.exitCode == 0;
}

bool ProbeShell(IProcessHost& host, const std::filesystem::path& executable) {
    if (executable.empty()) return false;
    ProcessSpec spec;
    spec.executable = executable;
    // A usable shell is the hard requirement.  `uname` is queried by the
    // upstream makefile, but an unavailable uname only produces a harmless
    // warning there; it must not turn a workable shell into a false negative.
    spec.arguments = {"-c", "exit 0"};
    spec.timeoutSeconds = 20;
    const ProcessResult result = host.Run(spec, nullptr);
    return result.started && result.outcome == ProcessOutcome::Success && result.exitCode == 0;
}

// 上游 verilated.mk 固定写 PYTHON3 = python3，环境变量的同名值会被 Makefile
// 覆盖。Windows 上把已验证的 python.exe 映射成 Job 私有的 python3.exe，前置到
// 子进程 PATH，避免修改第三方 runtime 或依赖 Windows Store 别名。
bool MaterializePython3Shim(const std::filesystem::path& python,
                            const std::filesystem::path& jobDir,
                            std::filesystem::path& shimDirectory,
                            std::string& errorMessage) {
#if defined(_WIN32)
    shimDirectory = jobDir / "toolchain" / "python3";
    const std::filesystem::path shim = shimDirectory / "python3.exe";
    std::error_code error;
    std::filesystem::create_directories(shimDirectory, error);
    if (error) {
        errorMessage = "unable to create Python shim directory: " + error.message();
        return false;
    }
    std::filesystem::remove(shim, error);
    error.clear();
    std::filesystem::create_hard_link(python, shim, error);
    if (error) {
        // Python 与 Job 根可能不在同一卷，或源文件系统不支持硬链接；此时复制即可。
        error.clear();
        std::filesystem::copy_file(python, shim, std::filesystem::copy_options::overwrite_existing,
                                   error);
    }
    if (error || !RegularNonEmptyFile(shim)) {
        errorMessage = error ? "unable to create python3 shim: " + error.message()
                             : "python3 shim is empty";
        return false;
    }
    return true;
#else
    (void)jobDir;
    if (python.filename() != "python3") {
        errorMessage = "a python3 command is required";
        return false;
    }
    shimDirectory = python.parent_path();
    return true;
#endif
}

void PrependPath(std::vector<std::filesystem::path>& directories,
                 const std::filesystem::path& directory) {
    if (directory.empty()) return;
    for (const auto& existing : directories) {
        std::error_code error;
        if (std::filesystem::equivalent(existing, directory, error) && !error) return;
    }
    directories.push_back(directory);
}

std::vector<std::filesystem::path> PythonCandidates(const std::filesystem::path& verilatorExe) {
    std::vector<std::filesystem::path> candidates;
    auto Push = [&candidates](const std::filesystem::path& path) {
        if (path.empty()) return;
        for (const auto& seen : candidates) {
            std::error_code error;
            if (std::filesystem::equivalent(seen, path, error) && !error) return;
        }
        candidates.push_back(path);
    };
    const std::string suffix = platform::ExecutableSuffix();
    for (const char* variable : {"SIGFLOW_PYTHON3", "SIGFLOW_PYTHON", "PYTHON3"}) {
        const std::string value = platform::EnvUtf8(variable);
        if (!value.empty()) Push(platform::PathFromUtf8(value));
    }
    for (const auto& directory : PathDirectories()) {
        Push(directory / ("python3" + suffix));
        Push(directory / ("python" + suffix));
    }
    // 随包 runtime 的 sibling（apicula 携带的嵌入式 Python）。
    const std::filesystem::path runtimeRoot =
        verilatorExe.parent_path().parent_path().parent_path();
    Push(runtimeRoot / "apicula" / "Scripts" / ("python" + suffix));
    return candidates;
}

// POSIX shell 目录发现：SIGFLOW_SH（文件或目录）→ 常见安装目录 → PATH。
// 特意只认 sh：Windows 自带的 C:\windows\system32\bash.exe 是 WSL 引导 stub，
// 绝不能被当成 make 用的 POSIX shell。
std::filesystem::path FindPosixShellDirectory(const std::string& suffix) {
    const std::string configuredValue = platform::EnvUtf8("SIGFLOW_SH");
    if (!configuredValue.empty()) {
        const std::filesystem::path configured = platform::PathFromUtf8(configuredValue);
        if (RegularNonEmptyFile(configured)) return configured.parent_path();
        std::error_code error;
        if (std::filesystem::is_directory(configured, error) && !error) return configured;
    }
    std::vector<std::filesystem::path> directories = {
        std::filesystem::path("C:/msys64/usr/bin"),
        std::filesystem::path("C:/msys2/usr/bin"),
        std::filesystem::path("C:/msys/usr/bin"),
        std::filesystem::path("C:/Git/usr/bin"),
        std::filesystem::path("C:/Program Files/Git/usr/bin"),
        std::filesystem::path("C:/Program Files (x86)/Git/usr/bin"),
    };
    const std::string localAppData = platform::EnvUtf8("LOCALAPPDATA");
    if (!localAppData.empty()) {
        directories.push_back(platform::PathFromUtf8(localAppData) / "Programs" / "Git" /
                              "usr" / "bin");
    }
    for (const auto& directory : PathDirectories()) {
        directories.push_back(directory);
        // Git for Windows normally contributes <git>/cmd to PATH, not
        // <git>/usr/bin where sh.exe lives.  Derive that sibling instead of
        // forcing every user to set SIGFLOW_SH (and without accepting WSL's
        // bash launcher as a build shell).
        const std::filesystem::path parent = directory.parent_path();
        if (directory.filename() == "cmd" &&
            (parent.filename() == "Git" || parent.filename() == "git")) {
            directories.push_back(parent / "usr" / "bin");
        }
    }
    for (const auto& directory : directories) {
        if (RegularNonEmptyFile(directory / ("sh" + suffix))) return directory;
    }
    return {};
}

struct PreflightOutcome {
    std::vector<std::pair<std::string, std::string>> environment;
    bool ready = false;
    std::string failures;
};

// --binary/--build 默认路径上的全部外部前置：
//   make、g++（verilator 内部调 make，链接还要编译器）、
//   python3（verilated.mk 的 VERILATOR_INCLUDER）、POSIX sh（verilated.mk 归档规则）。
// 缺 python3/sh 时能注入就注入（将真实 Python 映射为 job 私有 python3.exe，shell 显式指定），
// 注入不了就地报"指名道姓"的错误，而不是让用户对着 exit code 9009 猜。
PreflightOutcome RunVerilatorPreflight(JobContext& ctx, const std::filesystem::path& verilatorExe) {
    IProcessHost& host = ctx.processHost();
    PreflightOutcome outcome;
    outcome.ready = true;
    Json metrics = Json::object();
    const std::string suffix = platform::ExecutableSuffix();
    std::vector<std::filesystem::path> pathPrefixes;

    if (!ProbeCommand(host, verilatorExe)) {
        metrics["verilator"] = "unusable";
        outcome.ready = false;
        outcome.failures +=
            "verilator: 无法执行 --version（Windows 版 verilator_bin 还依赖 perl）。\n"
            "    请检查 Verilator runtime 与 Perl 是否完整，或设置 SIGFLOW_VERILATOR 指向可运行版本。\n";
    } else {
        metrics["verilator"] = platform::PathToUtf8(verilatorExe);
    }

    for (const char* name : {"make", "g++"}) {
        ToolQuery query;
        query.name = name;
        query.searchPath = true;
        const ToolResolution resolution = DefaultToolchain().Resolve(query);
        if (resolution.found && ProbeCommand(host, resolution.path)) {
            metrics[name] = platform::PathToUtf8(resolution.path);
            continue;
        }
        metrics[name] = resolution.found ? "unusable" : "missing";
        outcome.ready = false;
        outcome.failures +=
            std::string(name) + (resolution.found ? ": 找到但无法执行。" : ": 未找到。") +
            resolution.reason + "\n    请安装 MinGW-w64（含 make/g++）并把其 bin 目录加入 PATH。\n";
    }

    std::filesystem::path python;
    std::string pythonDeclines;
    for (const auto& candidate : PythonCandidates(verilatorExe)) {
        if (!RegularNonEmptyFile(candidate)) {
            pythonDeclines += platform::PathToUtf8(candidate) + "（0 字节占位或缺失） ";
            continue;
        }
        if (!ProbeCommand(host, candidate)) {
            pythonDeclines += platform::PathToUtf8(candidate) + "（--version 执行失败） ";
            continue;
        }
        python = candidate;
        break;
    }
    if (!python.empty()) {
        std::filesystem::path pythonShimDirectory;
        std::string shimError;
        if (MaterializePython3Shim(python, ctx.jobDir(), pythonShimDirectory, shimError)) {
            PrependPath(pathPrefixes, pythonShimDirectory);
            // Keep the real interpreter's directory visible as well.  Some
            // Windows Python distributions keep python3*.dll beside python.exe.
            PrependPath(pathPrefixes, python.parent_path());
            metrics["python3"] = SlashPath(pythonShimDirectory /
                                             ("python3" + suffix));
        } else {
            outcome.ready = false;
            outcome.failures += "python3: 已找到可运行 Python，但无法建立 job 私有 python3 命令：" +
                                shimError + "\n";
        }
    } else {
        ctx.log("preflight: python 候选全部不可用：" + pythonDeclines, true);
        outcome.ready = false;
        outcome.failures +=
            "python3: 未找到可运行的 Python。\n"
            "    Verilator 的 makefile 经 python3 调 verilator_includer（合并 V*_ALL.cpp）。\n"
            "    Windows 商店的 python3 别名是 0 字节占位程序：请在 设置>应用>高级应用设置>\n"
            "    应用执行别名 中关闭，或安装 Python 并让 PATH 命中真实 python.exe，\n"
            "    或设置环境变量 SIGFLOW_PYTHON3 指向真实 python.exe。\n";
    }

    const std::filesystem::path shellDirectory = FindPosixShellDirectory(suffix);
    const std::filesystem::path shell = shellDirectory / ("sh" + suffix);
    if (!shellDirectory.empty() && ProbeShell(host, shell)) {
        // 不只前置 PATH：显式 SHELL 避免 Windows GNU Make 回退到 cmd.exe，
        // 后者不能解析 verilated.mk 中的 `if test ...; then ...; fi`。
        PrependPath(pathPrefixes, shellDirectory);
        outcome.environment.emplace_back("SHELL", SlashPath(shell));
        metrics["sh"] = SlashPath(shell);
        metrics["uname"] = "optional (provided by shell PATH when available)";
    } else {
        outcome.ready = false;
        outcome.failures +=
            "sh: 未找到 POSIX shell。\n"
            "    Verilator 生成的 makefile 需要 sh.exe（if test/mkdir/cat/xargs 等 POSIX 语法），\n"
            "    缺失时 make 以 cmd.exe 兜底并把上述语法报成 Error 9009。\n"
            "    请安装 Git for Windows 或 MSYS2 并把 usr/bin 加入 PATH，\n"
            "    或设置环境变量 SIGFLOW_SH 指向真实 sh.exe。\n";
    }

    if (!pathPrefixes.empty()) {
        std::string pathValue;
        for (const auto& directory : pathPrefixes) {
            if (!pathValue.empty()) pathValue.push_back(platform::PathListSeparator());
            pathValue += platform::PathToUtf8(directory);
        }
        const std::string parentPath = platform::EnvUtf8("PATH");
        if (!parentPath.empty()) {
            if (!pathValue.empty()) pathValue.push_back(platform::PathListSeparator());
            pathValue += parentPath;
        }
        outcome.environment.emplace_back("PATH", pathValue);
    }

    const Json report{{"preflight", metrics}};
    ctx.emitMetric(report);
    if (outcome.ready) {
        std::string summary = "preflight: 前置条件就绪:";
        for (const auto& entry : metrics.items()) {
            summary += " " + entry.key() + "=" +
                       (entry.value().is_string() ? entry.value().get<std::string>() : "");
        }
        ctx.log(summary, false);
    } else {
        ctx.log(outcome.failures, true);
    }
    return outcome;
}

} // namespace

bool CheckVerilatorToolchain(const std::filesystem::path& executable, std::string& reason) {
    auto host = CreatePlatformProcessHost();
    auto probe = [&](const std::filesystem::path& file, bool shell = false) {
        if (!RegularNonEmptyFile(file)) return false;
        ProcessSpec spec; spec.executable = file;
        spec.arguments = shell ? std::vector<std::string>{"-c", "exit 0"} : std::vector<std::string>{"--version"};
        spec.timeoutSeconds = 1; spec.maxOutputBytes = 8192;
        return host->Run(spec, {}).outcome == ProcessOutcome::Success;
    };
    if (!probe(executable)) { reason = "verilator cannot run --version (check Perl/runtime)"; return false; }
    for (const char* name : {"make", "g++"}) {
        ToolQuery query; query.name = name;
        auto found = DefaultToolchain().Resolve(query);
        if (!found.found || !probe(found.path)) { reason = std::string(name) + " is missing or unusable"; return false; }
    }
    bool python = false;
    for (const auto& candidate : PythonCandidates(executable)) if (probe(candidate)) { python = true; break; }
    if (!python) { reason = "Python for verilator_includer is missing or unusable"; return false; }
    const auto shellDirectory = FindPosixShellDirectory(platform::ExecutableSuffix());
    if (shellDirectory.empty() || !probe(shellDirectory / ("sh" + std::string(platform::ExecutableSuffix())), true)) {
        reason = "a working POSIX sh is required (SIGFLOW_SH, Git for Windows or MSYS2)"; return false;
    }
    reason.clear(); return true;
}

Json VerilatorSimulator::paramsSchema() const {
    return Json{
        {"type", "object"},
        {"required", {"top_module", "source_files"}},
        {"properties", {
            {"top_module", {{"type", "string"}}},
            {"source_files", {{"type", "array"}}},
            {"testbench", {{"type", "string"}}},
            {"out_dir", {{"type", "string"}}},
            {"executable", {{"type", "string"}}},
            {"verilator_path", {{"type", "string"}}},
        }},
    };
}

Json VerilatorSimulator::resultSchema() const {
    return Json{{"type", "object"}, {"properties", {{"executable", {{"type", "string"}}}}}};
}

Error VerilatorSimulator::startJob(const JobRequest& request, JobContext& ctx) {
    const Json& params = request.params;
    const std::string topModule = ParamString(params, "top_module");
    if (topModule.empty()) {
        return Error{ErrorCode::InvalidArgument, "sim.build requires top_module", ""};
    }
    std::vector<std::string> sourceFiles;
    if (params.is_object() && params.contains("source_files") &&
        params["source_files"].is_array()) {
        for (const auto& entry : params["source_files"]) {
            if (entry.is_string()) sourceFiles.push_back(entry.get<std::string>());
        }
    }
    if (sourceFiles.empty()) {
        return Error{ErrorCode::InvalidArgument, "sim.build requires at least one source file", ""};
    }
    const std::string testbench = ParamString(params, "testbench");

    std::filesystem::path outDir = platform::PathFromUtf8(ParamString(params, "out_dir"));
    if (outDir.empty()) outDir = ctx.jobDir() / "sim";
    std::error_code dirError;
    std::filesystem::create_directories(outDir, dirError);

    std::string executable = ParamString(params, "executable");
    if (executable.empty()) {
        executable = platform::PathToUtf8(
            outDir / ("sim_main" + std::string(platform::ExecutableSuffix())));
    }
    const std::filesystem::path traceMain = outDir / "sigflow_trace_main.cpp";
    std::string traceMainError;
    if (!WriteTraceMain(traceMain, topModule, traceMainError)) {
        return Error{ErrorCode::Internal, traceMainError, ""};
    }

    std::string verilatorPath = ParamString(params, "verilator_path");
    if (verilatorPath.empty()) {
        ToolQuery query;
        query.name = "verilator_bin";
        query.alternativeNames = {"verilator_bin_dbg", "verilator"};
        query.environmentVariables = {"VERILATOR_BIN", "SIGFLOW_VERILATOR"};
        query.searchPath = true;
        const ToolResolution resolution = DefaultToolchain().Resolve(query);
        if (!resolution.found) {
            return Error{ErrorCode::NotFound, "verilator not found: " + resolution.reason, ""};
        }
        verilatorPath = platform::PathToUtf8(resolution.path);
    }

    const PreflightOutcome preflight =
        RunVerilatorPreflight(ctx, platform::PathFromUtf8(verilatorPath));
    if (!preflight.ready) {
        // hint 字段不会被 JobReport 归一化收编；详细缺项必须随 message 原样进报告，
        // 这样学生/老师能直接看到"缺哪样、装什么"，而不是 exit code。
        return Error{ErrorCode::NotFound, "verilator 前置条件不满足:\n" + preflight.failures, ""};
    }
    ctx.progress(5, "preflight passed");

    ProcessSpec spec;
    spec.environment = preflight.environment;
    spec.executable = verilatorPath;
    // Do not use --binary here: it is a shortcut that forcibly adds
    // Verilator's autogenerated --main, which conflicts with our trace main
    // and (without it) never writes a VCD.  The explicit equivalent keeps
    // the build fully native while making the produced waveform deterministic.
    spec.arguments = {"--cc", "--exe", "--build", "--trace", "-Wno-fatal", "--top-module",
                      topModule};
    if (!testbench.empty()) spec.arguments.push_back(testbench);
    for (const std::string& source : sourceFiles) spec.arguments.push_back(source);
    // Supplying this C++ file prevents Verilator from falling back to its
    // no-waveform autogenerated --main harness.
    spec.arguments.push_back(platform::PathToUtf8(traceMain));
    spec.arguments.push_back("-o");
    spec.arguments.push_back(executable);
    spec.workingDirectory = outDir;
    const ProcessResult process = ctx.processHost().Run(
        spec, [&ctx](const std::string& line, bool isError) { ctx.log(line, isError); });

    ctx.emitMetric(Json{{"exit_code", process.exitCode}});
    if (process.outcome == ProcessOutcome::Cancelled) {
        return Error{ErrorCode::Cancelled, "simulation build cancelled", ""};
    }
    if (process.outcome == ProcessOutcome::TimedOut) {
        return Error{ErrorCode::TimedOut, "simulation build timed out", ""};
    }
    if (!process.started) {
        return Error{ErrorCode::Crashed, process.errorMessage, ""};
    }
    if (process.exitCode != 0) {
        return Error{ErrorCode::Internal,
                     "verilator exited with code " + std::to_string(process.exitCode), ""};
    }

    std::error_code existsError;
    if (!std::filesystem::exists(platform::PathFromUtf8(executable), existsError)) {
        return Error{ErrorCode::Internal,
                     "verilator did not produce the expected executable: " + executable, ""};
    }

    Artifact artifact;
    artifact.id = "sim-executable";
    artifact.path = executable;
    artifact.schema = "eda.sim.verilator-executable.v1";
    artifact.role = "primary";
    ctx.registerArtifact(artifact);
    ctx.progress(100, "simulation build complete");
    return Error::Ok();
}

void VerilatorSimulator::invoke(const MethodCall& call, Callback<Error, Json> onReply) {
    onReply(Error{ErrorCode::Unsupported, "method not supported via invoke: " + call.method, ""},
            Json::object());
}

std::uint64_t VerilatorSimulator::subscribe(const std::string&, EventHandler<const Json&>) {
    return 0;
}

void VerilatorSimulator::unsubscribe(std::uint64_t) {}

PluginInfo VerilatorSimulator::info() const {
    PluginInfo info;
    info.id = "eda-sim-verilator";
    info.version = "1.0.0";
    info.displayName = "Verilator Simulator";
    info.vendor = "SigFlow";
    info.location = "inner";
    info.runtime = "inprocess";
    info.capabilities = {"sim/verilator"};
    info.methods = {"sim_build"};
    info.abi = EDA_PLUGIN_ABI_VERSION;
    return info;
}

} // namespace sim
} // namespace eda

EDA_REGISTER_PLUGIN(VerilatorSimulator, eda::sim::VerilatorSimulator, "eda-sim-verilator");
