eda_add_plugin(eda-pack-gowin KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GowinPackService.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GowinPacker.cpp"
)

target_include_directories(eda-pack-gowin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-pack-gowin PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
