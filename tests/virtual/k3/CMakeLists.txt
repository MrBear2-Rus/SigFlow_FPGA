add_executable(k3_live_trace_tests k3_live_trace_tests.cpp
    "${SIGFLOW_MAIN_DIR}/trace/LiveTraceSource.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/TraceCache.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/TraceMemoryBudget.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/WaveformBackendTraceSource.cpp")
target_include_directories(k3_live_trace_tests PRIVATE "${SIGFLOW_MAIN_DIR}/trace")
target_link_libraries(k3_live_trace_tests PRIVATE eda_virtual_board eda-wave-vcd)
add_test(NAME k3_live_trace_concurrency_and_cases COMMAND k3_live_trace_tests "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(k3_live_trace_concurrency_and_cases PROPERTIES TIMEOUT 120)
option(SIGFLOW_ENABLE_WSL_TSAN "Run instrumented LiveTrace test under WSL Ubuntu-24.04" OFF)
if(SIGFLOW_ENABLE_WSL_TSAN)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME k3_live_trace_thread_sanitizer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/virtual/verify_k3_tsan.py")
    set_tests_properties(k3_live_trace_thread_sanitizer PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
endif()
