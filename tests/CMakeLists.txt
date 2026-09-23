# jobs 层冒烟测试：无 GUI，仅 wxBase + jsoncpp
set(JOB_TESTS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/job_tests.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/PlatformProcess.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/JobService.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/Sha256.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/ToolJobs.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/JobRegistry.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/ArtifactValidator.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaPackService.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaYosysLogParser.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrLogParser.cpp"
    "${SIGFLOW_MAIN_DIR}/platform/DynamicLibrary.cpp"
    "${SIGFLOW_MAIN_DIR}/platform/LocalPipe.cpp"
    "${SIGFLOW_3RD_DIR}/json/jsoncpp.cpp"
)

add_executable(job_tests ${JOB_TESTS_SOURCES})

# 插件化后 job_tests 直接编译 main/fpga/*.cpp（它们 #include 插件的头）
file(GLOB SIGFLOW_PLUGIN_INCLUDE_DIRS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/InnerPlugin/*/include")

target_include_directories(job_tests PRIVATE
    "${SIGFLOW_MAIN_DIR}"
    "${SIGFLOW_3RD_DIR}"
    "${CMAKE_SOURCE_DIR}/core/src"
    ${SIGFLOW_PLUGIN_INCLUDE_DIRS}
)

# 这些薄封装把实现委托给 core 与官方插件静态库，必须链接
foreach(_eda_lib eda_platform eda-synth-yosys eda-pnr-nextpnr eda-pack-gowin
                 eda-program-openfpgaloader eda-target-tangnano9k eda-cst-gowin-cst
                 eda-wave-vcd eda-lib-basic eda-sim-verilator)
    if(TARGET ${_eda_lib})
        target_link_libraries(job_tests PRIVATE ${_eda_lib})
    endif()
endforeach()

# ---- wxBase：本地源码 / 预编译 / 系统 三选一 ----
if(SIGFLOW_WX_LOCAL)
    if(WIN32)
        target_compile_definitions(job_tests PRIVATE
            WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
    endif()
    target_link_libraries(job_tests PRIVATE wx::base)
elseif(WIN32)
    target_include_directories(job_tests PRIVATE
        "${SIGFLOW_WX_INCLUDE_DIR}"
        "${SIGFLOW_WX_CONFIG_DIR}"
    )
    target_compile_definitions(job_tests PRIVATE
        WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
    find_library(JOB_TESTS_WX_BASE_LIBRARY NAMES wx_baseu-3.3 wx_baseu-3.2 wxbase33ud wxbase32ud
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(job_tests PRIVATE "${JOB_TESTS_WX_BASE_LIBRARY}")
    file(GLOB JOB_TESTS_WX_RUNTIME_DLLS
        "${SIGFLOW_WX_LIB_DIR}/../bin/*.dll"
        "${SIGFLOW_WX_LIB_DIR}/*.dll")
    foreach(WX_DLL ${JOB_TESTS_WX_RUNTIME_DLLS})
        add_custom_command(TARGET job_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WX_DLL}"
                    "$<TARGET_FILE_DIR:job_tests>/")
    endforeach()
else()
    foreach(SIGFLOW_WX_BASE_NAME IN ITEMS wx_baseu-3.2 wx_baseu-3.3 wx_baseu)
        find_library(JOB_TESTS_WX_BASE_LIBRARY NAMES ${SIGFLOW_WX_BASE_NAME})
        if(JOB_TESTS_WX_BASE_LIBRARY)
            break()
        endif()
    endforeach()
    if(NOT JOB_TESTS_WX_BASE_LIBRARY)
        message(WARNING
            "wxBase library not found; job_tests may fail to link. "
            "Install wxWidgets or set -DJOB_TESTS_WX_BASE_LIBRARY=<path>.")
    endif()
    target_link_libraries(job_tests PRIVATE ${JOB_TESTS_WX_BASE_LIBRARY})
endif()

# 线程（Linux）；Windows 无需额外系统库（哈希改用 jobs/Sha256）
if(NOT WIN32)
    target_link_libraries(job_tests PRIVATE Threads::Threads)
endif()

# 本地 wx 构建：把 wx DLL 拷到测试可执行文件旁
if(SIGFLOW_WX_LOCAL AND WIN32)
    file(GLOB_RECURSE JOB_TESTS_WX_LOCAL_DLLS
        "${CMAKE_BINARY_DIR}/wxWidgets/*.dll"
        "${CMAKE_BINARY_DIR}/wxWidgets/**/*.dll"
    )
    foreach(WX_DLL ${JOB_TESTS_WX_LOCAL_DLLS})
        add_custom_command(TARGET job_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WX_DLL}"
                    "$<TARGET_FILE_DIR:job_tests>/")
    endforeach()
endif()

add_test(NAME job_tests COMMAND job_tests)

# ---------------------------------------------------------------------------
# fpga_flow_probe：无 GUI 的 FPGA 工具链探针
#
# 直接调用 SigFlow 内部的 FPGA 代码路径（脚本生成 / 进程执行 / 日志解析 /
# 产物校验 / nextpnr Prepare+Finalize），不启动主程序。
# 只链接 wxBase，不碰任何 GUI 源文件。
# ---------------------------------------------------------------------------
set(FPGA_PROBE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/fpga_flow_probe.cpp"
    "${SIGFLOW_MAIN_DIR}/FpgaYosysRuntime.cpp"
    "${SIGFLOW_MAIN_DIR}/FpgaYosysScriptGenerator.cpp"
    "${SIGFLOW_MAIN_DIR}/FpgaYosysExecutor.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaYosysReport.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaYosysLogParser.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrExecutor.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrReport.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrLogParser.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/CstValidator.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaConstraint.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaPinData.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/ArtifactValidator.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/PlatformProcess.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/Sha256.cpp"
    "${SIGFLOW_3RD_DIR}/json/jsoncpp.cpp"
)

add_executable(fpga_flow_probe ${FPGA_PROBE_SOURCES})

target_include_directories(fpga_flow_probe PRIVATE
    "${SIGFLOW_MAIN_DIR}"
    "${SIGFLOW_3RD_DIR}"
)

if(SIGFLOW_WX_LOCAL)
    target_link_libraries(fpga_flow_probe PRIVATE wx::base)
else()
    target_link_libraries(fpga_flow_probe PRIVATE ${JOB_TESTS_WX_BASE_LIBRARY})
endif()

if(NOT WIN32)
    target_link_libraries(fpga_flow_probe PRIVATE Threads::Threads)
endif()

# 测试用的 Verilog 设计与约束文件要拷到可执行文件旁（探针按 ExecutableDir() 找）
add_custom_command(TARGET fpga_flow_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/fpga_flow"
        "$<TARGET_FILE_DIR:fpga_flow_probe>/fpga_flow"
)

# ---------------------------------------------------------------------------
# Harness P0 契约冒烟测试（无 GUI、无 wx/slang 依赖，仅 STL + nlohmann）
# ---------------------------------------------------------------------------
option(SIGFLOW_BUILD_CONTRACT_TESTS "Build the eda API contract smoke test" ON)
if(SIGFLOW_BUILD_CONTRACT_TESTS)
    add_subdirectory(contract)
    add_subdirectory(plugins)
endif()

# P2 双向同步回归（非 GUI）
add_subdirectory(ir)
