// SF-03：真实 Yosys / Verilator 经插件与 Gateway 的端到端验证。
//
// 与 edu_gateway_smoke 的区别：这里不用 Fake IJobService，而是真的把官方插件
// （eda-synth-yosys / eda-sim-verilator）注册进 CoreJobService，跑真实二进制，
// 再通过 Gateway 的受控提交流程观察结果。因此它验证的是"真实工具链路"，
// 而不是"Gateway 的接线形状"。
//
// 工具不存在时**跳过**并明确打印 SKIP，绝不把跳过当通过（spec：不能用 Mock 替代真实工具验收）。
#include "eda-agent-gateway/GatewayServer.h"
#include "eda-agent-gateway/ReportNormalizer.h"   // JobStateName

#include <eda/api/Types.h>
#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>

#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
#include "eda-core/Toolchain.h"
#include "eda-platform/Platform.h"
#include "VcdWaveformBackend.h"

#include <httplib.h>

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// 与 Composer 同款：静态库中的自注册 TU 若无外部引用会被链接器丢弃，
// 导致 StaticPluginRegistry 为空、RegisterBuiltins() 返回 0。
// 这里显式引用官方插件的 force-link 符号，保证真实 provider 进入注册表。
extern "C" void eda_plugin_force_link_YosysSynthesizer();
extern "C" void eda_plugin_force_link_VerilatorSimulator();
extern "C" void eda_plugin_force_link_VerilatorRunJob();

void ForceLinkOfficialPlugins() {
    eda_plugin_force_link_YosysSynthesizer();
    eda_plugin_force_link_VerilatorSimulator();
    eda_plugin_force_link_VerilatorRunJob();
}

int g_failures = 0;
int g_skipped = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

void Skip(const std::string& what) {
    ++g_skipped;
    std::cout << "  SKIP: " << what << "\n";
}

template <typename F>
bool WaitUntil(F predicate, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

// 在仓库内查找随附的真实工具；找不到返回空。
//
// 按平台给候选名：POSIX 上**不得**接受 Windows 的 `.exe`。原因有两条：
//   1) WSL 的互操作层会把 PE 二进制当成可执行文件跑起来，于是"Linux 验证"实际跑的是
//      Windows 工具，得到的是假通过/假失败，污染跨平台结论；
//   2) 原生 Linux 的随附 runtime 里就是没有扩展名的 `yosys` / `verilator_bin`，
//      只认 `.exe` 会让这个测试在 Linux 上永远 SKIP，等于 Linux 侧没有真实验收。
std::filesystem::path FindTool(const std::string& relativeDir,
                               const std::vector<std::string>& candidates) {
    const std::filesystem::path root = SIGFLOW_REPO_ROOT;
    std::error_code ec;
    for (const std::string& name : candidates) {
        const std::filesystem::path candidate = root / relativeDir / name;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) return candidate;
    }
    return {};
}

std::filesystem::path FindPathCommand(const std::string& name) {
    eda::ToolQuery query;
    query.name = name;
    query.searchPath = true;
    const eda::ToolResolution result = eda::DefaultToolchain().Resolve(query);
    return result.found ? result.path : std::filesystem::path();
}

