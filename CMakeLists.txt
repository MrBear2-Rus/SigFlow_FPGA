cmake_minimum_required(VERSION 3.21)

project(SigFlow LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SIGFLOW_MAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main")
set(SIGFLOW_3RD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
set(SIGFLOW_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugin_sdk")
# slang：预编译 SDK（回退用）与本地源码（推荐）
set(SIGFLOW_SLANG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/slang-sdk" CACHE PATH
    "GCC-compatible slang prebuilt SDK root (fallback)")
set(SIGFLOW_SLANG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/external/slang-src" CACHE PATH
    "slang source tree for a local (from-source) build")
# wxWidgets：预编译根（回退用）与本地源码（推荐）
set(SIGFLOW_WX_ROOT "${SIGFLOW_3RD_DIR}/wxWidgets-3.2.9" CACHE PATH
    "Prebuilt wxWidgets root (fallback)")
set(SIGFLOW_WX_SRC "${SIGFLOW_3RD_DIR}/wxWidgets-src" CACHE PATH
    "wxWidgets source tree for a local (from-source) build")
set(MINGW_ROOT "" CACHE PATH "MinGW installation root used for GCC runtime DLLs")

# ---------------------------------------------------------------------------
# 本地源码构建开关（双模式）：
#   ON  + 源码存在 → 从本地源码编译 wxWidgets / slang（Win/Linux 同一套）
#   ON  + 源码缺失 → 自动回退到预编译/系统
#   OFF            → 一律用预编译/系统
# ---------------------------------------------------------------------------
option(SIGFLOW_USE_LOCAL_WX
    "Build wxWidgets from SIGFLOW_WX_SRC instead of prebuilt/system wx" ON)
option(SIGFLOW_BUILD_SLANG_FROM_SOURCE
    "Build slang from SIGFLOW_SLANG_SRC instead of the prebuilt SDK" OFF)
option(SIGFLOW_SLANG_FETCH_DEPS
    "Let slang fetch fmt/boost/tomlplusplus via FetchContent instead of requiring system libs" OFF)
set(SIGFLOW_SLANG_DEP_URL_PREFIX "" CACHE STRING
    "Base URL prefix for slang dependency FetchContent (e.g. https://gitclone.com/github.com/)")

if(SIGFLOW_USE_LOCAL_WX AND EXISTS "${SIGFLOW_WX_SRC}/CMakeLists.txt")
    set(SIGFLOW_WX_LOCAL ON)
else()
    set(SIGFLOW_WX_LOCAL OFF)
endif()

if(SIGFLOW_BUILD_SLANG_FROM_SOURCE AND EXISTS "${SIGFLOW_SLANG_SRC}/CMakeLists.txt")
    set(SIGFLOW_SLANG_LOCAL ON)
else()
    set(SIGFLOW_SLANG_LOCAL OFF)
endif()

# ---------------------------------------------------------------------------
# 本项目只支持 CMake + GCC：
#   - Windows：MinGW-w64 GCC（-G "MinGW Makefiles"）
#   - Linux：系统 GCC
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "SigFlow targets CMake + GCC (MinGW on Windows, GCC on Linux). "
        "Detected compiler: ${CMAKE_CXX_COMPILER_ID}.")
endif()

if(WIN32)
    # ---- MinGW-w64 on Windows ----
    set(SIGFLOW_SLANG_DIR "${SIGFLOW_SLANG_ROOT}")
    if(NOT SIGFLOW_WX_LOCAL)
        set(SIGFLOW_WX_DIR "${SIGFLOW_WX_ROOT}")
        set(SIGFLOW_WX_INCLUDE_DIR "${SIGFLOW_WX_DIR}/include")
        set(SIGFLOW_WX_CONFIG_DIR "" CACHE PATH
            "GCC wxWidgets generated include directory containing wx/setup.h")
        set(SIGFLOW_WX_LIB_DIR "" CACHE PATH
            "GCC wxWidgets library directory")
        if(NOT EXISTS "${SIGFLOW_WX_CONFIG_DIR}/wx/setup.h")
            message(FATAL_ERROR
                "GCC build requires a wxWidgets GCC build. Set SIGFLOW_WX_CONFIG_DIR "
                "to its generated include directory containing wx/setup.h, or enable "
                "-DSIGFLOW_USE_LOCAL_WX=ON with a local wxWidgets source tree.")
        endif()
        if(NOT EXISTS "${SIGFLOW_WX_LIB_DIR}")
            message(FATAL_ERROR
                "GCC build requires SIGFLOW_WX_LIB_DIR pointing to the wxWidgets GCC "
                "libraries, or enable -DSIGFLOW_USE_LOCAL_WX=ON.")
        endif()
    endif()
    if(NOT SIGFLOW_SLANG_LOCAL AND NOT EXISTS "${SIGFLOW_SLANG_ROOT}/include/slang/ast/Compilation.h")
        message(FATAL_ERROR
            "GCC build requires a slang SDK with GCC-compatible libraries and headers, "
            "or enable -DSIGFLOW_BUILD_SLANG_FROM_SOURCE=ON with a local slang source tree.")
    endif()
else()
    # ---- Linux / 其他 Unix ----
    set(SIGFLOW_SLANG_DIR "${SIGFLOW_SLANG_ROOT}")
    set(SIGFLOW_WX_INCLUDE_DIR "")
    set(SIGFLOW_WX_CONFIG_DIR "")
    set(SIGFLOW_WX_LIB_DIR "")

    if(NOT SIGFLOW_WX_LOCAL)
        find_package(wxWidgets REQUIRED COMPONENTS core base aui stc gl xml)
        include(${wxWidgets_USE_FILE})
    endif()
    find_package(Threads REQUIRED)

    if(NOT SIGFLOW_SLANG_LOCAL AND NOT EXISTS "${SIGFLOW_SLANG_ROOT}/include/slang/ast/Compilation.h")
        message(WARNING
            "slang SDK headers not found under ${SIGFLOW_SLANG_ROOT}. Configure with "
            "-DSIGFLOW_SLANG_ROOT=<linux slang SDK> or -DSIGFLOW_BUILD_SLANG_FROM_SOURCE=ON.")
    endif()
endif()

# ---------------------------------------------------------------------------
# 本地源码构建 wxWidgets（推荐路径）
# ---------------------------------------------------------------------------
if(SIGFLOW_WX_LOCAL)
    message(STATUS "wxWidgets: building from local source ${SIGFLOW_WX_SRC}")
    set(wxBUILD_SHARED ON CACHE BOOL "" FORCE)
    set(wxBUILD_MONOLITHIC OFF CACHE BOOL "" FORCE)
    set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(wxBUILD_PRECOMP OFF CACHE BOOL "" FORCE)
    set(wxUSE_OPENGL ON CACHE BOOL "" FORCE)
    add_subdirectory("${SIGFLOW_WX_SRC}" "${CMAKE_BINARY_DIR}/wxWidgets" EXCLUDE_FROM_ALL)
else()
    message(STATUS "wxWidgets: using prebuilt/system wx (SIGFLOW_WX_ROOT=${SIGFLOW_WX_ROOT})")
endif()

# ---------------------------------------------------------------------------
# 本地源码构建 slang（推荐路径）
# ---------------------------------------------------------------------------
if(SIGFLOW_SLANG_LOCAL)
    message(STATUS "slang: building from local source ${SIGFLOW_SLANG_SRC}")
    set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
    set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
    set(SLANG_INCLUDE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SLANG_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
    # 依赖策略：默认用系统库；SIGFLOW_SLANG_FETCH_DEPS=ON 时改为 FetchContent（可配镜像前缀，先拉再编译）。
    if(SIGFLOW_SLANG_FETCH_DEPS)
        if(SIGFLOW_SLANG_DEP_URL_PREFIX)
            set(SLANG_DEP_URL_PREFIX "${SIGFLOW_SLANG_DEP_URL_PREFIX}" CACHE STRING "" FORCE)
        endif()
    else()
        set(SLANG_USE_SYSTEM_FMT ON CACHE BOOL "" FORCE)
        set(SLANG_USE_SYSTEM_BOOST ON CACHE BOOL "" FORCE)
        set(SLANG_USE_SYSTEM_TOMLPLUSPLUS ON CACHE BOOL "" FORCE)
    endif()
    set(SLANG_LIB_NAME "svlang" CACHE STRING "" FORCE)
    add_subdirectory("${SIGFLOW_SLANG_SRC}" "${CMAKE_BINARY_DIR}/slang" EXCLUDE_FROM_ALL)
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
)

if(SIGFLOW_SLANG_LOCAL)
    target_include_directories(sigflow PRIVATE
        "${SIGFLOW_SLANG_SRC}/include"
        "${SIGFLOW_SLANG_SRC}/external"      # vendored boost_unordered/expected 等 shim
        "${CMAKE_BINARY_DIR}/slang/source")   # slang 构建期生成的 slang_export.h 等
else()
    target_include_directories(sigflow PRIVATE "${SIGFLOW_SLANG_DIR}/include")
endif()

if(WIN32 AND NOT SIGFLOW_WX_LOCAL)
    target_include_directories(sigflow PRIVATE
        "${SIGFLOW_WX_INCLUDE_DIR}"
        "${SIGFLOW_WX_CONFIG_DIR}"
    )
endif()

if(WIN32)
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

# ---- 链接 ----
if(SIGFLOW_WX_LOCAL)
    target_link_libraries(sigflow PRIVATE wx::core wx::base wx::aui wx::stc wx::gl wx::xml)
elseif(WIN32)
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
        "${SIGFLOW_WX_CORE_LIBRARY}"
        "${SIGFLOW_WX_AUI_LIBRARY}"
        "${SIGFLOW_WX_STC_LIBRARY}"
        "${SIGFLOW_WX_GL_LIBRARY}"
        "${SIGFLOW_WX_BASE_LIBRARY}"
        "${SIGFLOW_WX_XML_LIBRARY}"
    )
else()
    target_link_libraries(sigflow PRIVATE
        ${wxWidgets_LIBRARIES}
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    find_package(OpenGL)
    if(OpenGL_FOUND)
        target_link_libraries(sigflow PRIVATE OpenGL::GL)
    else()
        message(WARNING "OpenGL not found; waveform GL rendering may fail to link on Linux.")
    endif()
endif()

if(SIGFLOW_SLANG_LOCAL)
    target_link_libraries(sigflow PRIVATE slang::slang)
elseif(WIN32)
    find_library(SIGFLOW_SLANG_LIBRARY NAMES svlang slang
        PATHS "${SIGFLOW_SLANG_ROOT}" "${SIGFLOW_SLANG_ROOT}/lib"
        NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(sigflow PRIVATE "${SIGFLOW_SLANG_LIBRARY}")
else()
    find_library(SIGFLOW_SLANG_LIBRARY NAMES svlang slang
        PATHS "${SIGFLOW_SLANG_DIR}/lib" "${SIGFLOW_SLANG_DIR}")
    if(NOT SIGFLOW_SLANG_LIBRARY)
        message(WARNING
            "libslang/svlang not found. Configure with -DSIGFLOW_SLANG_LIBRARY=<path> "
            "or install slang before building.")
    endif()
    target_link_libraries(sigflow PRIVATE ${SIGFLOW_SLANG_LIBRARY})
endif()

if(WIN32)
    target_link_libraries(sigflow PRIVATE setupapi advapi32 opengl32)
endif()

if(SIGFLOW_STRICT_WARNINGS AND NOT MSVC)
    target_compile_options(sigflow PRIVATE -Wall -Wextra)
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
    if(SIGFLOW_WX_LOCAL)
        # 本地 wx 构建时，**不能**用 file(GLOB) 去收集 wx 的 DLL：
        # GLOB 在 configure 期求值，而 wx 是 EXCLUDE_FROM_ALL 的子工程，
        # 全新配置时它的 DLL 还没被构建出来，GLOB 得到空列表 →
        # 一条拷贝命令都不会注册。结果是 sigflow.exe（GUI 子系统、无控制台）
        # 在加载阶段就因缺少 libwxmsw*.dll 被拒绝，而且普通 cmake --build
        # 永远修不好（只有重新 configure 才行）。
        # 改用生成器表达式，直接引用真实 target 的产物，构建后必然存在。
        set(SIGFLOW_WX_RUNTIME_TARGETS core base aui stc gl xml)
        foreach(SIGFLOW_WX_RUNTIME IN LISTS SIGFLOW_WX_RUNTIME_TARGETS)
            if(TARGET "wx::${SIGFLOW_WX_RUNTIME}")
                add_custom_command(TARGET sigflow POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:wx::${SIGFLOW_WX_RUNTIME}>"
                        "$<TARGET_FILE_DIR:sigflow>/"
                )
            endif()
        endforeach()
    else()
        # 预编译 wx：DLL 在 configure 时已经存在，GLOB 可用；
        # 但仍然显式提示"一个 DLL 都没找到"，避免再次静默漏拷。
        file(GLOB SIGFLOW_WX_DLLS
            "${SIGFLOW_WX_ROOT}/debug/bin/*.dll"
            "${SIGFLOW_WX_ROOT}/bin/*.dll"
            "${SIGFLOW_WX_LIB_DIR}/*.dll"
        )
        if(NOT SIGFLOW_WX_DLLS)
            message(WARNING
                "No wxWidgets DLLs found to copy next to sigflow.exe. "
                "Check SIGFLOW_WX_ROOT (${SIGFLOW_WX_ROOT}) / SIGFLOW_WX_LIB_DIR "
                "(${SIGFLOW_WX_LIB_DIR}); the executable will fail to start "
                "without them.")
        endif()
        foreach(SIGFLOW_WX_DLL IN LISTS SIGFLOW_WX_DLLS)
            add_custom_command(TARGET sigflow POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SIGFLOW_WX_DLL}"
                    "$<TARGET_FILE_DIR:sigflow>/"
            )
        endforeach()
    endif()
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
option(SIGFLOW_STRICT_WARNINGS
    "Enable -Wall -Wextra for the SigFlow target (portability sweep)" OFF)
if(SIGFLOW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 可选：FPGA 工具链（yosys / nextpnr / apicula）源码获取 + 本地编译
#
# 默认关闭，不影响普通构建；开启后：
#   cmake -S . -B build -DSIGFLOW_FETCH_FPGA_TOOLS=ON
#   cmake --build build --target sigflow_fpga_tools -j
# Windows 与 Linux 由 FetchContent 下载**同一套源码**，各自本地编译，
# 产物落位到 external/fpga-tools/runtime/，主程序无需改动即可找到。
# 详见 cmake/FpgaTools.cmake。
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FpgaTools.cmake")
