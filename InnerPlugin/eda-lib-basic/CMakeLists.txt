eda_add_plugin(eda-lib-basic KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/BasicComponentLibrary.cpp"
)

target_include_directories(eda-lib-basic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-lib-basic PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