// 真实工具由测试从仓库的受控 runtime 目录选定；只写入本测试进程的环境，
// 不把可执行路径开放给 Agent 请求参数。插件仍经既有 Toolchain 的环境变量发现流程运行。
bool SetToolEnvironment(const char* name, const std::filesystem::path& executable) {
    if (executable.empty()) return false;
    const std::string value = eda::platform::PathToUtf8(executable);
#if defined(_WIN32)
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

// 判断某个命令是否**真的可用**，而不只是"文件存在"。
// Windows 上 `python3`、`python` 常被解析到 Microsoft Store 的 0 字节占位程序：
// 它存在、会被 Toolchain 判定为 found，但执行必然失败（cmd 报 9009）。
// 因此这里实际运行一次并检查退出码，避免把"看起来有"当成"能用"。
bool CommandRuns(const std::filesystem::path& executable, const std::vector<std::string>& args) {
    if (executable.empty()) return false;
    eda::ProcessSpec spec;
    spec.executable = eda::platform::PathToUtf8(executable);
    spec.arguments = args;
    const eda::ProcessResult result = eda::CreatePlatformProcessHost()->Run(spec, nullptr);
    return result.outcome == eda::ProcessOutcome::Success && result.exitCode == 0;
}

// 在 PATH 上找命令，并确认它真的能执行（`<name> --version`）。不可用则返回空。
std::filesystem::path FindWorkingCommand(const std::string& name) {
    const std::filesystem::path found = FindPathCommand(name);
    if (found.empty()) return {};
    if (!CommandRuns(found, {"--version"})) return {};
    return found;
}

eda::Json GetJson(int port, const std::string& path, const std::string& token, int& status) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(30, 0);
    httplib::Headers headers;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    auto res = client.Get(path.c_str(), headers);
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

eda::Json PostJson(int port, const std::string& path, const std::string& token,
                   const eda::Json& body, int& status, const httplib::Headers& extra) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(30, 0);
    httplib::Headers headers = extra;
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    auto res = client.Post(path.c_str(), headers, body.dump(), "application/json");
    if (!res) {
        status = -1;
        return eda::Json::object();
    }
    status = res->status;
    try {
        return eda::Json::parse(res->body);
    } catch (const std::exception&) {
        return eda::Json::object();
    }
}

// 一个可被 Yosys Gowin 映射与 Verilator 共同接受的最小时序样例。不能用教学用
// latch 例子：目标 gw1n 不支持该锁存器原语，失败会掩盖本测试要验证的真实工具链路。
const char* kRegisterRtl = R"RTL(module top (
    input  wire clk,
    input  wire en,
    input  wire d,
    output reg  q
);
    always @(posedge clk) begin
        if (en) q <= d;
    end
endmodule
)RTL";

} // namespace

