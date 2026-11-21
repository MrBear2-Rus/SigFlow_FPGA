find_package(Python3 COMPONENTS Interpreter REQUIRED)
# The Phase 4 kernels are exercised through the shared real-DUT harness: the
# Python drivers assemble genuine Verilator runners from examples/virtual/phase4
# and talk to them over the production JSONL protocol.
add_test(NAME p4_i2c_spi_oled_and_bus_faults COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/virtual/verify_phase4.py" --build-dir "${CMAKE_BINARY_DIR}" --mode bus)
add_test(NAME p4_crc_tamper_and_cdc_approx COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/virtual/verify_phase4.py" --build-dir "${CMAKE_BINARY_DIR}" --mode protocols)
add_test(NAME p4_gui_oled_keypad_and_course05_acceptance COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/virtual/verify_phase4_gui.py" --build-dir "${CMAKE_BINARY_DIR}" --gui "$<TARGET_FILE:u1_m0_full_product_acceptance>")
set_tests_properties(p4_i2c_spi_oled_and_bus_faults p4_crc_tamper_and_cdc_approx p4_gui_oled_keypad_and_course05_acceptance PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
