#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/plugin_registry.h>

#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
#include "YosysSynthesizer.h"

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

// 模拟 yosys：静默返回 Success，并按 `payload` 写出网表文件。
class FakeYosysHost final : public eda::IProcessHost {
public:
    FakeYosysHost(fs::path output, std::string payload)
        : output_(std::move(output)), payload_(std::move(payload)) {}

    eda::ProcessResult Run(const eda::ProcessSpec&,
                           const eda::ProcessOutputCallback& onOutput) override {
        if (onOutput) onOutput("simulated yosys\n", false);
        std::error_code error;
        fs::create_directories(output_.parent_path(), error);
        std::ofstream out(output_, std::ios::binary | std::ios::trunc);
        out << payload_;
        eda::ProcessResult result;
        result.started = true;
        result.outcome = eda::ProcessOutcome::Success;
        result.exitCode = 0;
        result.output = "ok";
        return result;
    }

    void Cancel() override {}

private:
    fs::path output_;
    std::string payload_;
};

constexpr const char* kValidNetlist =
    "{\"modules\":{\"top\":{\"ports\":{\"a\":{}},\"cells\":{\"u1\":{}},"
    "\"netnames\":{\"n1\":{}}}}}";

eda::JobRequest MakeRequest(const fs::path& source, const fs::path& output) {
    eda::JobRequest request;
    request.jobType = "synth";
    request.projectId = "prj";
    request.params = eda::Json{
        {"top_module", "top"},
        {"source_files", eda::Json::array({source.string()})},
        {"yosys_family", "gw1n"},
        {"strategy", "baseline"},
        {"output_json", output.string()},
        {"yosys_path", "yosys"}};
    return request;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "eda_synth_yosys_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path source = dir / "top.v";
    {
        std::ofstream(source) << "module top; endmodule\n";
    }

    // 插件经 EDA_REGISTER_PLUGIN 自注册。
    {
        eda::PluginHost host;
        Check(host.RegisterBuiltins() >= 1, "EDA_REGISTER_PLUGIN registers eda-synth-yosys");
        Check(!host.ProvidersOf("synth/yosys").empty(), "eda-synth-yosys exposes synth/yosys");
    }

    // 成功路径：合法网表 → Succeeded + artifact。
    {
        const fs::path output = dir / "top.json";
        eda::CoreJobService service(
            [&]() { return std::make_unique<FakeYosysHost>(output, kValidNetlist); });
        service.RegisterProvider(std::make_shared<eda::synth::YosysSynthesizer>());

        const std::string id = service.submit(MakeRequest(source, output));
        Check(WaitUntil([&]() {
                  const eda::JobReport report = service.report(id);
                  return report.state == eda::JobState::Succeeded && !report.artifacts.empty();
              }),
              "synth job reaches Succeeded with artifact");

        const eda::JobReport report = service.report(id);
        Check(report.artifacts.size() == 1, "exactly one artifact registered");
        Check(!report.artifacts.empty() && report.artifacts[0].id == "netlist",
              "artifact id is netlist");
        Check(!report.artifacts.empty() && report.artifacts[0].schema == "eda.netlist.yosys-json.v1",
              "artifact schema is yosys-json");
        Check(!report.artifacts.empty() && !report.artifacts[0].sha256.empty(),
              "artifact carries sha256");
        Check(report.metrics.contains("exit_code"), "job report records exit_code metric");
        Check(report.metrics.value("cells", 0) == 1, "job report records cell count metric");
    }

    // 失败路径：非法网表 → Failed。
    {
        const fs::path output = dir / "bad.json";
        eda::CoreJobService service(
            [&]() { return std::make_unique<FakeYosysHost>(output, "this is not valid json"); });
        service.RegisterProvider(std::make_shared<eda::synth::YosysSynthesizer>());

        const std::string id = service.submit(MakeRequest(source, output));
        Check(WaitUntil([&]() { return service.report(id).state == eda::JobState::Failed; }),
              "invalid netlist makes the job Failed");
    }

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
