#include <eda/api/jobs.hpp>
#include <eda/api/process.hpp>
#include <eda/api/plugin_registry.h>

#include "eda-core/JobService.h"
#include "eda-core/PluginHost.h"
#include "GowinPacker.h"
#include "OpenFpgaLoaderProgrammer.h"

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

class FakeToolHost final : public eda::IProcessHost {
public:
    explicit FakeToolHost(std::string artifact = {}) : artifact_(std::move(artifact)) {}

    eda::ProcessResult Run(const eda::ProcessSpec&,
                           const eda::ProcessOutputCallback& onOutput) override {
        if (onOutput) onOutput("simulated tool\n", false);
        if (!artifact_.empty()) {
            std::error_code error;
            fs::create_directories(fs::path(artifact_).parent_path(), error);
            std::ofstream out(artifact_, std::ios::binary | std::ios::trunc);
            out << "bitstream-payload-0123456789abcdef0123456789abcdef";
        }
        eda::ProcessResult result;
        result.started = true;
        result.outcome = eda::ProcessOutcome::Success;
        result.exitCode = 0;
        return result;
    }

    void Cancel() override {}

private:
    std::string artifact_;
};

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "eda_pack_flash_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);

    {
        eda::PluginHost host;
        Check(host.RegisterBuiltins() >= 2, "EDA_REGISTER_PLUGIN registers pack/flash plugins");
        Check(!host.ProvidersOf("pack/gowin").empty(), "pack/gowin capability exposed");
        Check(!host.ProvidersOf("program/openfpgaloader").empty(),
              "program/openfpgaloader capability exposed");
    }

    // pack
    {
        const fs::path output = dir / "design.fs";
        const fs::path pnrJson = dir / "top.pnr.json";
        std::ofstream(pnrJson) << "{}";
        const fs::path packerExe = dir / "gowin_pack.exe";
        std::ofstream(packerExe) << "dummy packer executable";

        eda::CoreJobService service(
            [&]() { return std::make_unique<FakeToolHost>(output.string()); });
        service.RegisterProvider(std::make_shared<eda::pack::GowinPacker>());

        eda::JobRequest request;
        request.jobType = "pack";
        request.projectId = "prj";
        request.params = eda::Json{{"pnr_json", pnrJson.string()},
                                   {"device", "GW1N-9C"},
                                   {"output", output.string()},
                                   {"gowin_pack_path", packerExe.string()}};

        const std::string id = service.submit(request);
        Check(WaitUntil([&]() {
                  const eda::JobReport report = service.report(id);
                  return report.state == eda::JobState::Succeeded && !report.artifacts.empty();
              }),
              "pack job reaches Succeeded with bitstream artifact");
        const eda::JobReport report = service.report(id);
        Check(!report.artifacts.empty() && report.artifacts[0].id == "bitstream",
              "pack artifact id is bitstream");
    }

    // flash requireConfirm gate
    {
        const fs::path bitstream = dir / "design.fs";
        eda::CoreJobService service([]() { return std::make_unique<FakeToolHost>(); });
        service.RegisterProvider(std::make_shared<eda::program::OpenFpgaLoaderProgrammer>());

        eda::JobRequest request;
        request.jobType = "flash";
        request.projectId = "prj";
        request.params = eda::Json{{"bitstream", bitstream.string()},
                                   {"board", "tangnano9k"},
                                   {"openfpgaloader_path", "openFPGALoader"}};

        const std::string unconfirmed = service.submit(request);
        Check(WaitUntil([&]() {
                  return service.report(unconfirmed).state == eda::JobState::Failed;
              }),
              "flash without requireConfirm is rejected");

        request.requireConfirm = true;
        const std::string confirmed = service.submit(request);
        Check(WaitUntil([&]() {
                  return service.report(confirmed).state == eda::JobState::Succeeded;
              }),
              "flash with requireConfirm succeeds");
    }

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
