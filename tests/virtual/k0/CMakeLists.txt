cmake_minimum_required(VERSION 3.21)
project(SigFlowVirtualK0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(SIGFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(k0_generate_batch
    generate_batch.cpp
    "${SIGFLOW_ROOT}/InnerPlugin/eda-sim-verilator/src/SimMainCodeGen.cpp"
    "${SIGFLOW_ROOT}/core/src/eda-platform/Platform.cpp"
    "${SIGFLOW_ROOT}/core/src/eda-platform/Sha256.cpp")
target_include_directories(k0_generate_batch PRIVATE
    "${SIGFLOW_ROOT}/InnerPlugin/eda-sim-verilator/include"
    "${SIGFLOW_ROOT}/core/src")
add_executable(k1_baseline_cst_probe
    ../k1/baseline_cst_probe.cpp
    "${SIGFLOW_ROOT}/InnerPlugin/eda-cst-gowin-cst/src/ConstraintSheet.cpp"
    "${SIGFLOW_ROOT}/core/src/eda-platform/Platform.cpp"
    "${SIGFLOW_ROOT}/core/src/eda-platform/Sha256.cpp")
target_include_directories(k1_baseline_cst_probe PRIVATE
    "${SIGFLOW_ROOT}/InnerPlugin/eda-cst-gowin-cst/include"
    "${SIGFLOW_ROOT}/InnerPlugin/eda-target-tangnano9k/include"
    "${SIGFLOW_ROOT}/include" "${SIGFLOW_ROOT}/3rd" "${SIGFLOW_ROOT}/core/src"
    "${SIGFLOW_ROOT}/core/src/eda-platform")
