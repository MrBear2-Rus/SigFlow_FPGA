eda_add_plugin(eda-wave-vcd KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VcdWaveformBackend.cpp"
)

target_include_directories(eda-wave-vcd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-wave-vcd PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
