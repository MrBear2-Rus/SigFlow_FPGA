// The caller owns the absolute fixture directory, marked by a test-owned file.
// This executable writes fixtures there and never deletes files or starts tools.
#include "VcdWaveformBackend.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
const std::string header =
    "$timescale 1 ns $end\n$scope module TOP $end\n"
    "$var wire 1 ! q $end\n$var wire 1 ! q_alias $end\n"
    "$upscope $end\n$enddefinitions $end\n";
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    if (argc != 2) return 2;
    const auto root = std::filesystem::u8path(argv[1]);
    if (!root.is_absolute() || !std::filesystem::is_regular_file(root / "test-owned")) return 2;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const std::string& name) {
        ++checks;
        if (!ok) { ++failures; std::cout << "FAIL " << name << '\n'; }
    };
    int fixtureNumber = 0;
    auto fixture = [&](const std::string& contents) {
        const auto path = root / ("vcd-integrity-" + std::to_string(++fixtureNumber) + ".vcd");
        std::ofstream stream(path, std::ios::binary);
        stream << contents;
        stream.close();
        if (!stream) throw std::runtime_error("cannot write test-owned fixture");
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(),utf8.end());
    };

    eda::wave::VcdWaveformBackend backend;
    std::string error;
    auto open = [&](const std::string& contents) {
        try { return backend.Open(fixture(contents), error); }
        catch (const std::exception& ex) {
            check(false, "Open must return a status instead of throwing: " + std::string(ex.what()));
            return false;
        }
    };
    check(open(header + "$dumpvars\n0!\n$end\n#5\n1!\n#9\n0!\n"),
          "valid same-width aliases open");
    check(backend.Signals().size() == 2 && backend.Signals()[0].fullName == "TOP.q" &&
          backend.Signals()[1].fullName == "TOP.q_alias", "aliases retain distinct names");
    for (int id : {0, 1}) {
        std::vector<eda::WaveTransition> events;
        check(backend.Query(id, 0, 9, events, error) && events.size() == 3 &&
              events[0].time == 0 && events[0].value == "0" &&
              events[1].time == 5 && events[1].value == "1" &&
              events[2].time == 9 && events[2].value == "0",
              "alias " + std::to_string(id) + " returns every event including tick zero");
        std::string value;
        check(backend.ValueAt(id, 0, value, error) && value == "0" &&
              backend.ValueAt(id, 6, value, error) && value == "1",
              "alias " + std::to_string(id) + " ValueAt follows shared events");
    }

    check(open(header + "#0\n0!\n#18446744073709551615\n1!\n"),
          "maximum uint64 timestamp opens");
    std::vector<eda::WaveTransition> events;
    check(backend.Query(1, std::numeric_limits<std::uint64_t>::max(),
                        std::numeric_limits<std::uint64_t>::max(), events, error) &&
          events.size() == 1 && events[0].time == std::numeric_limits<std::uint64_t>::max() &&
          events[0].value == "1" && backend.TimeRange().end == std::numeric_limits<std::uint64_t>::max(),
          "maximum uint64 timestamp retains exact tick");

    const std::vector<std::pair<std::string, std::string>> invalidTimes = {
        {"negative", "#-1"}, {"signed", "#+1"}, {"trailing text", "#5junk"},
        {"missing digits", "#"}, {"overflow", "#18446744073709551616"},
        {"nonmonotonic", "#7\n1!\n#6"}
    };
    for (const auto& test : invalidTimes) {
        error.clear();
        check(!open(header + "#0\n0!\n" + test.second + "\n1!\n") &&
              !error.empty(), "reject " + test.first + " timestamp");
        check(backend.Signals().empty() && !backend.TimeRange().valid,
              "failed " + test.first + " timestamp exposes no partial waveform");
    }

    const std::vector<std::pair<std::string, std::string>> invalidDefinitions = {
        {"zero width", "$var wire 0 ! a $end\n"},
        {"signed width", "$var wire -1 ! a $end\n"},
        {"trailing width", "$var wire 1x ! a $end\n"},
        {"overflow width", "$var wire 4294967296 ! a $end\n"},
        {"inconsistent alias width", "$var wire 1 ! a $end\n$var wire 2 ! b $end\n"}
    };
    for (const auto& test : invalidDefinitions) {
        bool opened = false, threw = false;
        error.clear();
        try {
            opened = backend.Open(fixture(test.second + "$enddefinitions $end\n#0\n0!\n"), error);
        } catch (...) { threw = true; }
        check(!threw && !opened && !error.empty(), "reject " + test.first + " definition without throwing");
    }

    const std::string vectorHeader = "$var wire 4 ! bus $end\n$enddefinitions $end\n";
    for (const auto& value : {"b1020 !", "b10000 !", "b !", "b1010", "r1.0 !", "2!", "1!", "b10 unknown"}) {
        error.clear();
        check(!open(vectorHeader + "#0\n" + value + "\n") && !error.empty(),
              "reject malformed or unsupported value " + std::string(value));
    }

    const std::string legal =
        "$date test fixture $end\n$version fixture $end\n"
        "$comment\n#bad\n$var wire broken ! ignored $end\n"
        "$timescale\n1\nns\n$end\n$scope module TOP $end\n"
        "$var wire 4 ! bus [3:0] $end\n$var wire 4 ! alias [3:0] $end\n"
        "$upscope $end\n$enddefinitions $end\n"
        "$dumpvars\nb0010\t!\n$end\n#5\n"
        "$comment #bad b1234 ! $end\nbx !\n#5\nbz !\n#10\n";
    check(open(legal) && backend.Signals().size() == 2 &&
          backend.Timescale() == "1ns", "legal comments, ranges, tabs, and multiline timescale open");
    events.clear();
    check(backend.Query(0, 0, 5, events, error) && events.size() == 3 &&
          events[0].value == "0010" && events[1].time == 5 && events[2].time == 5,
          "legal vector aliases retain tick-zero and same-tick events");
    std::string value;
    check(backend.ValueAt(1, 5, value, error) && value == "zzzz", "compressed vector uses VCD z extension");
    check(backend.TimeRange().valid && backend.TimeRange().end == 10,
          "last timestamp contributes to trace range without a value change");
    check(open("$timescale 10ps $end\n" + vectorHeader + "#0\nb10 !\n") &&
          backend.Timescale() == "10ps" && backend.ValueAt(0, 0, value, error) && value == "0010",
          "inline timescale and compressed binary zero extension remain legal");
    check(!open("$var wire 4097 ! wide $end\n$enddefinitions $end\n#0\nb0 !\n"),
          "reject unsupported width before expanding a compressed value");
    std::string expanded;
    for (int alias=0;alias<17;++alias)
        expanded += "$var wire 4096 ! alias"+std::to_string(alias)+" $end\n";
    expanded += "$enddefinitions $end\n";
    for (int event=0;event<1025;++event) expanded += "#"+std::to_string(event)+"\nb0 !\n";
    check(!open(expanded),"reject cumulative expanded alias event budget before allocation");
    check(backend.Signals().empty() && !backend.TimeRange().valid,
          "resource-rejected VCD exposes no partial evidence");
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
