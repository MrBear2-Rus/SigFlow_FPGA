#include "SimMainCodeGen.h"

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    eda::sim::Timeline timeline;
    timeline.topModuleName = "k0_counter";
    timeline.clock = {"clock_in", 5, "0", true};
    timeline.maxSimTime = 120;
    timeline.events = {
        {0, "clock_in", "0", "initial clock"},
        {0, "reset_n", "0", "hold reset"},
        {0, "gate_in", "0", "disabled"},
        {21, "reset_n", "1", "release between edges"},
        {21, "gate_in", "1", "enable"}
    };
    eda::sim::SimMainGenerator generator;
    if (!generator.Generate(timeline, argv[1], "batch.vcd")) {
        std::cerr << generator.LastError() << '\n';
        return 1;
    }
    std::cout << "Generated using the unchanged baseline SimMainGenerator\n";
}
