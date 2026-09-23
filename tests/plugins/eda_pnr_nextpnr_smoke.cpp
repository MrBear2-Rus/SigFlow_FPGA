#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/plugin_registry.h>

#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
#include "NextpnrLogModel.h"
#include "NextpnrPlaceRouter.h"
#include "NextpnrReportGen.h"

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

class FakeNextpnrHost final : public eda::IProcessHost {
public:
    explicit FakeNextpnrHost(fs::path output) : output_(std::move(output)) {}

    eda::ProcessResult Run(const eda::ProcessSpec&,
                           const eda::ProcessOutputCallback& onOutput) override {
        if (onOutput) onOutput("simulated nextpnr\n", false);
        std::error_code error;
        fs::create_directories(output_.parent_path(), error);
        std::ofstream out(output_, std::ios::binary | std::ios::trunc);
        out << "{\"modules\":{}}";
        eda::ProcessResult result;
        result.started = true;
        result.outcome = eda::ProcessOutcome::Success;
        result.exitCode = 0;
        return result;
    }

    void Cancel() override {}

private:
    fs::path output_;
};

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "eda_pnr_nextpnr_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path netlist = dir / "top.json";
    {
        std::ofstream(netlist) << "{\"modules\":{}}";
    }
    const fs::path output = dir / "top.pnr.json";

    {
        eda::PluginHost host;
        Check(host.RegisterBuiltins() >= 1, "EDA_REGISTER_PLUGIN registers the plugin");
        Check(!host.ProvidersOf("pnr/nextpnr").empty(), "eda-pnr-nextpnr exposes pnr/nextpnr");
    }

    eda::CoreJobService service([&output]() {
        return std::make_unique<FakeNextpnrHost>(output);
    });
    service.RegisterProvider(std::make_shared<eda::pnr::NextpnrPlaceRouter>());

    eda::JobRequest request;
    request.jobType = "pnr";
    request.projectId = "prj";
    request.params = eda::Json{
        {"top_module", "top"},
        {"netlist", netlist.string()},
        {"device", "GW1NR-LV9QN88PC6/I5"},
        {"family", "GW1N-9C"},
        {"output", output.string()},
        {"nextpnr_path", "nextpnr-himbaechel"}};

    const std::string id = service.submit(request);
    Check(WaitUntil([&]() {
              const eda::JobReport report = service.report(id);
              return report.state == eda::JobState::Succeeded && !report.artifacts.empty();
          }),
          "pnr job reaches Succeeded with artifact");

    const eda::JobReport report = service.report(id);
    Check(!report.artifacts.empty() && report.artifacts[0].id == "pnr-json",
          "artifact id is pnr-json");
    Check(report.metrics.contains("exit_code"), "job report records exit_code metric");

    // 移入的日志解析/报告（wx-free）
    {
        const std::string log =
            "Info: nextpnr-himbaechel -- Next Place and Route -- for Gowin GW1N-9C\n"
            "Info: Version 0.7 (git sha1 abc)\n"
            "Info: Packing IOBs..\n"
            "Info: Placing...\n"
            "Info: Routing...\n"
            "Info: Device utilisation:\n"
            "IOB: 5/276 1%\n"
            "LUT4: 108/8640 2%\n"
            "Info: Max frequency for clock 'clk': 62.64 MHz (PASS at 27.00 MHz)\n"
            "Warning: sample warning\n";

        eda::pnr::NextpnrRunRecord parsed;
        eda::pnr::NextpnrLogParser parser;
        parser.Parse(log, "", parsed);
        Check(parsed.packCompleted && parsed.placeCompleted && parsed.routeCompleted,
              "log parser detects all PnR stages");
        Check(parsed.maxFrequencyMHz > 60.0 && parsed.timingPassed,
              "log parser extracts timing");
        Check(parsed.resources.count("IOB") == 1, "log parser extracts resource usage");
        Check(parsed.toolVersion == "0.7", "log parser extracts tool version");

        eda::pnr::NextpnrReport reportGenerator;
        Check(reportGenerator.FormatSummary(parsed).find("Result: succeeded") != std::string::npos,
              "report summary generated");
        Check(reportGenerator.GenerateJson(parsed).find("\"max_freq_mhz\"") != std::string::npos,
              "report json generated");
    }

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
