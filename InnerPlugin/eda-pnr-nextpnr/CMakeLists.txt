eda_add_plugin(eda-pnr-nextpnr KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NextpnrLogParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NextpnrReport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NextpnrPlaceRouter.cpp"
)

target_include_directories(eda-pnr-nextpnr PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-pnr-nextpnr PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
