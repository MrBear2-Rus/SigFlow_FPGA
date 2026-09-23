#include "VcdWaveformBackend.h"

#include <filesystem>
#include <fstream>
#include <iostream>
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

const eda::WaveSignal* FindSignal(const std::vector<eda::WaveSignal>& signals,
                                  const std::string& name) {
    for (const auto& signal : signals) {
        if (signal.name == name) return &signal;
    }
    return nullptr;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "eda_wave_vcd_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path vcd = dir / "wave.vcd";
    {
        std::ofstream out(vcd);
        out << "$timescale 1ns $end\n"
               "$scope module TOP $end\n"
               "$var wire 1 ! clk $end\n"
               "$var reg 8 \" state $end\n"
               "$upscope $end\n"
               "$enddefinitions $end\n"
               "$dumpvars\n"
               "0!\n"
               "b00000000 \"\n"
               "$end\n"
               "#10\n"
               "1!\n"
               "#20\n"
               "b00000101 \"\n"
               "#30\n"
               "0!\n";
    }

    eda::wave::VcdWaveformBackend backend;
    std::string error;
    Check(backend.Open(vcd.string(), error), "opens VCD");
    Check(backend.Timescale() == "1ns", "timescale parsed");
    Check(backend.Signals().size() == 2, "signal count");

    const eda::WaveSignal* clk = FindSignal(backend.Signals(), "clk");
    const eda::WaveSignal* state = FindSignal(backend.Signals(), "state");
    Check(clk != nullptr && clk->width == 1, "clk signal width");
    Check(state != nullptr && state->width == 8 && state->scope == "TOP", "state signal metadata");
    Check(state != nullptr && state->fullName == "TOP.state", "full name");

    const eda::WaveTimeRange range = backend.TimeRange();
    Check(range.valid && range.begin == 0 && range.end == 30, "time range");

    if (clk != nullptr) {
        std::vector<eda::WaveTransition> transitions;
        Check(backend.Query(clk->id, 0, 100, transitions, error), "query transitions");
        Check(transitions.size() == 3, "clk transition count");
        std::string value;
        Check(backend.ValueAt(clk->id, 25, value, error) && value == "1",
              "value-at between transitions");
        Check(backend.ValueAt(clk->id, 30, value, error) && value == "0", "value-at exact");
    }

    if (state != nullptr) {
        std::string value;
        Check(backend.ValueAt(state->id, 25, value, error) && value == "00000101",
              "vector value-at");
    }

    {
        std::string queryError;
        std::vector<eda::WaveTransition> transitions;
        Check(!backend.Query(999, 0, 10, transitions, queryError), "unknown signal rejected");
    }

    Check(!backend.Open((dir / "missing.vcd").string(), error), "missing VCD rejected");

    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
