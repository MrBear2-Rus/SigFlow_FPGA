eda_add_plugin(eda-synth-yosys KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/YosysScriptGenerator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/YosysArtifactValidator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/YosysSynthesizer.cpp"
)

target_include_directories(eda-synth-yosys PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-synth-yosys PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
