// Stub for sc_time_stamp function required by Verilator
#include <cstdint>

// 注意：Verilator 在 C++20 模式下使用普通 C++ 链接
// 不需要 extern "C"，也不需要 __declspec(dllexport)
// 因为最终链接到同一个 DLL 中

static uint64_t g_sim_time = 0;

double sc_time_stamp() {
    return static_cast<double>(g_sim_time);
}

void advance_sim_time(uint64_t delta) {
    g_sim_time += delta;
}
