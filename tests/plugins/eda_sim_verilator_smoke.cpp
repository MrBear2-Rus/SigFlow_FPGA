#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/plugin_registry.h>

#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
#include "VerilatorRunJob.h"
#include "VerilatorSimulator.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

class FakeVerilatorHost final : public eda::IProcessHost {
public:
    explicit FakeVerilatorHost(fs::path executable) : executable_(std::move(executable)) {}

    eda::ProcessResult Run(const eda::ProcessSpec&,
                           const eda::ProcessOutputCallback& onOutput) override {
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

    {
        eda::PluginHost host;
        Check(host.RegisterBuiltins() >= 1, "EDA_REGISTER_PLUGIN registers eda-sim-verilator");
        Check(!host.ProvidersOf("sim/verilator").empty(), "sim/verilator capability exposed");
    }

    eda::CoreJobService service(
        [&]() { return std::make_unique<FakeVerilatorHost>(executable); });
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
