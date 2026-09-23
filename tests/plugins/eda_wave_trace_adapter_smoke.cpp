#include "WaveformBackendTraceSource.h"

#include "VcdWaveformBackend.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "eda_wave_trace_adapter_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path vcd = dir / "wave.vcd";
    {
        std::ofstream out(vcd);
        out << "$timescale 1ns $end\n"
               "$scope module TOP $end\n"
               "$var wire 1 ! clk $end\n"
               "$upscope $end\n"
               "$enddefinitions $end\n"
               "$dumpvars\n0!\n$end\n#10\n1!\n#20\n0!\n";
    }

    sigflow::trace::WaveformBackendTraceSource source(
        std::make_shared<eda::wave::VcdWaveformBackend>());
    std::string error;
    Check(source.Open(vcd.string(), error), "adapter opens VCD via IWaveformBackend");
    Check(source.Signals().size() == 1, "adapter exposes signals");
    Check(source.Timescale() == "1ns", "adapter timescale");

    const sigflow::trace::TraceTimeRange range = source.TimeRange();
    Check(range.valid && range.end == 20, "adapter time range");

    if (!source.Signals().empty()) {
        const sigflow::trace::SignalInfo& signal = source.Signals()[0];
        Check(signal.name == "clk" && signal.kind == sigflow::trace::SignalKind::Scalar,
              "adapter signal metadata");

        std::vector<sigflow::trace::Transition> transitions;
        Check(source.Query(signal, 0, 100, transitions, error) && transitions.size() == 3,
              "adapter query transitions");
        std::string value;
        Check(source.ValueAt(signal, 15, value, error) && value == "1",
              "adapter value-at");
    }

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
