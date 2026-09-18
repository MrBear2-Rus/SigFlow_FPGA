#include "Vmulticlock_demo_top.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

#include <cstdint>
#include <iostream>

double sc_time_stamp()
{
    return 0.0;
}

static vluint64_t sim_time = 0;

static void dump(Vmulticlock_demo_top* top, VerilatedVcdC* trace)
{
    top->eval();
    trace->dump(sim_time++);
}

static void tick_ctrl(Vmulticlock_demo_top* top, VerilatedVcdC* trace)
{
    top->ctrl_clk = 1;
    dump(top, trace);
    top->ctrl_clk = 0;
    dump(top, trace);
}

static void tick_a(Vmulticlock_demo_top* top, VerilatedVcdC* trace)
{
    top->clk_a = 1;
    dump(top, trace);
    top->clk_a = 0;
    dump(top, trace);
}

static void tick_b(Vmulticlock_demo_top* top, VerilatedVcdC* trace)
{
    top->clk_b = 1;
    dump(top, trace);
    top->clk_b = 0;
    dump(top, trace);
}

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    auto* top = new Vmulticlock_demo_top;
    auto* trace = new VerilatedVcdC;
    top->trace(trace, 99);
    trace->open("multiclock.vcd");

    top->ctrl_clk = 0;
    top->clk_a = 0;
    top->clk_b = 0;
    top->rst_n = 0;
    top->ctrl_req_a = 0;
    top->ctrl_req_b = 0;
    top->ctrl_payload_a = 0;
    top->ctrl_payload_b = 0;
    top->arm_a = 0;
    top->arm_b = 0;
    top->rd_addr_a = 0;
    top->rd_addr_b = 0;
    dump(top, trace);
    for (int cycle = 0; cycle < 3; ++cycle) {
        tick_ctrl(top, trace);
        tick_a(top, trace);
        tick_b(top, trace);
    }
    top->rst_n = 1;

    top->ctrl_payload_a = 0x3c;
    top->ctrl_payload_b = 0xa7;
    top->ctrl_req_a = 1;
    top->ctrl_req_b = 1;
    tick_ctrl(top, trace);
    top->ctrl_req_a = 0;
    top->ctrl_req_b = 0;

    bool saw_pulse_a = false;
    bool saw_pulse_b = false;
    bool saw_ack_a = false;
    bool saw_ack_b = false;
    for (int cycle = 0; cycle < 20; ++cycle) {
        tick_a(top, trace);
        saw_pulse_a = saw_pulse_a || top->sample_pulse_a;
        tick_ctrl(top, trace);
        saw_ack_a = saw_ack_a || top->ctrl_ack_a;
        tick_b(top, trace);
        saw_pulse_b = saw_pulse_b || top->sample_pulse_b;
        tick_ctrl(top, trace);
        saw_ack_b = saw_ack_b || top->ctrl_ack_b;
    }
    if (!saw_pulse_a || !saw_pulse_b || top->sample_payload_a != 0x3c || top->sample_payload_b != 0xa7) {
        std::cerr << "CDC FAIL pulse/payload: "
                  << saw_pulse_a << "/" << saw_pulse_b << " "
                  << std::hex << static_cast<unsigned>(top->sample_payload_a) << "/"
                  << static_cast<unsigned>(top->sample_payload_b) << std::dec << "\n";
        return 1;
    }
    if (top->ctrl_busy_a || top->ctrl_busy_b || !saw_ack_a || !saw_ack_b) {
        std::cerr << "CDC FAIL completion/busy: busy=" << static_cast<unsigned>(top->ctrl_busy_a)
                  << "/" << static_cast<unsigned>(top->ctrl_busy_b)
                  << " ack=" << saw_ack_a << "/" << saw_ack_b << "\n";
        return 1;
    }

    top->arm_a = 1;
    top->arm_b = 1;
    tick_a(top, trace);
    tick_b(top, trace);
    top->arm_a = 0;
    top->arm_b = 0;
    for (int cycle = 0; cycle < 12 && (!top->done_a || !top->done_b); ++cycle) {
        tick_a(top, trace);
        tick_b(top, trace);
    }
    if (!top->done_a || !top->done_b || !top->triggered_a || !top->triggered_b) {
        std::cerr << "CAPTURE FAIL done/trigger: " << top->done_a << "/" << top->done_b
                  << " " << top->triggered_a << "/" << top->triggered_b << "\n";
        return 1;
    }

    top->rd_addr_a = 0;
    top->rd_addr_b = 0;
    tick_a(top, trace);
    tick_b(top, trace);
    const auto first_a = static_cast<unsigned>(top->rd_data_a);
    const auto first_b = static_cast<unsigned>(top->rd_data_b);
    if (first_a == 0 || first_b == 0 || first_a == first_b) {
        std::cerr << "CAPTURE FAIL readback: " << first_a << "/" << first_b << "\n";
        return 1;
    }

    trace->close();
    delete trace;
    delete top;
    std::cout << "TRACEBRIDGE MULTICLOCK VERILATOR PASS\n"
              << "CDC payloads: 0x3c / 0xa7\n"
              << "Capture first samples: " << first_a << " / " << first_b << "\n";
    return 0;
}
