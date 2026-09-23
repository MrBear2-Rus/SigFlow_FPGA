add_executable(eda_api_contract_smoke eda_api_contract_smoke.cpp)
target_link_libraries(eda_api_contract_smoke PRIVATE eda_core eda_platform)
target_include_directories(eda_api_contract_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src")

add_test(NAME eda_api_contract_smoke COMMAND eda_api_contract_smoke)

add_executable(eda_job_service_smoke job_service_smoke.cpp)
target_link_libraries(eda_job_service_smoke PRIVATE eda_core eda_platform)
target_include_directories(eda_job_service_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src")

add_test(NAME eda_job_service_smoke COMMAND eda_job_service_smoke)

add_executable(eda_toolchain_project_smoke toolchain_project_smoke.cpp)
target_link_libraries(eda_toolchain_project_smoke PRIVATE eda_core eda_platform)
target_include_directories(eda_toolchain_project_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src")

add_test(NAME eda_toolchain_project_smoke COMMAND eda_toolchain_project_smoke)

# ---------------------------------------------------------------------------
# P0-3/P0-7 插件宿主：三个测试用动态插件（MODULE，经 eda_add_plugin 构建）
#   eda_test_plugin_ok          -> ABI/指纹匹配，应 Ready
#   eda_test_plugin_bad_build   -> 指纹不匹配，应 Error（修 R13）
#   eda_test_plugin_reject_abi  -> 拒绝握手，应 Error（修 R1）
# ---------------------------------------------------------------------------
set(EDA_TEST_PLUGIN_DIR "${CMAKE_BINARY_DIR}/test-plugins")

eda_add_plugin(eda_test_plugin_ok KIND MODULE SOURCES test_plugin_ok.cpp)
eda_add_plugin(eda_test_plugin_bad_build KIND MODULE SOURCES test_plugin_bad_build.cpp)
eda_add_plugin(eda_test_plugin_reject_abi KIND MODULE SOURCES test_plugin_reject_abi.cpp)

foreach(plugin_target eda_test_plugin_ok eda_test_plugin_bad_build eda_test_plugin_reject_abi)
    set_target_properties(${plugin_target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${EDA_TEST_PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${EDA_TEST_PLUGIN_DIR}")
endforeach()

add_executable(eda_plugin_host_smoke plugin_host_smoke.cpp)
target_link_libraries(eda_plugin_host_smoke PRIVATE eda_core)
target_include_directories(eda_plugin_host_smoke PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_dependencies(eda_plugin_host_smoke
    eda_test_plugin_ok eda_test_plugin_bad_build eda_test_plugin_reject_abi)
add_test(NAME eda_plugin_host_smoke COMMAND eda_plugin_host_smoke "${EDA_TEST_PLUGIN_DIR}")
