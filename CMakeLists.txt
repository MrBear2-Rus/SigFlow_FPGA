cmake_minimum_required(VERSION 3.21)

project(SigFlow LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SIGFLOW_MAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main")
set(SIGFLOW_3RD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
set(SIGFLOW_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugin_sdk")
set(SIGFLOW_SLANG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/slang-sdk" CACHE PATH
    "GCC-compatible slang SDK root")
set(SIGFLOW_WX_ROOT "${SIGFLOW_3RD_DIR}/wxWidgets-3.2.9" CACHE PATH
    "GCC-compatible wxWidgets root")
set(MINGW_ROOT "" CACHE PATH "MinGW installation root used for GCC runtime DLLs")

# ---------------------------------------------------------------------------
# 本项目只支持 CMake + GCC：
#   - Windows：MinGW-w64 GCC（-G "MinGW Makefiles"）
#   - Linux：系统 GCC
# 注意：业务代码仍含 Win32 依赖，Linux 目前只保证 configure 通过，
#       代码移植（#ifdef 双实现 / 平台层）是后续独立工作。
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "SigFlow targets CMake + GCC (MinGW on Windows, GCC on Linux). "
        "Detected compiler: ${CMAKE_CXX_COMPILER_ID}.")
endif()

if(WIN32)
    # ---- MinGW-w64 on Windows ----
    set(SIGFLOW_SLANG_DIR "${SIGFLOW_SLANG_ROOT}")
    set(SIGFLOW_WX_DIR "${SIGFLOW_WX_ROOT}")
    set(SIGFLOW_WX_INCLUDE_DIR "${SIGFLOW_WX_DIR}/include")
    set(SIGFLOW_WX_CONFIG_DIR "" CACHE PATH
        "GCC wxWidgets generated include directory containing wx/setup.h")
    set(SIGFLOW_WX_LIB_DIR "" CACHE PATH
        "GCC wxWidgets library directory")
    if(NOT EXISTS "${SIGFLOW_WX_CONFIG_DIR}/wx/setup.h")
        message(FATAL_ERROR
            "GCC build requires a wxWidgets GCC build. Set SIGFLOW_WX_CONFIG_DIR "
            "to its generated include directory containing wx/setup.h.")
    endif()
    if(NOT EXISTS "${SIGFLOW_WX_LIB_DIR}")
        message(FATAL_ERROR
            "GCC build requires SIGFLOW_WX_LIB_DIR pointing to the wxWidgets GCC libraries.")
    endif()
    if(NOT EXISTS "${SIGFLOW_SLANG_ROOT}/include/slang/ast/Compilation.h")
        message(FATAL_ERROR
            "GCC build requires a slang SDK with GCC-compatible libraries and headers.")
    endif()
else()
    # ---- Linux / 其他 Unix ----
    set(SIGFLOW_SLANG_DIR "${SIGFLOW_SLANG_ROOT}")
    set(SIGFLOW_WX_INCLUDE_DIR "")
    set(SIGFLOW_WX_CONFIG_DIR "")
    set(SIGFLOW_WX_LIB_DIR "")

    find_package(wxWidgets REQUIRED COMPONENTS core base aui stc gl)
    include(${wxWidgets_USE_FILE})
    find_package(Threads REQUIRED)

    if(NOT EXISTS "${SIGFLOW_SLANG_ROOT}/include/slang/ast/Compilation.h")
        message(WARNING
            "slang SDK headers not found under ${SIGFLOW_SLANG_ROOT}. "
            "Configure with -DSIGFLOW_SLANG_ROOT=<linux slang SDK> before building.")
    endif()
endif()

file(GLOB_RECURSE SIGFLOW_MAIN_SOURCES CONFIGURE_DEPENDS
    "${SIGFLOW_MAIN_DIR}/*.cpp"
)
list(FILTER SIGFLOW_MAIN_SOURCES EXCLUDE REGEX "/research/")
list(FILTER SIGFLOW_MAIN_SOURCES EXCLUDE REGEX "/x64/")
list(FILTER SIGFLOW_MAIN_SOURCES EXCLUDE REGEX "/bin/")
list(FILTER SIGFLOW_MAIN_SOURCES EXCLUDE REGEX "/archive/")
list(FILTER SIGFLOW_MAIN_SOURCES EXCLUDE REGEX "/Simulation/sc_time_stub\\.cpp$")
list(FILTER SIGFLOW_MAIN_SOURCES EXCLUDE REGEX "/SFNodePropertyPanel\\.cpp$")
list(FILTER SIGFLOW_MAIN_SOURCES EXCLUDE REGEX "/WavePanel\\.cpp$")

set(SIGFLOW_THIRD_PARTY_SOURCES
    "${SIGFLOW_3RD_DIR}/json/jsoncpp.cpp"
    "${SIGFLOW_3RD_DIR}/tree-sitter-verilog/src/parser.c"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/src/lib.c"
    "${SIGFLOW_3RD_DIR}/vcd/vcd.cpp"
)

add_executable(sigflow WIN32
    ${SIGFLOW_MAIN_SOURCES}
    ${SIGFLOW_THIRD_PARTY_SOURCES}
)

if(WIN32)
    target_sources(sigflow PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sigflow.rc")
endif()

target_include_directories(sigflow PRIVATE
    "${SIGFLOW_MAIN_DIR}"
    "${SIGFLOW_PLUGIN_DIR}"
    "${SIGFLOW_3RD_DIR}"
    "${SIGFLOW_3RD_DIR}/vcd"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/include"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/src"
    "${SIGFLOW_3RD_DIR}/tree-sitter-verilog/src"
    "${SIGFLOW_3RD_DIR}/nlohmann"
    "${SIGFLOW_SLANG_DIR}/include"
)

if(WIN32)
    target_include_directories(sigflow PRIVATE
        "${SIGFLOW_WX_INCLUDE_DIR}"
        "${SIGFLOW_WX_CONFIG_DIR}"
    )
    target_compile_definitions(sigflow PRIVATE
        WIN32
        _WINDOWS
        NOPCH
        WXUSINGDLL
        __WXMSW__
        UNICODE
        _UNICODE
        wxUSE_DPI_AWARE_MANIFEST=2
    )
endif()

if(WIN32)
    find_library(SIGFLOW_SLANG_LIBRARY NAMES svlang slang
        PATHS "${SIGFLOW_SLANG_ROOT}" "${SIGFLOW_SLANG_ROOT}/lib"
        NO_DEFAULT_PATH REQUIRED)
    find_library(SIGFLOW_WX_CORE_LIBRARY NAMES wx_mswu_core-3.2 wxmsw33ud_core wxmsw32ud_core wxmsw32u_core
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(SIGFLOW_WX_AUI_LIBRARY NAMES wx_mswu_aui-3.2 wxmsw33ud_aui wxmsw32ud_aui wxmsw32u_aui
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(SIGFLOW_WX_STC_LIBRARY NAMES wx_mswu_stc-3.2 wxmsw33ud_stc wxmsw32ud_stc wxmsw32u_stc
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(SIGFLOW_WX_GL_LIBRARY NAMES wx_mswu_gl-3.2 wxmsw33ud_gl wxmsw32ud_gl wxmsw32u_gl
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(SIGFLOW_WX_BASE_LIBRARY NAMES wx_baseu-3.2 wxbase33ud wxbase32ud wxbase32u
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(SIGFLOW_WX_XML_LIBRARY NAMES wx_baseu_xml-3.2 wxbase33ud_xml wxbase32ud_xml wxbase32u_xml
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(sigflow PRIVATE
        "${SIGFLOW_SLANG_LIBRARY}"
        "${SIGFLOW_WX_CORE_LIBRARY}"
        "${SIGFLOW_WX_AUI_LIBRARY}"
        "${SIGFLOW_WX_STC_LIBRARY}"
        "${SIGFLOW_WX_GL_LIBRARY}"
        "${SIGFLOW_WX_BASE_LIBRARY}"
        "${SIGFLOW_WX_XML_LIBRARY}"
        setupapi advapi32 bcrypt opengl32
    )
else()
    find_library(SIGFLOW_SLANG_LIBRARY NAMES svlang slang
        PATHS "${SIGFLOW_SLANG_DIR}/lib" "${SIGFLOW_SLANG_DIR}")
    if(NOT SIGFLOW_SLANG_LIBRARY)
        message(WARNING
            "libslang/svlang not found. Configure with -DSIGFLOW_SLANG_LIBRARY=<path> "
            "or install slang before building.")
    endif()
    target_link_libraries(sigflow PRIVATE
        ${SIGFLOW_SLANG_LIBRARY}
        ${wxWidgets_LIBRARIES}
        Threads::Threads
    )
endif()

add_custom_command(TARGET sigflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SIGFLOW_MAIN_DIR}/res"
        "$<TARGET_FILE_DIR:sigflow>/res"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SIGFLOW_MAIN_DIR}/canvas_elements.json"
        "$<TARGET_FILE_DIR:sigflow>/canvas_elements.json"
)

if(WIN32)
    file(GLOB SIGFLOW_WX_DLLS
        "${SIGFLOW_WX_ROOT}/debug/bin/*.dll"
        "${SIGFLOW_WX_ROOT}/bin/*.dll"
        "${SIGFLOW_WX_LIB_DIR}/*.dll"
    )
    foreach(SIGFLOW_WX_DLL IN LISTS SIGFLOW_WX_DLLS)
        add_custom_command(TARGET sigflow POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SIGFLOW_WX_DLL}"
                "$<TARGET_FILE_DIR:sigflow>/"
        )
    endforeach()
endif()

if(WIN32 AND MINGW_ROOT)
    foreach(SIGFLOW_MINGW_DLL IN ITEMS
        "${MINGW_ROOT}/bin/libgcc_s_seh-1.dll"
        "${MINGW_ROOT}/bin/libstdc++-6.dll"
        "${MINGW_ROOT}/bin/libwinpthread-1.dll"
    )
        if(EXISTS "${SIGFLOW_MINGW_DLL}")
            add_custom_command(TARGET sigflow POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SIGFLOW_MINGW_DLL}"
                    "$<TARGET_FILE_DIR:sigflow>/"
            )
        endif()
    endforeach()
endif()

option(SIGFLOW_BUILD_TESTS "Build the jobs-layer smoke tests" ON)
if(SIGFLOW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
