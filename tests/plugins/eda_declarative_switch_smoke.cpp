#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/plugin_registry.h>

#include "eda-core/DeclarativeTool.h"
#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
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

// 写入 "-o <path>" 指定的产物。
class ToolHost final : public eda::IProcessHost {
public:
    eda::ProcessResult Run(const eda::ProcessSpec& spec,
                           const eda::ProcessOutputCallback& onOutput) override {
        if (onOutput) onOutput("tool\n", false);
        for (std::size_t i = 0; i + 1 < spec.arguments.size(); ++i) {
            if (spec.arguments[i] == "-o") {
                std::error_code error;
                fs::create_directories(fs::path(spec.arguments[i + 1]).parent_path(), error);
                std::ofstream(spec.arguments[i + 1], std::ios::binary) << "artifact";
            }
        }
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
    // P1 DoD 场景 B：sim/verilator ↔ sim/icarus 声明式切换，无需重编译。
    // icarus 用 JSON 描述在运行时创建，不新增任何 C++ 插件代码。
    const std::string icarusJson = R"({
        "id": "sim.icarus",
        "job_type": "sim.icarus",
        "capability": "sim/icarus",
        "executable": "iverilog",
        "arguments": ["-o", "${output}", "${source_files}"],
        "requires_params": ["output"],
        "artifacts": [{"id": "sim-exe", "path": "${output}", "schema": "eda.sim.icarus.v1"}]
    })";

    eda::DeclarativeToolSpec icarusSpec;
    std::string error;
    Check(eda::DeclarativeJobProvider::ParseSpec(eda::Json::parse(icarusJson), icarusSpec, error),
          "parse declarative icarus spec");

    eda::PluginHost host;
    auto icarus = std::make_shared<eda::DeclarativeJobProvider>(icarusSpec);
    Check(host.RegisterStatic(icarus), "register declarative icarus into PluginHost");
    auto verilator = std::make_shared<eda::sim::VerilatorSimulator>();
    Check(host.RegisterStatic(verilator), "register verilator plugin into PluginHost");

    Check(!host.ProvidersOf("sim/verilator").empty(), "sim/verilator capability available");
    Check(!host.ProvidersOf("sim/icarus").empty(), "sim/icarus capability available");
    Check(host.Select("sim/icarus") == icarus.get(),
          "capability selector returns the declarative icarus backend");
    Check(host.Select("sim/verilator") != nullptr, "capability selector returns verilator backend");
    Check(host.Select("sim/icarus") != host.Select("sim/verilator"),
          "two backends are distinct (switch without recompile)");

    // 用 icarus 后端跑一个 Job，证明"声明式后端"可执行。
    const fs::path dir = fs::temp_directory_path() / "eda_declarative_switch_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path output = dir / "sim.out";

    eda::CoreJobService service([]() { return std::make_unique<ToolHost>(); });
    service.RegisterProvider(icarus);

    eda::JobRequest request;
    request.jobType = "sim.icarus";
    request.projectId = "prj";
    request.params = eda::Json{{"output", output.string()},
                               {"source_files", eda::Json::array({"a.v"})}};
    const std::string id = service.submit(request);
    Check(WaitUntil([&]() {
              const eda::JobReport report = service.report(id);
              return report.state == eda::JobState::Succeeded && !report.artifacts.empty();
          }),
          "declarative icarus job reaches Succeeded with Job record + artifact");
    Check(service.report(id).artifacts[0].id == "sim-exe", "declared artifact recorded");

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
