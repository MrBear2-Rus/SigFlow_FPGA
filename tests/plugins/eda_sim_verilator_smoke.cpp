#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/plugin_registry.h>

#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
#include "VerilatorRunJob.h"
#include "VerilatorSimulator.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

template <typename F>
bool WaitUntil(F predicate, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

struct VerilatorInvocation {
    bool buildInvoked = false;
    std::vector<std::pair<std::string, std::string>> environment;
};

class FakeVerilatorHost final : public eda::IProcessHost {
public:
    FakeVerilatorHost(fs::path executable, std::shared_ptr<VerilatorInvocation> invocation)
        : executable_(std::move(executable)), invocation_(std::move(invocation)) {}

    eda::ProcessResult Run(const eda::ProcessSpec& spec,
                           const eda::ProcessOutputCallback& onOutput) override {
        // 构建步骤的识别：插件**刻意不再使用 `--binary`**（它会强制加入 Verilator 自带的
        // --main，与受控 trace main 冲突且不产 VCD），改为显式 `--cc --exe --build --trace`。
        // 因此这里按 `--build` 判定（同时兼容旧的 `--binary`），否则断言会永远看不到构建调用。
        const auto hasFlag = [&spec](const char* flag) {
            return std::find(spec.arguments.begin(), spec.arguments.end(), flag) !=
                   spec.arguments.end();
        };
        if (hasFlag("--build") || hasFlag("--binary")) {
            invocation_->buildInvoked = true;
            invocation_->environment = spec.environment;
        }
        if (onOutput) onOutput("simulated verilator\n", false);
        std::error_code error;
        fs::create_directories(executable_.parent_path(), error);
        std::ofstream(executable_, std::ios::binary) << "sim exe";
        eda::ProcessResult result;
        result.started = true;
        result.outcome = eda::ProcessOutcome::Success;
        result.exitCode = 0;
        return result;
    }

    void Cancel() override {}

private:
    fs::path executable_;
    std::shared_ptr<VerilatorInvocation> invocation_;
};

class FakeRunHost final : public eda::IProcessHost {
public:
    eda::ProcessResult Run(const eda::ProcessSpec& spec,
                           const eda::ProcessOutputCallback& onOutput) override {
        if (onOutput) onOutput("simulating\n", false);
        std::error_code error;
        fs::create_directories(spec.workingDirectory, error);
        std::ofstream(spec.workingDirectory / "wave.vcd") << "$date\n$end\n";
        eda::ProcessResult result;
        result.started = true;
        result.outcome = eda::ProcessOutcome::Success;
        result.exitCode = 0;
        return result;
    }

    void Cancel() override {}
};

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "eda_sim_verilator_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path source = dir / "top.v";
    std::ofstream(source) << "module top; endmodule\n";
    const fs::path executable = dir / "sim_main.exe";

    // startJob 现在带前置自检（make/g++/python3/POSIX sh）。Fake host 能让"实跑探测"
    // 通过，但 python/sh 的真实文件侧校验仍要求存在非 0 字节文件——本测试在临时目录
    // 伪造两者，并经 SIGFLOW_PYTHON3/SIGFLOW_SH 显式喂给插件，避免依赖构建机环境。
    {
        const fs::path fakePython = dir / "fakebin" / "python.exe";
        const fs::path fakeShell = dir / "fakesh" / "sh.exe";
        fs::create_directories(fakePython.parent_path(), cleanupError);
        fs::create_directories(fakeShell.parent_path(), cleanupError);
        std::ofstream(fakePython, std::ios::binary) << "fake python";
        std::ofstream(fakeShell, std::ios::binary) << "fake sh";
        _putenv_s("SIGFLOW_PYTHON3", fakePython.string().c_str());
        _putenv_s("SIGFLOW_SH", fakeShell.string().c_str());
    }

    {
        eda::PluginHost host;
        Check(host.RegisterBuiltins() >= 1, "EDA_REGISTER_PLUGIN registers eda-sim-verilator");
        Check(!host.ProvidersOf("sim/verilator").empty(), "sim/verilator capability exposed");
    }

    const auto invocation = std::make_shared<VerilatorInvocation>();
    eda::CoreJobService service(
        [&]() { return std::make_unique<FakeVerilatorHost>(executable, invocation); });
    service.RegisterProvider(std::make_shared<eda::sim::VerilatorSimulator>());

    eda::JobRequest request;
    request.jobType = "sim.build";
    request.projectId = "prj";
    request.params = eda::Json{{"top_module", "top"},
                               {"source_files", eda::Json::array({source.string()})},
                               {"out_dir", dir.string()},
                               {"executable", executable.string()},
                               {"verilator_path", "verilator_bin"}};

    const std::string id = service.submit(request);
    Check(WaitUntil([&]() {
              const eda::JobReport report = service.report(id);
              return report.state == eda::JobState::Succeeded && !report.artifacts.empty();
          }),
          "sim.build job reaches Succeeded with executable artifact");
    const eda::JobReport report = service.report(id);
    Check(!report.artifacts.empty() && report.artifacts[0].id == "sim-executable",
          "artifact id is sim-executable");
    Check(report.metrics.contains("exit_code"), "job report records exit_code metric");
    Check(invocation->buildInvoked, "Verilator build command is invoked after preflight");
    const auto findEnv = [&](const char* key) -> std::string {
        for (const auto& entry : invocation->environment) {
            if (entry.first == key) return entry.second;
        }
        return {};
    };
    const std::string shellEnv = findEnv("SHELL");
    const std::string pathEnv = findEnv("PATH");
    Check(!shellEnv.empty() && shellEnv.find("sh.exe") != std::string::npos,
          "Verilator child explicitly receives the verified POSIX SHELL");
    Check(!pathEnv.empty() && pathEnv.find("toolchain") != std::string::npos &&
              pathEnv.find("python3") != std::string::npos,
          "Verilator child PATH starts with the job-private python3 shim");

    // sim.run
    {
        eda::CoreJobService runService([]() { return std::make_unique<FakeRunHost>(); });
        runService.RegisterProvider(std::make_shared<eda::sim::VerilatorRunJob>());
        eda::JobRequest runRequest;
        runRequest.jobType = "sim.run";
        runRequest.projectId = "prj";
        runRequest.params =
            eda::Json{{"sim_exe", executable.string()}, {"working_dir", dir.string()}};
        const std::string runId = runService.submit(runRequest);
        Check(WaitUntil([&]() {
                  const eda::JobReport runReport = runService.report(runId);
                  return runReport.state == eda::JobState::Succeeded && !runReport.artifacts.empty();
              }),
              "sim.run job reaches Succeeded with waveform artifact");
        const eda::JobReport runReport = runService.report(runId);
        Check(!runReport.artifacts.empty() && runReport.artifacts[0].id == "waveform",
              "artifact id is waveform");
    }

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
