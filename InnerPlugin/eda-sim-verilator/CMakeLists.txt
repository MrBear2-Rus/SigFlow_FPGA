eda_add_plugin(eda-sim-verilator KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SimMainCodeGen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SimTimeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/StimulusModel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VerilatorRunJob.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VerilatorSimulator.cpp"
)

target_include_directories(eda-sim-verilator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-sim-verilator PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
