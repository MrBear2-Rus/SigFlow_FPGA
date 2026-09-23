eda_add_plugin(eda-program-openfpgaloader KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/OpenFpgaLoaderProgrammer.cpp"
)

target_include_directories(eda-program-openfpgaloader PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-program-openfpgaloader PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
