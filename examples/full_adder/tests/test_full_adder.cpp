#include "Vtop.h"
#include "verilated.h"
#include "verilated_cov.h"
#include "verilated_vcd_c.h"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

double sc_time_stamp() {
    // Legacy fallback used only before the context has advanced from tick 0.
    return 0.0;
}

int main(int argc, char** argv) {
    VerilatedContext context;
    context.commandArgs(argc, argv);
    context.traceEverOn(true);
    Vtop dut{&context};
    VerilatedVcdC trace;
    dut.trace(&trace, 5);
    trace.open("full_adder.vcd");
    std::array<bool, 8> inputs{};
    std::array<bool, 4> outputs{};
    std::array<bool, 64> transitions{};
    std::array<bool, 10> toggles{}; // a/b/cin/sum/cout, both directions
    unsigned checks = 0;
    unsigned previous = 0;
    bool initialized = false;
    auto evaluate = [&](unsigned value) {
        dut.a = (value >> 2) & 1;
        dut.b = (value >> 1) & 1;
        dut.cin = value & 1;
        dut.eval();
        trace.dump(context.time());
        context.timeInc(1);
        // Independent arithmetic oracle, distinct from the gate implementation.
        const unsigned expected = unsigned(dut.a) + unsigned(dut.b) + unsigned(dut.cin);
        const unsigned actual = (unsigned(dut.cout) << 1) | unsigned(dut.sum);
        ++checks;
        if (actual != expected) {
            throw std::runtime_error("Mismatch at input " + std::to_string(value) +
                ": expected " + std::to_string(expected) + ", observed " + std::to_string(actual));
        }
        inputs[value] = true;
        outputs[actual] = true;
        const unsigned signals = (value << 2) | (unsigned(dut.sum) << 1) | unsigned(dut.cout);
        if (initialized) {
            for (unsigned bit = 0; bit < 5; ++bit) {
                const unsigned before = (previous >> bit) & 1;
                const unsigned after = (signals >> bit) & 1;
                if (before != after) toggles[bit * 2 + after] = true;
            }
        }
        initialized = true;
        previous = signals;
    };
    try {
        for (unsigned value = 0; value < 8; ++value) evaluate(value);
        for (unsigned from = 0; from < 8; ++from) {
            for (unsigned to = 0; to < 8; ++to) {
                evaluate(from);
                evaluate(to);
                transitions[from * 8 + to] = true;
                // A second evaluation without changing inputs checks stability.
                evaluate(to);
            }
        }
        const auto count = [](const auto& bins) {
            unsigned result = 0;
            for (bool hit : bins) result += hit;
            return result;
        };
        if (count(inputs) != 8 || count(outputs) != 4 || count(transitions) != 64 || count(toggles) != 10)
            throw std::runtime_error("Functional coverage bins are missing");
        dut.final();
        trace.close();
        context.coveragep()->write("coverage.dat");
        std::ofstream report("functional_coverage.json");
        report << "{\n  \"status\": \"passed\",\n  \"checks\": " << checks
               << ",\n  \"input_bins\": [8, 8],\n  \"output_bins\": [4, 4],"
               << "\n  \"ordered_transition_bins\": [64, 64],\n  \"signal_toggle_bins\": [10, 10],"
               << "\n  \"stable_cases\": 64,\n  \"logic_domain\": \"two_state\"\n}\n";
        if (!report) throw std::runtime_error("Cannot write coverage report");
        std::cout << "PASS: " << checks << " checks, inputs 8/8, transitions 64/64, toggles 10/10\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
