add_executable(k2_session_tests k2_session_tests.cpp)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
target_link_libraries(k2_session_tests PRIVATE eda-cst-gowin-cst eda_virtual_board)
add_test(NAME k2_session_pause_guard_replay COMMAND k2_session_tests "${CMAKE_SOURCE_DIR}")
set_tests_properties(k2_session_pause_guard_replay PROPERTIES TIMEOUT 60)
add_test(NAME k2_real_verilated_session COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/virtual/verify_k2.py" --build-dir "${CMAKE_BINARY_DIR}")
set_tests_properties(k2_real_verilated_session PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
