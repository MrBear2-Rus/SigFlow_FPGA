eda_add_plugin(eda-cst-gowin-cst KIND STATIC SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ConstraintSheet.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CstFileValidator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GowinCst.cpp"
)

target_include_directories(eda-cst-gowin-cst PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(eda-cst-gowin-cst PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/src/eda-platform"
    "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-target-tangnano9k/include")
target_link_libraries(eda-cst-gowin-cst PUBLIC eda-target-tangnano9k)
