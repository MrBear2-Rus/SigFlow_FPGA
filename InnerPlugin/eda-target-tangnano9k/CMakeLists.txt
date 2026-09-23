eda_add_plugin(eda-target-tangnano9k KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/PinDatabaseStore.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/TargetProfileStore.cpp"
)

target_include_directories(eda-target-tangnano9k PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-target-tangnano9k PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform")
