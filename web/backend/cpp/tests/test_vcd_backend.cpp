#include "plugins/wave-vcd/VcdWaveformBackend.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
void Check(bool ok, const char* msg) {
    if (!ok)
        throw std::runtime_error(msg);
}
int main() {
    auto path = std::filesystem::temp_directory_path() /
                ("sigflow-vcd-" + std::to_string(getpid()) + ".vcd");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{path};
    auto write = [&](const std::string& text) {
        std::ofstream out(path);
        out << text;
        if (!out)
            throw std::runtime_error("write failed");
    };
    write("$timescale 1 ns $end\n$scope module top $end\n$var wire 1 ! clk $end\n$var wire 1 ! "
          "alias $end\n$var wire 4 # bus $end\n$upscope $end\n$enddefinitions "
          "$end\n$dumpvars\n0!\nbx #\n$end\n#4294967297\n1!\nb101 #\n");
    eda::wave::VcdWaveformBackend backend;
    std::string error, value;
    Check(backend.Open(path.string(), error), "open");
    Check(backend.Signals().size() == 3 && backend.Timescale() == "1ns", "metadata");
    Check(backend.TimeRange().end == 4294967297ULL, "64-bit timestamp");
    Check(backend.ValueAt(1, 4294967297ULL, value, error) && value == "1", "alias value");
    Check(backend.ValueAt(2, 0, value, error) && value == "xxxx", "unknown extension");
    Check(backend.ValueAt(2, 4294967297ULL, value, error) && value == "0101", "vector padding");
    std::vector<eda::WaveTransition> out;
    Check(backend.Query(0, 1, 4294967297ULL, out, error) && out.size() == 1,
          "closed interval query");
    Check(!backend.Query(99, 0, 1, out, error), "invalid signal");
    Check(!backend.Query(0, 2, 1, out, error), "inverted interval rejected");
    error = "old error";
    Check(backend.Query(0, 0, 0, out, error) && error.empty(), "success clears stale error");
    write("$comment " + std::string(65537, 'a') + " $end\n");
    Check(!backend.Open(path.string(), error) && backend.Signals().empty(),
          "oversized comment token rejected");
    write("$scope a b c d e f g h i $end\n");
    Check(!backend.Open(path.string(), error) && backend.Signals().empty(),
          "excess directive arguments rejected");
    std::string comment = "$comment ";
    for (int i = 0; i < 100000; ++i)
        comment += "x ";
    comment += "$end\n$var wire 1 ! clk $end\n$enddefinitions $end\n0!\n";
    write(comment);
    Check(backend.Open(path.string(), error), "large comment with short tokens accepted");
    write("$var wire 1 ! clk $end\n$enddefinitions $end\n#42\n0!\n#90\n1!\n");
    Check(backend.Open(path.string(), error) && backend.TimeRange().begin == 42 &&
              backend.TimeRange().end == 90,
          "time range uses first and last observed timestamp");
    write("$scope module top $end\n$var wire 1 ! clk $end\n$upscope $end\n$enddefinitions "
          "$end\n#10\n1!\n#9\n0!\n");
    Check(!backend.Open(path.string(), error) && backend.Signals().empty(),
          "invalid time clears prior data");
}
