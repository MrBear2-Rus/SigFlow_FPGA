cmake_minimum_required(VERSION 3.21)

project(SigFlow LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "This CMake bootstrap targets the current Windows implementation. "
        "Finish the POSIX platform layer before building on Linux or Kylin.")
endif()

set(SIGFLOW_MAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main")
set(SIGFLOW_3RD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
set(SIGFLOW_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugin_sdk")
set(SIGFLOW_SLANG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/slang-sdk" CACHE PATH
    "GCC/MSVC-compatible slang SDK root")
set(SIGFLOW_WX_ROOT "${SIGFLOW_3RD_DIR}/wxWidgets-3.2.9" CACHE PATH
    "GCC/MSVC-compatible wxWidgets root")
set(MINGW_ROOT "" CACHE PATH "MinGW installation root used for GCC runtime DLLs")

if(MSVC)
    set(SIGFLOW_SLANG_DIR "${SIGFLOW_SLANG_ROOT}")
    set(SIGFLOW_WX_DIR "${SIGFLOW_WX_ROOT}")
    set(SIGFLOW_WX_INCLUDE_DIR "${SIGFLOW_WX_DIR}/include")
    set(SIGFLOW_WX_CONFIG_DIR "${SIGFLOW_WX_DIR}/lib/vc_x64_dll/mswud")
    set(SIGFLOW_WX_LIB_DIR "${SIGFLOW_WX_DIR}/lib/vc_x64_dll")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
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
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
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

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
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
    "${SIGFLOW_WX_INCLUDE_DIR}"
    $<$<CXX_COMPILER_ID:MSVC>:${SIGFLOW_WX_INCLUDE_DIR}/msvc>
    "${SIGFLOW_WX_CONFIG_DIR}"
)

target_compile_definitions(sigflow PRIVATE
    WIN32
    _WINDOWS
    NOPCH
    $<$<CXX_COMPILER_ID:MSVC>:WXUSINGDLL>
    __WXMSW__
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    $<$<CXX_COMPILER_ID:MSVC>:SLANG_DEBUG>
    $<$<CXX_COMPILER_ID:GNU>:wxUSE_DPI_AWARE_MANIFEST=2>
)

if(MSVC)
    set_property(TARGET sigflow PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(sigflow PRIVATE /utf-8 /W3)
endif()

if(MSVC)
    target_link_directories(sigflow PRIVATE
        "${SIGFLOW_SLANG_DIR}"
        "${SIGFLOW_WX_LIB_DIR}"
    )
    target_link_libraries(sigflow PRIVATE
        svlang setupapi advapi32 bcrypt opengl32
        $<$<CONFIG:Debug>:wxmsw32ud_core>
        $<$<CONFIG:Debug>:wxmsw32ud_aui>
        $<$<CONFIG:Debug>:wxmsw32ud_stc>
        $<$<CONFIG:Debug>:wxmsw32ud_gl>
        $<$<CONFIG:Debug>:wxbase32ud>
        $<$<NOT:$<CONFIG:Debug>>:wxmsw32u_core>
        $<$<NOT:$<CONFIG:Debug>>:wxmsw32u_aui>
        $<$<NOT:$<CONFIG:Debug>>:wxmsw32u_stc>
        $<$<NOT:$<CONFIG:Debug>>:wxmsw32u_gl>
        $<$<NOT:$<CONFIG:Debug>>:wxbase32u>
    )
else()
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
    target_compile_definitions(sigflow PRIVATE WXUSINGDLL)
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
endif()

add_custom_command(TARGET sigflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SIGFLOW_MAIN_DIR}/res"
        "$<TARGET_FILE_DIR:sigflow>/res"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SIGFLOW_MAIN_DIR}/canvas_elements.json"
        "$<TARGET_FILE_DIR:sigflow>/canvas_elements.json"
)

if(MSVC)
    add_custom_command(TARGET sigflow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SIGFLOW_SLANG_DIR}/svlang.dll"
            "$<TARGET_FILE_DIR:sigflow>/svlang.dll"
    )
endif()

if(MSVC)
    file(GLOB SIGFLOW_WX_DLLS "${SIGFLOW_WX_LIB_DIR}/*.dll")
else()
    file(GLOB SIGFLOW_WX_DLLS
        "${SIGFLOW_WX_ROOT}/debug/bin/*.dll"
        "${SIGFLOW_WX_ROOT}/bin/*.dll"
    )
endif()
foreach(SIGFLOW_WX_DLL IN LISTS SIGFLOW_WX_DLLS)
    add_custom_command(TARGET sigflow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SIGFLOW_WX_DLL}"
            "$<TARGET_FILE_DIR:sigflow>/"
    )
endforeach()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND MINGW_ROOT)
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

if(MSVC)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT sigflow)
    set_property(TARGET sigflow PROPERTY VS_DPI_AWARE "PerMonitor")
    set_target_properties(sigflow PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
