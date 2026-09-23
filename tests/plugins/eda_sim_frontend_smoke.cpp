#include "SimMainCodeGen.h"
#include "SimTimeline.h"
#include "StimulusModel.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

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

const char* kTestbench =
    "module tb;\n"
    "  reg clk;\n"
    "  reg rst;\n"
    "  wire out;\n"
    "  top uut(.clk(clk), .rst(rst), .out(out));\n"
    "  initial begin\n"
    "    clk = 0;\n"
    "    forever #5 clk = ~clk;\n"
    "  end\n"
    "  initial begin\n"
    "    rst = 1;\n"
    "    #10 rst = 0;\n"
    "    #20 $finish;\n"
    "  end\n"
    "endmodule\n";

} // namespace

int main() {
    // StimulusParser
    eda::sim::StimulusParser parser;
    eda::sim::TestbenchInfo info;
    Check(parser.ParseCode(kTestbench, info), "parses testbench code");
    Check(info.moduleName == "tb", "testbench module name");
    Check(info.topModuleName == "top" && info.topInstanceName == "uut", "DUT instance");
    Check(info.portMappings.size() == 3, "three port mappings");
    Check(!info.initialBlocks.empty(), "initial blocks found");
    bool hasClock = false;
    for (const auto& block : info.initialBlocks) {
        if (block.hasClock && block.clock.halfPeriod == 5 && block.clock.signalName == "clk") {
            hasClock = true;
        }
    }
    Check(hasClock, "clock definition detected (halfPeriod=5)");

    // TimelineGenerator
    eda::sim::TimelineGenerator timelineGenerator;
    const eda::sim::Timeline timeline = timelineGenerator.Generate(info, "top");
    Check(timeline.clock.valid && timeline.clock.signalName == "clk", "timeline clock mapped");
    Check(!timeline.events.empty(), "timeline has events");
    Check(timeline.maxSimTime >= 200, "timeline max sim time");

    // SimMainGenerator
    const fs::path dir = fs::temp_directory_path() / "eda_sim_frontend_test";
    std::error_code cleanupError;
    fs::remove_all(dir, cleanupError);
    fs::create_directories(dir);
    const fs::path mainPath = dir / "sim_main.cpp";
    eda::sim::SimMainGenerator mainGenerator;
    Check(mainGenerator.Generate(timeline, mainPath.string()), "generates sim_main.cpp");
    {
        std::ifstream input(mainPath);
        const std::string content((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        Check(content.find("#include \"Vtop.h\"") != std::string::npos, "includes Vtop header");
        Check(content.find("tfp->open") != std::string::npos, "opens VCD trace");
        Check(content.find("top->clk") != std::string::npos, "drives clock signal");
    }
    fs::remove_all(dir, cleanupError);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
