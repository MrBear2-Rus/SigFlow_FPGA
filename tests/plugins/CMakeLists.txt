add_executable(eda_synth_yosys_smoke eda_synth_yosys_smoke.cpp)
target_link_libraries(eda_synth_yosys_smoke PRIVATE eda_core eda_platform eda-synth-yosys)
target_include_directories(eda_synth_yosys_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-synth-yosys/include")

add_test(NAME eda_synth_yosys_smoke COMMAND eda_synth_yosys_smoke)

add_executable(eda_pnr_nextpnr_smoke eda_pnr_nextpnr_smoke.cpp)
target_link_libraries(eda_pnr_nextpnr_smoke PRIVATE eda_core eda_platform eda-pnr-nextpnr)
target_include_directories(eda_pnr_nextpnr_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-pnr-nextpnr/include")

add_test(NAME eda_pnr_nextpnr_smoke COMMAND eda_pnr_nextpnr_smoke)

add_executable(eda_pack_flash_smoke eda_pack_flash_smoke.cpp)
target_link_libraries(eda_pack_flash_smoke PRIVATE
    eda_core eda_platform eda-pack-gowin eda-program-openfpgaloader)
target_include_directories(eda_pack_flash_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-pack-gowin/include"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-program-openfpgaloader/include")

add_test(NAME eda_pack_flash_smoke COMMAND eda_pack_flash_smoke)

add_executable(eda_sim_verilator_smoke eda_sim_verilator_smoke.cpp)
target_link_libraries(eda_sim_verilator_smoke PRIVATE eda_core eda_platform eda-sim-verilator)
target_include_directories(eda_sim_verilator_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-sim-verilator/include")

add_test(NAME eda_sim_verilator_smoke COMMAND eda_sim_verilator_smoke)

add_executable(eda_sim_frontend_smoke eda_sim_frontend_smoke.cpp)
target_link_libraries(eda_sim_frontend_smoke PRIVATE eda_core eda_platform eda-sim-verilator)
target_include_directories(eda_sim_frontend_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-sim-verilator/include")

add_test(NAME eda_sim_frontend_smoke COMMAND eda_sim_frontend_smoke)

add_executable(eda_target_profile_smoke eda_target_profile_smoke.cpp)
target_link_libraries(eda_target_profile_smoke PRIVATE eda_core eda_platform eda-target-tangnano9k)
target_include_directories(eda_target_profile_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-target-tangnano9k/include")

add_test(NAME eda_target_profile_smoke
    COMMAND eda_target_profile_smoke
            "${CMAKE_SOURCE_DIR}/main/fpga/target-profiles/tang-nano-9k.json"
            "${CMAKE_SOURCE_DIR}/main/fpga/target-profiles/tang-nano-9k.pins.json")

add_executable(eda_gowin_cst_smoke eda_gowin_cst_smoke.cpp)
target_link_libraries(eda_gowin_cst_smoke PRIVATE eda_core eda_platform eda-cst-gowin-cst)
target_include_directories(eda_gowin_cst_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-cst-gowin-cst/include")

add_test(NAME eda_gowin_cst_smoke COMMAND eda_gowin_cst_smoke)

add_executable(eda_constraint_sheet_smoke eda_constraint_sheet_smoke.cpp)
target_link_libraries(eda_constraint_sheet_smoke PRIVATE
    eda_core eda_platform eda-cst-gowin-cst eda-target-tangnano9k)
target_include_directories(eda_constraint_sheet_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-cst-gowin-cst/include"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-target-tangnano9k/include")

add_test(NAME eda_constraint_sheet_smoke
    COMMAND eda_constraint_sheet_smoke
            "${CMAKE_SOURCE_DIR}/main/fpga/target-profiles/tang-nano-9k.pins.json")

add_executable(eda_declarative_tool_smoke eda_declarative_tool_smoke.cpp)
target_link_libraries(eda_declarative_tool_smoke PRIVATE eda_core eda_platform)
target_include_directories(eda_declarative_tool_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src")

add_test(NAME eda_declarative_tool_smoke COMMAND eda_declarative_tool_smoke)

add_executable(eda_wave_vcd_smoke eda_wave_vcd_smoke.cpp)
target_link_libraries(eda_wave_vcd_smoke PRIVATE eda_core eda_platform eda-wave-vcd)
target_include_directories(eda_wave_vcd_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-wave-vcd/include")

add_test(NAME eda_wave_vcd_smoke COMMAND eda_wave_vcd_smoke)

add_executable(eda_wave_trace_adapter_smoke
    eda_wave_trace_adapter_smoke.cpp
    "${CMAKE_SOURCE_DIR}/main/trace/WaveformBackendTraceSource.cpp")
target_link_libraries(eda_wave_trace_adapter_smoke PRIVATE eda_core eda_platform eda-wave-vcd)
target_include_directories(eda_wave_trace_adapter_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/main"
    "${CMAKE_SOURCE_DIR}/main/trace"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-wave-vcd/include")

add_test(NAME eda_wave_trace_adapter_smoke COMMAND eda_wave_trace_adapter_smoke)

add_executable(eda_declarative_switch_smoke eda_declarative_switch_smoke.cpp)
target_link_libraries(eda_declarative_switch_smoke PRIVATE
    eda_core eda_platform eda-sim-verilator)
target_include_directories(eda_declarative_switch_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-sim-verilator/include")

add_test(NAME eda_declarative_switch_smoke COMMAND eda_declarative_switch_smoke)

add_executable(eda_component_library_smoke eda_component_library_smoke.cpp)
target_link_libraries(eda_component_library_smoke PRIVATE eda_core eda_platform eda-lib-basic)
target_include_directories(eda_component_library_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-lib-basic/include")

add_test(NAME eda_component_library_smoke
    COMMAND eda_component_library_smoke
            "${CMAKE_SOURCE_DIR}/main/canvas_elements.json")

add_executable(eda_command_bus_smoke eda_command_bus_smoke.cpp)
target_link_libraries(eda_command_bus_smoke PRIVATE eda_core eda_platform)
target_include_directories(eda_command_bus_smoke PRIVATE "${CMAKE_SOURCE_DIR}/core/src")

add_test(NAME eda_command_bus_smoke COMMAND eda_command_bus_smoke)
