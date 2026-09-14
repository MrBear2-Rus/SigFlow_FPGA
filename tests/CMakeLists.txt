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
    "${SIGFLOW_3RD_DIR}/json/jsoncpp.cpp"
)

add_executable(job_tests ${JOB_TESTS_SOURCES})

target_include_directories(job_tests PRIVATE
    "${SIGFLOW_MAIN_DIR}"
    "${SIGFLOW_3RD_DIR}"
)

if(WIN32)
    target_include_directories(job_tests PRIVATE
        "${SIGFLOW_WX_INCLUDE_DIR}"
        "${SIGFLOW_WX_CONFIG_DIR}"
    )
    target_compile_definitions(job_tests PRIVATE
        WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)

    # MinGW：wx 为动态库（libwxbase33ud），链接导入库并在运行前拷贝 DLL
    find_library(JOB_TESTS_WX_BASE_LIBRARY NAMES wx_baseu-3.3 wx_baseu-3.2 wxbase33ud wxbase32ud
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(job_tests PRIVATE "${JOB_TESTS_WX_BASE_LIBRARY}" bcrypt)
    file(GLOB JOB_TESTS_WX_RUNTIME_DLLS
        "${SIGFLOW_WX_LIB_DIR}/../bin/*.dll"
        "${SIGFLOW_WX_LIB_DIR}/*.dll")
    foreach(WX_DLL ${JOB_TESTS_WX_RUNTIME_DLLS})
        add_custom_command(TARGET job_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WX_DLL}"
                    "$<TARGET_FILE_DIR:job_tests>/")
    endforeach()
else()
    # Linux / 其他 Unix：wx 头由根目录的 wxWidgets_USE_FILE 提供
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
    target_link_libraries(job_tests PRIVATE ${JOB_TESTS_WX_BASE_LIBRARY} Threads::Threads)
endif()

add_test(NAME job_tests COMMAND job_tests)
