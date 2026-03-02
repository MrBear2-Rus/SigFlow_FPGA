// Stub for sc_time_stamp function required by Verilator
#include <cstdint>

#ifdef _WIN32
    #define DLLEXPORT __declspec(dllexport)
#else
    #define DLLEXPORT
#endif

static uint64_t g_sim_time = 0;

// Verilator 使用 C++ 链接（不是 extern "C"）
DLLEXPORT double sc_time_stamp() {
    return static_cast<double>(g_sim_time);
}

DLLEXPORT void advance_sim_time(uint64_t delta) {
    g_sim_time += delta;
}