int main() {
    namespace fs = std::filesystem;
    std::error_code cleanupError;

    // 工具发现按平台区分：
    //   Windows：仓库随附的 external/fpga-tools/runtime（*.exe）。
    //   POSIX  ：**只用系统 PATH 上的原生工具**。仓库随附的 runtime 是 Windows 载荷
    //            （.exe，以及指向 .exe 的包装脚本）；WSL 的互操作层会把它们跑起来，
    //            于是"Linux 验证"实际执行的是 Windows 工具，得到假通过/假失败，
    //            这正是项目约束里禁止的"用跨平台二进制顶替原生验收"。
    //            原生 Linux 缺工具时显式 SKIP，绝不用 Windows 二进制冒充。
#ifdef _WIN32
    const std::string exeSuffix = eda::platform::ExecutableSuffix();
    const fs::path yosys =
        FindTool("external/fpga-tools/runtime/yosys/bin", {"yosys" + exeSuffix});
    const fs::path verilator = FindTool("external/fpga-tools/runtime/verilator/bin",
                                        {"verilator_bin" + exeSuffix});
    const std::string discoveryNote =
        "Windows bundled runtime external/fpga-tools/runtime/{yosys,verilator}/bin";
#else
    const fs::path verilatorBin = FindPathCommand("verilator_bin");
    const fs::path yosys = FindPathCommand("yosys");
    const fs::path verilator = !verilatorBin.empty() ? verilatorBin : FindPathCommand("verilator");
    const std::string discoveryNote =
        "POSIX system PATH only (the bundled runtime is a Windows payload)";
#endif
    // `verilator --binary` 把生成的构建委托给两个字面命令：
    //   1) `python3`（verilator_includer 脚本）
    //   2) POSIX shell：verilated.mk 的归档规则用的是 `if test ... then ... else`
    // 两者都不是"文件存在即可用"——Store 占位程序存在但必然失败，
    // Windows 上也可能根本没有 sh.exe。任一缺失都显式 SKIP，
    // 绝不把"没跑"计作 sim.build 通过，也不去跑一个注定失败的 Job。
    const fs::path python3 = FindWorkingCommand("python3");
    const fs::path posixShell = !FindWorkingCommand("sh").empty() ? FindWorkingCommand("sh")
                                                                  : FindWorkingCommand("bash");
    const fs::path python3OnPath = FindPathCommand("python3");
    std::cout << "  info: yosys=" << (yosys.empty() ? "(missing)" : yosys.string()) << "\n";
    std::cout << "  info: verilator=" << (verilator.empty() ? "(missing)" : verilator.string())
              << "\n";
    std::cout << "  info: tool discovery = " << discoveryNote << "\n";
    std::cout << "  info: python3=" << (python3.empty() ? "(missing or not runnable)" : python3.string())
              << "\n";
    if (python3.empty() && !python3OnPath.empty()) {
        std::cout << "  info: (python3 on PATH is " << python3OnPath.string()
                  << " but cannot execute -- likely a Microsoft Store alias stub)\n";
    }
    std::cout << "  info: posix shell for make="
              << (posixShell.empty() ? "(missing)" : posixShell.string()) << "\n";

    // 回归保护：PATH 上存在 python3 时，必须能明确区分"可运行"与"不可运行"。
    // 若解析到的 python3 实际执行失败，它就不应被当成可用前置条件。
    if (!python3OnPath.empty() && python3.empty()) {
        Check(!CommandRuns(python3OnPath, {"--version"}),
              "python3 on PATH is reported unusable only when it really cannot execute");
    } else if (!python3OnPath.empty()) {
        Check(CommandRuns(python3OnPath, {"--version"}),
              "python3 accepted as runnable really executes --version");
    }
    if (!yosys.empty()) {
        Check(SetToolEnvironment("SIGFLOW_YOSYS", yosys),
              "real Yosys runtime is supplied through the controlled tool environment");
    }
    if (!verilator.empty()) {
        Check(SetToolEnvironment("SIGFLOW_VERILATOR", verilator),
              "real Verilator runtime is supplied through the controlled tool environment");
    }

    const fs::path root = fs::temp_directory_path() /
                          ("sigflow_real_tools_" + std::to_string(
                              std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fs::remove_all(root, cleanupError);
    const std::string projectName = "proj 真实 工具";
    const fs::path projectRoot = root / eda::platform::PathFromUtf8(projectName);
    Check(eda::platform::PathToUtf8(projectRoot.filename()) == projectName,
          "UTF-8 project name round-trips through filesystem path");
    const std::string nonBmpName = "path-\xF0\x9F\xA7\xAA";
    Check(eda::platform::PathToUtf8(eda::platform::PathFromUtf8(nonBmpName)) == nonBmpName,
          "UTF-8 non-BMP path component round-trips");
    fs::create_directories(projectRoot / "rtl");
    Check(fs::is_directory(projectRoot / "rtl"), "UTF-8 project directory is created at its round-trip path");
    {
        std::ofstream rtl(projectRoot / "rtl" / "latch.v", std::ios::binary);
        rtl << kRegisterRtl;
    }
    {
        std::ofstream manifest(projectRoot / "sigflow.project", std::ios::binary);
        manifest << R"JSON({
  "build": {"top_module": ["top"]},
  "paths": {"source_files": ["rtl/latch.v"]},
  "fpga": {"target_profile": "test-target", "yosys_strategy": "baseline"}
})JSON";
    }

    // 真实 Job 服务（宿主同款）：CoreJobService + 平台进程宿主。
    eda::CoreJobService jobService([]() { return eda::CreatePlatformProcessHost(); },
                                   root / "jobs");
    // 失败时把真实工具输出留在 CTest 日志中；临时目录仍可在测试结束后清理，
    // 避免为了诊断而长期残留带工程源码的目录。
    jobService.SetLogSink([](const std::string& line, bool isError) {
        std::cout << "      tool" << (isError ? "[err]" : "") << ": " << line << "\n";
    });

    // 与 Composer 同款：把编译期注册的官方插件拉到内存并注册其 IJobProvider，
    // 这样 Job 才会走真实 Yosys/Verilator 实现，而不是"没有 provider"。
    eda::PluginHost pluginHost;
    ForceLinkOfficialPlugins();
    const std::size_t builtins = pluginHost.RegisterBuiltins();
    std::cout << "  info: register builtins=" << builtins << "\n";
    for (const auto& record : pluginHost.Records()) {
        if (record.status != eda::PluginStatus::Ready) continue;
        eda::IPluginInteraction* instance = pluginHost.Get(record.id);
        if (instance == nullptr) continue;
        if (auto* provider = dynamic_cast<eda::IJobProvider*>(instance)) {
            jobService.RegisterProvider(
                std::shared_ptr<eda::IJobProvider>(provider, [](eda::IJobProvider*) {}));
        }
    }

    // 只有真实注册成功的 provider 才算"就绪能力"（以 IJobProvider::jobType 为准，
    // 与 Composer::ReadyPlugins 的判定一致，不用装饰性的 "synth/yosys" 串）。
    bool synthProviderReady = false;
    bool simBuildProviderReady = false;
    std::vector<eda::agent::ReadyPlugin> providerReady;
    for (const auto& record : pluginHost.Records()) {
        if (record.status != eda::PluginStatus::Ready) continue;
        eda::IPluginInteraction* instance = pluginHost.Get(record.id);
        if (instance == nullptr) continue;
        auto* provider = dynamic_cast<eda::IJobProvider*>(instance);
        if (provider == nullptr) continue;
        const std::string jobType = provider->jobType();
        if (jobType == "synth") synthProviderReady = true;
        if (jobType == "sim.build") simBuildProviderReady = true;
        std::cout << "  info: provider " << record.id << " jobType=" << jobType << "\n";
    }
    std::cout << "  info: synth provider=" << (synthProviderReady ? "ready" : "missing")
              << " sim.build provider=" << (simBuildProviderReady ? "ready" : "missing") << "\n";

    eda::agent::GatewayConfig config;
    config.instanceId = "inst-real";
    config.edition = "edu";
    config.token = "real-token";
    config.uiToken = "real-ui-token";
    config.port = 0;

    // 关键：上报给 Gateway 的 ReadyPlugin.capabilities 必须是 **jobType** 字符串
    // （synth / sim.build / sim.run），与 Composer::ReadyPlugins 的判定口径一致。
    // AD-12：同时镜像生产的"工具可执行性"口径——插件在册但工具缺失时标记 ready=false，
    // 能力必须报为不可用并给出可读原因（否则这条断言在缺工具的环境里必然失败）。
    std::vector<eda::agent::ReadyPlugin> ready;
    for (const auto& record : pluginHost.Records()) {
        if (record.status != eda::PluginStatus::Ready) continue;
        eda::IPluginInteraction* instance = pluginHost.Get(record.id);
        if (instance == nullptr) continue;
        auto* provider = dynamic_cast<eda::IJobProvider*>(instance);
        if (provider == nullptr) continue;
        const std::string jobType = provider->jobType();
        if (jobType.empty()) continue;
        eda::agent::ReadyPlugin entry;
        entry.id = record.id;
        entry.version = record.version;
        entry.capabilities = {jobType};
        if (jobType == "synth" && yosys.empty()) {
            entry.ready = false;
            entry.reason = "tool for 'synth' is not available: yosys was not found";
        } else if ((jobType == "sim.build" || jobType == "sim.run") && verilator.empty()) {
            entry.ready = false;
            entry.reason = "tool for '" + jobType + "' is not available: verilator was not found";
        }
        ready.push_back(std::move(entry));
    }
    eda::agent::GatewayServer server(config, [ready]() { return ready; });
    server.SetJobServiceProvider([&jobService]() -> eda::IJobService* { return &jobService; });
    server.SetWaveformBackendFactory([]() -> std::shared_ptr<eda::IWaveformBackend> {
        return std::make_shared<eda::wave::VcdWaveformBackend>();
    });

    std::string error;
    std::string projectId;
    Check(server.RefreshProjectSnapshotStateFromDisk(projectRoot, false, true, projectId, error),
          "real-tool project context built from disk");
    Check(server.Start(error), "gateway starts for real-tool run");
    server.RunAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int status = 0;
    const std::string route = "/api/v1/projects/" + projectId;

    // 能力必须是真实就绪状态，而不是"插件文件存在"。
    const eda::Json caps = GetJson(server.Port(), "/api/v1/capabilities", "real-token", status);
    Check(status == 200, "capabilities readable");
    bool synthReady = false;
    for (const auto& entry : caps["data"]["capabilities"]) {
        if (entry.value("id", std::string()) == "eda.synth") {
            synthReady = entry.value("ready", false);
        }
    }
    if (yosys.empty()) {
        Check(!synthReady, "missing yosys is reported as not ready (no fake capability)");
        Skip("real yosys not present: synthesis end-to-end not exercised");
    } else {
        Check(synthReady, "real yosys is reported ready");
    }
    Check(!caps["data"]["capabilities"].empty(), "capability list is populated");

    const bool canSynth = !yosys.empty() && synthReady && synthProviderReady;
    if (!canSynth) {
        Skip("synth path not fully available (tool/provider/capability): real synthesis skipped");
    }

    // 建快照 + 授权（真实流程）。
    const eda::Json context = GetJson(server.Port(), route + "/context", "real-token", status);
    const std::string revision = context["data"].value("revision", std::string());
    Check(status == 200 && !revision.empty(), "context exposes a revision");

    httplib::Headers idem;
    idem.emplace("Idempotency-Key", "real-synth-1");
    const eda::Json snapshot = PostJson(
        server.Port(), route + "/snapshots", "real-token",
        eda::Json{{"expected_revision", revision}, {"require_saved", true}}, status, {});
    Check(status == 201, "snapshot created for real-tool run");
    const std::string snapshotId = snapshot["data"].value("snapshot_id", std::string());

    const eda::Json approvedSteps = eda::Json::array({
        eda::Json{{"step_id", "s1"}, {"capability", "eda.synth"}, {"description", "synth"}, {"max_jobs", 1},
                  {"params", {{"top_module", "top"}, {"strategy", "baseline"}}}, {"depends_on", eda::Json::array()}},
        eda::Json{{"step_id", "s2"}, {"capability", "eda.sim.build"}, {"description", "build"}, {"max_jobs", 1},
                  {"params", {{"top_module", "top"}, {"testbench", "top"}}}, {"depends_on", {"s1"}}},
        eda::Json{{"step_id", "s3"}, {"capability", "eda.sim.run"}, {"description", "run"}, {"max_jobs", 1},
                  {"params", {{"top_module", "top"}, {"testbench", "top"}}}, {"depends_on", {"s2"}}}});

    const eda::Json grant = PostJson(
        server.Port(), route + "/grants", "real-ui-token",
        eda::Json{{"plan_hash", "ph-real"}, {"revision", revision}, {"snapshot_id", snapshotId},
                  {"max_jobs", 3}, {"steps", approvedSteps}},
        status, {});
    Check(status == 201, "grant issued for real-tool run");
    if (status != 201) { std::cout << grant.dump() << '\n'; server.Stop(); return 1; }
    const std::string grantId = grant["data"].value("grant_id", std::string());

    // ---- 真实综合 ----
    if (canSynth) {
        const eda::Json job = PostJson(
            server.Port(), route + "/jobs", "real-token",
            eda::Json{{"snapshot_id", snapshotId},
                      {"expected_revision", revision},
                      {"capability", "eda.synth"},
                      {"plan_hash", "ph-real"}, {"step_id", "s1"},
                      {"grant_id", grantId},
                      {"params", eda::Json{{"top_module", "top"}, {"strategy", "baseline"}}}},
            status, idem);
        Check(status == 202, "real synth job accepted");
        const std::string jobId = job["data"].value("job_id", std::string());

        if (!jobId.empty()) {
            httplib::Headers duplicateKey{{"Idempotency-Key", "real-synth-other-key"}};
            PostJson(server.Port(), route + "/jobs", "real-token",
                eda::Json{{"snapshot_id", snapshotId}, {"expected_revision", revision}, {"capability", "eda.synth"},
                          {"plan_hash", "ph-real"}, {"step_id", "s1"}, {"grant_id", grantId},
                          {"params", {{"top_module", "top"}, {"strategy", "baseline"}}}}, status, duplicateKey);
            Check(status == 403, "another idempotency key cannot execute the approved synth step again");
            // 等待真实 yosys 进程结束（给足超时；真实综合通常数秒）。
            const auto terminal = [&]() {
                const auto record = jobService.get(jobId);
                if (!record.has_value()) return false;
                return record->state == eda::JobState::Succeeded ||
                       record->state == eda::JobState::Failed ||
                       record->state == eda::JobState::TimedOut ||
                       record->state == eda::JobState::Cancelled;
            };
            const bool finished = WaitUntil(terminal, 240000);
            Check(finished, "real synth job reaches a terminal state");

            const auto record = jobService.get(jobId);
            const eda::JobState state =
                record.has_value() ? record->state : eda::JobState::Failed;
            if (state != eda::JobState::Succeeded) {
                std::cout << "      synth terminal state: "
                          << eda::agent::JobStateName(state) << "\n";
            }
            Check(state == eda::JobState::Succeeded, "real yosys synthesis succeeded");

            // 报告必须来自真实运行，且诊断/产物可追溯。
            const eda::Json report =
                GetJson(server.Port(), "/api/v1/jobs/" + jobId + "/report", "real-token", status);
            Check(status == 200, "real synth report readable");
            Check(report["data"].value("schema_version", std::string()) == "edu.jobreport.v1",
                  "real synth report uses edu.jobreport.v1");
            Check(report["data"].value("origin", std::string()) == "core",
                  "real synth report is core-origin (not legacy)");
            Check(report["data"].value("state", std::string()) == "Succeeded",
                  "real synth report reflects the real terminal state");
            std::cout << "      synth artifacts=" << report["data"]["artifacts"].size()
                      << " diagnostics=" << report["data"]["diagnostics"].size() << "\n";
            if (report["data"].value("state", std::string()) != "Succeeded") {
                // 失败时打印规范报告，便于定位真实工具的问题（不改变断言结果）。
                std::cout << "      synth report: " << report["data"].dump() << "\n";
            }
        }
    }

    // ---- 真实 sim.build（Verilator 编译）----
    // 插件已具备前置自检 + 自愈（SIGFLOW_PYTHON3/SIGFLOW_SH/常见安装目录 + python3 探测注入）：
    // 不再因为 PATH 上的 python3 占位程序或缺 sh 就预判 SKIP。真实提交 sim.build，
    // 预期要么 Succeeded；要么失败时报告必须"指名道姓"指出缺的是哪个前置项，
    // 依旧不能只给 exit code。
    if (verilator.empty() || !canSynth) {
        Skip("real ordered chain requires both synthesis and Verilator prerequisites");
    } else {
        httplib::Headers simIdem;
        simIdem.emplace("Idempotency-Key", "real-sim-1");
        const eda::Json simJob = PostJson(
            server.Port(), route + "/jobs", "real-token",
            eda::Json{{"snapshot_id", snapshotId},
                      {"expected_revision", revision},
                      {"capability", "eda.sim.build"},
                      {"plan_hash", "ph-real"}, {"step_id", "s2"},
                      {"grant_id", grantId},
                      {"params", eda::Json{{"top_module", "top"}, {"testbench", "top"}}}},
            status, simIdem);
        Check(status == 202, "real sim.build job accepted");
        const std::string simJobId = simJob["data"].value("job_id", std::string());
        if (!simJobId.empty()) {
            const auto terminal = [&]() {
                const auto record = jobService.get(simJobId);
                if (!record.has_value()) return false;
                return record->state == eda::JobState::Succeeded ||
                       record->state == eda::JobState::Failed ||
                       record->state == eda::JobState::TimedOut ||
                       record->state == eda::JobState::Cancelled;
            };
            Check(WaitUntil(terminal, 240000), "real sim.build reaches a terminal state");
            const auto record = jobService.get(simJobId);
            const eda::JobState simState =
                record.has_value() ? record->state : eda::JobState::Failed;
            if (simState == eda::JobState::Succeeded) {
                Check(true, "real verilator sim.build succeeded");

                // ---- 真实 sim.run + VCD 发布/读取 ----
                // The Gateway derives sim_exe from the succeeded build report;
                // this request deliberately carries no executable path.
                httplib::Headers runIdem;
                runIdem.emplace("Idempotency-Key", "real-sim-run-1");
                const eda::Json runJob = PostJson(
                    server.Port(), route + "/jobs", "real-token",
                    eda::Json{{"snapshot_id", snapshotId},
                              {"expected_revision", revision},
                              {"capability", "eda.sim.run"},
                              {"plan_hash", "ph-real"}, {"step_id", "s3"},
                              {"grant_id", grantId},
                              {"params", eda::Json{{"top_module", "top"},
                                                    {"testbench", "top"},
                                                    {"build_job_id", simJobId}}}},
                    status, runIdem);
                Check(status == 202, "real sim.run job accepted without an Agent-supplied path");
                const std::string runJobId = runJob["data"].value("job_id", std::string());
                const auto runTerminal = [&]() {
                    const auto record = jobService.get(runJobId);
                    if (!record.has_value()) return false;
                    return record->state == eda::JobState::Succeeded ||
                           record->state == eda::JobState::Failed ||
                           record->state == eda::JobState::TimedOut ||
                           record->state == eda::JobState::Cancelled;
                };
                Check(!runJobId.empty() && WaitUntil(runTerminal, 120000),
                      "real sim.run reaches a terminal state");
                const auto runRecord = jobService.get(runJobId);
                Check(runRecord.has_value() && runRecord->state == eda::JobState::Succeeded,
                      "real verilator sim.run succeeded and produced a VCD");
                if (!runRecord.has_value() || runRecord->state != eda::JobState::Succeeded) {
                    const eda::JobReport failedRunReport = jobService.report(runJobId);
                    std::cout << "      sim.run failure report: "
                              << eda::Json{{"state", eda::agent::JobStateName(failedRunReport.state)},
                                           {"diagnostics", failedRunReport.diagnostics},
                                           {"metrics", failedRunReport.metrics}}
                                     .dump()
                              << "\n";
                }

                int runReportStatus = 0;
                const eda::Json runReport = GetJson(
                    server.Port(), "/api/v1/jobs/" + runJobId + "/report", "real-token",
                    runReportStatus);
                std::string waveArtifactId;
                if (runReportStatus == 200 && runReport.contains("data") &&
                    runReport["data"].contains("artifacts")) {
                    for (const eda::Json& artifact : runReport["data"]["artifacts"]) {
                        if (artifact.value("schema", std::string()) == "eda.wave.vcd.v1") {
                            waveArtifactId = artifact.value("artifact_id", std::string());
                            break;
                        }
                    }
                }
                Check(!waveArtifactId.empty(), "sim.run report publishes an opaque VCD artifact id");
                if (!waveArtifactId.empty()) {
                    int waveStatus = 0;
                    const eda::Json signals = GetJson(
                        server.Port(), "/api/v1/waves/" + waveArtifactId + "/signals",
                        "real-token", waveStatus);
                    Check(waveStatus == 200 && signals["data"].contains("signals"),
                          "real VCD is queryable through the bounded wave endpoint");
                }
            } else {
                std::cout << "      sim.build terminal state: "
                          << eda::agent::JobStateName(simState) << "\n";
                // 失败必须可诊断：报告里必须点名缺的前置项（python3/sh/make/g++）。
                int reportStatus = 0;
                const eda::Json simReport = GetJson(
                    server.Port(), "/api/v1/jobs/" + simJobId + "/report", "real-token",
                    reportStatus);
                std::cout << "      sim.build report: " << simReport["data"].dump() << "\n";
                const std::string text = simReport["data"].dump();
                bool diagnosable = false;
                for (const char* keyword : {"python3:", "POSIX shell", "make:", "g++:"}) {
                    if (text.find(keyword) != std::string::npos) {
                        diagnosable = true;
                        break;
                    }
                }
                Check(diagnosable,
                      "sim.build failure names the missing precondition (python3/sh/make/g++)");
            }
        }
    }

    server.Stop();
    Check(!server.Running(), "real-tool gateway stops cleanly");
    if (std::getenv("SIGFLOW_KEEP_REAL_TOOL_TEMP") != nullptr) {
        std::cout << "  info: retaining real-tool temporary files at " << root.string() << "\n";
    } else {
        fs::remove_all(root, cleanupError);
        Check(!cleanupError, "real-tool temporary files are removed");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << " (skipped=" << g_skipped << ")\n";
    return g_failures == 0 ? 0 : 1;
}
