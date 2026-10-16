// Producer regression with a fake process host; never invokes a tool or deletes files.
#include "VerilatorSimulator.h"
#include "VerilatorRunJob.h"
#include "eda-core/JobService.h"
#include "eda-platform/Platform.h"
#include <algorithm>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

class ProcessFixture final : public eda::IProcessHost {
public:
    explicit ProcessFixture(fs::path executable) : executable_(std::move(executable)) {}
    eda::ProcessResult Run(const eda::ProcessSpec& spec,
                          const eda::ProcessOutputCallback&) override {
        if (std::find(spec.arguments.begin(), spec.arguments.end(), "--build") != spec.arguments.end()) {
            std::ofstream(executable_, std::ios::binary) << "compiled\n";
        } else if (spec.executable == executable_) {
            std::ofstream(spec.workingDirectory / "waveform/wave.vcd", std::ios::binary) << "$date\n$end\n";
        }
        eda::ProcessResult result;
        result.started = true; result.outcome = eda::ProcessOutcome::Success; result.exitCode = 0;
        return result;
    }
    void Cancel() override {}
private:
    fs::path executable_;
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path root = eda::platform::PathFromUtf8(argv[1]);
    if (!root.is_absolute() || !fs::is_regular_file(root / "test-owned")) return 2;
    fs::create_directories(root / "sim");
    fs::create_directories(root / "fakebin");
    const auto executable = root / "sim/sim_main.exe";
    std::ofstream(root / "top.v") << "module top; endmodule\n";
    std::ofstream(root / "fakebin/python.exe") << "fixture";
    std::ofstream(root / "fakebin/sh.exe") << "fixture";
#ifdef _WIN32
    _putenv_s("SIGFLOW_PYTHON3", (root / "fakebin/python.exe").string().c_str());
    _putenv_s("SIGFLOW_SH", (root / "fakebin/sh.exe").string().c_str());
#endif
    auto factory = [executable] { return std::make_unique<ProcessFixture>(executable); };
    eda::CoreJobContext build("build", root, factory, {});
    eda::sim::VerilatorSimulator builder;
    eda::JobRequest request;
    request.jobType = "sim.build";
    request.params = eda::Json{{"top_module","top"},
        {"source_files", eda::Json::array({(root / "top.v").string()})},
        {"out_dir", (root / "sim").string()}, {"executable", executable.string()},
        {"verilator_path","fixture-verilator"}};
    if (builder.startJob(request, build)) return 3;
    const auto built = build.Artifacts();
    const bool buildHash = built.size() == 1 && built[0].sha256 ==
        "82bbb1ee2e80489d4898e857907af2f6915ca682a0fbf1be13cea02fa3e88e79";
    eda::CoreJobContext run("run", root, factory, {});
    eda::sim::VerilatorRunJob runner;
    request.jobType = "sim.run";
    request.params = eda::Json{{"sim_exe",executable.string()}};
    if (runner.startJob(request, run)) return 3;
    const auto ran = run.Artifacts();
    const bool runHash = ran.size() == 1 && ran[0].sha256 ==
        "8dc846eecdaa0c797dc6f851d0b8abb3a13ba186586c0cb52fa917a1008d1f54";
    std::cout << "build_hash=" << buildHash << " run_hash=" << runHash << "\n";
    return buildHash && runHash ? 0 : 1;
}
