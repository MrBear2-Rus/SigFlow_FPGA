#include "Vreplay_demo_top.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

#include <cstdint>

double sc_time_stamp()
{
    return 0.0;
}

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    auto* top = new Vreplay_demo_top;
    auto* trace = new VerilatedVcdC;
    top->trace(trace, 99);
    trace->open("replay.vcd");

    vluint64_t time = 0;
    top->clk = 0;
    top->rst_n = 0;
    top->data = 1;
    top->eval();
    trace->dump(time);
    for (int cycle = 0; cycle < 32; ++cycle) {
        top->rst_n = cycle == 2 ? 1 : top->rst_n;
        top->data = static_cast<std::uint8_t>((cycle % 3) + 1);
        top->clk = 1;
        top->eval();
        trace->dump(++time);
        top->clk = 0;
        top->eval();
        trace->dump(++time);
    }
    trace->close();
    delete trace;
    delete top;
    return 0;
}
