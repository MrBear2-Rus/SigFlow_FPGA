// Stub for sc_time_stamp function required by Verilator
#include <cstdint>

static uint64_t g_sim_time = 0;

extern "C" double sc_time_stamp() {
    return static_cast<double>(g_sim_time);
}

void advance_sim_time(uint64_t delta) {
    g_sim_time += delta;
}
