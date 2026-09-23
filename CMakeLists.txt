cmake_minimum_required(VERSION 3.21)

project(SigFlow LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SIGFLOW_MAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main")
set(SIGFLOW_3RD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
set(SIGFLOW_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugin_sdk")
# wxWidgets：预编译根（回退用）与本地源码（推荐）
set(SIGFLOW_WX_ROOT "${SIGFLOW_3RD_DIR}/wxWidgets-3.2.9" CACHE PATH
    "Prebuilt wxWidgets root (fallback)")
set(SIGFLOW_WX_SRC "${SIGFLOW_3RD_DIR}/wxWidgets-src" CACHE PATH
    "wxWidgets source tree for a local (from-source) build")
set(MINGW_ROOT "" CACHE PATH "MinGW installation root used for GCC runtime DLLs")

# ---------------------------------------------------------------------------
# 本地源码构建开关（双模式）：
#   ON  + 源码存在 → 从本地源码编译 wxWidgets（Win/Linux 同一套）
#   ON  + 源码缺失 → 自动回退到预编译/系统
#   OFF            → 一律用预编译/系统
# ---------------------------------------------------------------------------
option(SIGFLOW_USE_LOCAL_WX
    "Build wxWidgets from SIGFLOW_WX_SRC instead of prebuilt/system wx" ON)

if(SIGFLOW_USE_LOCAL_WX AND EXISTS "${SIGFLOW_WX_SRC}/CMakeLists.txt")
    set(SIGFLOW_WX_LOCAL ON)
else()
    set(SIGFLOW_WX_LOCAL OFF)
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
else()
    # ---- Linux / 其他 Unix ----
    set(SIGFLOW_WX_INCLUDE_DIR "")
    set(SIGFLOW_WX_CONFIG_DIR "")
    set(SIGFLOW_WX_LIB_DIR "")

    if(NOT SIGFLOW_WX_LOCAL)
        # 注意：必须包含 net —— wxWebRequest 在 wxNet（net/webrequest.cpp），
        # DeepSeek 插件用到它；缺了会导致插件链接时报 wxWebRequest 未定义。
        find_package(wxWidgets REQUIRED COMPONENTS core base aui stc gl xml net)
        include(${wxWidgets_USE_FILE})
    endif()
    find_package(Threads REQUIRED)
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
    # DeepSeek 插件走 wxWebRequest，但这里**不要**强设 wxUSE_WEBREQUEST：
    # wx 会在有后端时自动开启（Windows=WinHTTP；Linux=libcurl，需 libcurl 开发包），
    # 无后端时自动关闭。强行 set(... CACHE ... FORCE) 会触发 wx 的
    # wx_option_force_value 报 "FORCE specified without CACHE"（见 build/cmake/functions.cmake）。
    # 插件是否装配由下面的 find_package(CURL) 决定。
    add_subdirectory("${SIGFLOW_WX_SRC}" "${CMAKE_BINARY_DIR}/wxWidgets" EXCLUDE_FROM_ALL)
else()
    message(STATUS "wxWidgets: using prebuilt/system wx (SIGFLOW_WX_ROOT=${SIGFLOW_WX_ROOT})")
endif()

# ---------------------------------------------------------------------------
# P2-6：slang 死链路已删除（`UpdateTreeFromSlang`/`AsyncAnalysisCenter::LoadProject` 移除），
# 不再构建/链接 slang。`external/slang-src` 保留但不再参与构建。
# ---------------------------------------------------------------------------

# P0-7（R16）：显式源列表取代 file(GLOB_RECURSE)，消除 CMake/vcxproj 源集合漂移。
set(SIGFLOW_MAIN_SOURCES
    "${SIGFLOW_MAIN_DIR}/AsyncAnalysisCenter.cpp"
    "${SIGFLOW_MAIN_DIR}/BuildProgressBar.cpp"
    "${SIGFLOW_MAIN_DIR}/CanvasElement.cpp"
    "${SIGFLOW_MAIN_DIR}/CanvasEventHandler.cpp"
    "${SIGFLOW_MAIN_DIR}/CanvasModel.cpp"
    "${SIGFLOW_MAIN_DIR}/CanvasNoteBook.cpp"
    "${SIGFLOW_MAIN_DIR}/CanvasPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/CanvasTextElement.cpp"
    "${SIGFLOW_MAIN_DIR}/cMain.cpp"
    "${SIGFLOW_MAIN_DIR}/Composer.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/CaptureDecoder.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugAcquisition.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugBehaviorSummary.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugContract.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugContractConfigWindow.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugFingerprint.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugMappingBuilder.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugNetlistValidator.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugOverlayBuilder.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugProtocol.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugSession.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/FtdiTransport.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/PipeTransport.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/ReplayScenario.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/RootCauseGraph.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/SerialPortEnumerator.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/SerialTransport.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/TraceBridgeWindow.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/WaveformAligner.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/WaveformComparator.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/ArtifactValidator.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/CstValidator.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaConstraint.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaPackService.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaPinBindingPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaPinData.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaSynthesisJobsPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaToolWindow.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaYosysLogParser.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaYosysReport.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrExecutor.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrJob.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrJobsPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrLogParser.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/NextpnrReport.cpp"
    "${SIGFLOW_MAIN_DIR}/FpgaSynthesisJob.cpp"
    "${SIGFLOW_MAIN_DIR}/FpgaYosysExecutor.cpp"
    "${SIGFLOW_MAIN_DIR}/FpgaYosysRuntime.cpp"
    "${SIGFLOW_MAIN_DIR}/FpgaYosysScriptGenerator.cpp"
    "${SIGFLOW_MAIN_DIR}/HandyToolKit.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/JobRegistry.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/JobService.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/PlatformProcess.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/Sha256.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/ToolJobs.cpp"
    "${SIGFLOW_MAIN_DIR}/MainFrame.cpp"
    "${SIGFLOW_MAIN_DIR}/MainMenuBar.cpp"
    "${SIGFLOW_MAIN_DIR}/parse_progress.cpp"
    "${SIGFLOW_MAIN_DIR}/platform/DynamicLibrary.cpp"
    "${SIGFLOW_MAIN_DIR}/platform/LocalPipe.cpp"
    "${SIGFLOW_MAIN_DIR}/platform/SerialEnumerator.cpp"
    "${SIGFLOW_MAIN_DIR}/platform/SerialPort.cpp"
    "${SIGFLOW_MAIN_DIR}/platform/SimToolchain.cpp"
    "${SIGFLOW_MAIN_DIR}/PluginManager.cpp"
    "${SIGFLOW_MAIN_DIR}/ProjectStartWindow.cpp"
    "${SIGFLOW_MAIN_DIR}/ProjectTreePanel.cpp"
    "${SIGFLOW_MAIN_DIR}/PropertyPanelBuilder.cpp"
    "${SIGFLOW_MAIN_DIR}/SFNPropertyPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/SigFlowTreePanel.cpp"
    "${SIGFLOW_MAIN_DIR}/SigTextEditor.cpp"
    "${SIGFLOW_MAIN_DIR}/SigTree.cpp"
    "${SIGFLOW_MAIN_DIR}/Simulation/SimMainGenerator.cpp"
    "${SIGFLOW_MAIN_DIR}/Simulation/SimulationEngine.cpp"
    "${SIGFLOW_MAIN_DIR}/Simulation/StimulusParser.cpp"
    "${SIGFLOW_MAIN_DIR}/Simulation/TimelineGenerator.cpp"
    "${SIGFLOW_MAIN_DIR}/SVGParser.cpp"
    "${SIGFLOW_MAIN_DIR}/TerminalController.cpp"
    "${SIGFLOW_MAIN_DIR}/TerminalCtrl.cpp"
    "${SIGFLOW_MAIN_DIR}/ToolboxPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/ToolStateMachine.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/TraceCache.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/TraceMemoryBudget.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/TraceQueryService.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/TraceSidecarIndex.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/VcdLazyTraceSource.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/WaveformBackendTraceSource.cpp"
    "${SIGFLOW_MAIN_DIR}/TreeSitterLinter.cpp"
    "${SIGFLOW_MAIN_DIR}/VerilogManager.cpp"
    "${SIGFLOW_MAIN_DIR}/VerilogStructuring.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/TraceViewPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveAnalysis.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveCompareHub.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveformGLCanvas.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveformGLRenderer.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveformMiniMap.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveformRenderData.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveformTextLayer.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveformView.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WavePatternSearch.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveSession.cpp"
    "${SIGFLOW_MAIN_DIR}/wave/WaveUartLane.cpp"
    "${SIGFLOW_MAIN_DIR}/Wire.cpp"
)

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

# TraceBridge 在运行时需要 rtl/debug 下的三个调试 RTL（sf_micro_ila.sv /
# sf_uart_link.sv / sf_debug_link.sv）。把它部署到可执行目录，避免只在
# "从仓库根目录运行" 时才找得到（Linux/安装目录下从 build 目录直接启动会失败）。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rtl")
    add_custom_command(TARGET sigflow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/rtl"
            "$<TARGET_FILE_DIR:sigflow>/rtl"
    )
endif()

if(WIN32)
    if(SIGFLOW_WX_LOCAL)
        # 本地 wx 构建时，**不能**用 file(GLOB) 去收集 wx 的 DLL：
        # GLOB 在 configure 期求值，而 wx 是 EXCLUDE_FROM_ALL 的子工程，
        # 全新配置时它的 DLL 还没被构建出来，GLOB 得到空列表 →
        # 一条拷贝命令都不会注册。结果是 sigflow.exe（GUI 子系统、无控制台）
        # 在加载阶段就因缺少 libwxmsw*.dll 被拒绝，而且普通 cmake --build
        # 永远修不好（只有重新 configure 才行）。
        # 改用生成器表达式，直接引用真实 target 的产物，构建后必然存在。
        set(SIGFLOW_WX_RUNTIME_TARGETS core base aui stc gl xml net)
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

# ---------------------------------------------------------------------------
# Harness P0：核心↔插件编译期契约（独立于 sigflow 目标的源集合）
#   eda_api  —— header-only 契约（include/eda/api）INTERFACE target
#   eda_core —— 最小内核静态库（Logger / BuildInfo）
# ---------------------------------------------------------------------------
add_library(eda_api INTERFACE)
target_include_directories(eda_api INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SIGFLOW_3RD_DIR}"
)
target_compile_features(eda_api INTERFACE cxx_std_20)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EdaPlugin.cmake")
add_subdirectory(core)
add_subdirectory(InnerPlugin/eda-synth-yosys)
add_subdirectory(InnerPlugin/eda-pnr-nextpnr)
add_subdirectory(InnerPlugin/eda-pack-gowin)
add_subdirectory(InnerPlugin/eda-program-openfpgaloader)
add_subdirectory(InnerPlugin/eda-sim-verilator)
add_subdirectory(InnerPlugin/eda-target-tangnano9k)
add_subdirectory(InnerPlugin/eda-cst-gowin-cst)
add_subdirectory(InnerPlugin/eda-wave-vcd)
add_subdirectory(InnerPlugin/eda-lib-basic)

# P0-8：主程序复用核心库（Composer 使用 eda::PluginHost）。
target_include_directories(sigflow PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/core/src")
target_link_libraries(sigflow PRIVATE eda_core)
# P1-1：迁移期 main/ 的 wx 适配层调用插件；后续 P1-8 由 IJobService 取代。
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-synth-yosys/include")
target_link_libraries(sigflow PRIVATE eda-synth-yosys)
# P1-2：迁移期 main/ 的 nextpnr 日志/报告 wx 适配层调用插件。
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-pnr-nextpnr/include")
target_link_libraries(sigflow PRIVATE eda-pnr-nextpnr)
# P1-3：迁移期 main/ 的 pack 记账 wx 适配层调用插件。
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-pack-gowin/include")
target_link_libraries(sigflow PRIVATE eda-pack-gowin)
# P1-5：迁移期 main/ 的目标 profile 解析调用插件。
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-target-tangnano9k/include")
target_link_libraries(sigflow PRIVATE eda-target-tangnano9k)
# P1-6：迁移期 main/ 的约束模型/校验 wx 适配层调用插件。
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-cst-gowin-cst/include")
target_link_libraries(sigflow PRIVATE eda-cst-gowin-cst)
# P1-4：迁移期 main/ 的仿真模型/生成器 wx 适配层调用插件。
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-sim-verilator/include")
target_link_libraries(sigflow PRIVATE eda-sim-verilator)
# P1-8：插件注册锚点（eda_plugin_force_link_*）需要所有官方插件库参与链接，
# 否则静态自注册 TU 被链接器丢弃，RegisterBuiltins 为空。
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-program-openfpgaloader/include")
target_link_libraries(sigflow PRIVATE eda-program-openfpgaloader)
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-wave-vcd/include")
target_link_libraries(sigflow PRIVATE eda-wave-vcd)
target_include_directories(sigflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/eda-lib-basic/include")
target_link_libraries(sigflow PRIVATE eda-lib-basic)
# P1-5：目标 profile / 引脚库 JSON 随可执行文件分发。
add_custom_command(TARGET sigflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/main/fpga/target-profiles"
        "$<TARGET_FILE_DIR:sigflow>/target-profiles")

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

# =============================================================================
# 调试 UART 的 libftdi 直连传输层（可选，见 main/debug/FtdiTransport.cpp）
#
# Tang Nano 9K 的板载 BL702 只是"模仿" FT2232：实测同一个 PING 帧，
#   * libftdi 直连（绕过内核驱动）能收到合法 Pong；而 /dev/ttyUSBx(ftdi_sio)
#     路径完全没有应答 —— 模拟层没有真正实现 ftdi_sio 用的 UART 厂商命令；
#   * 且 libftdi 打开设备会 detach ftdi_sio，烧录后调试串口的 tty 常常消失。
# 因此调试 UART 优先走 libftdi 直连。libftdi 由 FPGA 工具链模块源码构建并落位在
#   external/fpga-tools/runtime/deps/libftdi
# 找不到时本传输层不编译，自动回退到串口传输层。
# =============================================================================
set(SIGFLOW_LIBFTDI_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/fpga-tools/runtime/deps/libftdi"
    CACHE PATH "libftdi 安装前缀（默认用 FPGA 工具链的源码构建产物）")

find_path(SIGFLOW_LIBFTDI_INCLUDE_DIR NAMES libftdi1/ftdi.h
          HINTS "${SIGFLOW_LIBFTDI_ROOT}/include")
find_library(SIGFLOW_LIBFTDI_LIBRARY NAMES ftdi1
             HINTS "${SIGFLOW_LIBFTDI_ROOT}/lib")

if(SIGFLOW_LIBFTDI_INCLUDE_DIR AND SIGFLOW_LIBFTDI_LIBRARY)
    target_include_directories(sigflow PRIVATE "${SIGFLOW_LIBFTDI_INCLUDE_DIR}")
    target_link_libraries(sigflow PRIVATE "${SIGFLOW_LIBFTDI_LIBRARY}")
    target_compile_definitions(sigflow PRIVATE SIGFLOW_HAVE_LIBFTDI=1)
    set_target_properties(sigflow PROPERTIES
        BUILD_RPATH "${SIGFLOW_LIBFTDI_ROOT}/lib"
        INSTALL_RPATH "${SIGFLOW_LIBFTDI_ROOT}/lib")
    message(STATUS "[debug] libftdi 直连传输层: 启用 (${SIGFLOW_LIBFTDI_LIBRARY})")
else()
    message(STATUS "[debug] libftdi 直连传输层: 未找到 libftdi，调试 UART 回退到串口")
endif()

# ---------------------------------------------------------------------------
# 双版本（Edu/Pro）与可选插件
#
# 单代码库、双构建目标：SIGFLOW_EDITION 先落地"版本标识"（编译期宏
# SIGFLOW_EDITION），后续特性开关/发布包按它分化。DeepSeek AI 助手为
# 两版共享能力，由 SIGFLOW_BUILD_DEEPSEEK_PLUGIN 控制是否装配。
# ---------------------------------------------------------------------------
set(SIGFLOW_EDITION "dev" CACHE STRING "产品版本：edu | pro | dev（同一代码库、双构建目标）")
set_property(CACHE SIGFLOW_EDITION PROPERTY STRINGS edu pro dev)
target_compile_definitions(sigflow PRIVATE SIGFLOW_EDITION="${SIGFLOW_EDITION}")

# DeepSeek 插件默认"能装就装"：
#   Windows —— wxWebRequest 用自带 WinHTTP，直接默认 ON；
#   Linux   —— 需要 libcurl（wxWebRequest 的 curl 后端）且 wx 确实启用了它。
# 下面同时把"到底缺哪一项"记下来，未装配时打印出来，便于排查。
set(_sigflow_wx_setup "")
set(_sigflow_wx_webrequest "unknown")

if(WIN32)
    set(_sigflow_deepseek_default ON)
else()
    find_package(CURL QUIET)

    # 定位 wx/setup.h：系统 wx（include 目录 + wx-config）+ 本地源码构建（构建目录）
    set(_sigflow_wx_dirs "")
    if(DEFINED wxWidgets_INCLUDE_DIRS)
        list(APPEND _sigflow_wx_dirs ${wxWidgets_INCLUDE_DIRS})
    endif()
    find_program(_sigflow_wx_config NAMES wx-config)
    if(_sigflow_wx_config)
        execute_process(COMMAND ${_sigflow_wx_config} --cxxflags
                        OUTPUT_VARIABLE _sigflow_wxflags ERROR_QUIET)
        string(REGEX MATCHALL "-I[^ \t\r\n]+" _sigflow_wxincs "${_sigflow_wxflags}")
        foreach(_inc ${_sigflow_wxincs})
            string(SUBSTRING "${_inc}" 2 -1 _p)
            list(APPEND _sigflow_wx_dirs "${_p}")
        endforeach()
    endif()
    file(GLOB _sigflow_wxgen "${CMAKE_BINARY_DIR}/wxWidgets/lib/wx/include/*/wx/setup.h")
    list(APPEND _sigflow_wx_dirs ${_sigflow_wxgen})

    foreach(_dir ${_sigflow_wx_dirs})
        set(_candidate "${_dir}")
        if(NOT _candidate MATCHES "setup\\.h$")
            set(_candidate "${_dir}/wx/setup.h")
        endif()
        if(EXISTS "${_candidate}")
            set(_sigflow_wx_setup "${_candidate}")
            break()
        endif()
    endforeach()

    if(_sigflow_wx_setup)
        file(STRINGS "${_sigflow_wx_setup}" _wr_line REGEX "^#define[ \t]+wxUSE_WEBREQUEST[ \t]+")
        if(_wr_line MATCHES "wxUSE_WEBREQUEST[ \t]+1")
            set(_sigflow_wx_webrequest "1")
        else()
            set(_sigflow_wx_webrequest "0")
        endif()
    endif()

    if(CURL_FOUND AND _sigflow_wx_webrequest STREQUAL "1")
        set(_sigflow_deepseek_default ON)
    else()
        set(_sigflow_deepseek_default OFF)
    endif()
endif()

option(SIGFLOW_BUILD_DEEPSEEK_PLUGIN
       "构建 DeepSeek AI 助手插件（跨平台 MODULE；需要 wx 启用 wxUSE_WEBREQUEST）"
       ${_sigflow_deepseek_default})
if(SIGFLOW_BUILD_DEEPSEEK_PLUGIN)
    add_subdirectory(Plugin_DeepSeek)
    message(STATUS "[edition] ${SIGFLOW_EDITION} + DeepSeek 插件已装配")
else()
    if(WIN32)
        set(_deepseek_reason "已手动关闭（-DSIGFLOW_BUILD_DEEPSEEK_PLUGIN=OFF）")
    elseif(NOT CURL_FOUND)
        set(_deepseek_reason
            "未找到 libcurl 开发文件（curl-config / CURLConfig.cmake）；"
            "Ubuntu: sudo apt install libcurl4-openssl-dev，Arch: sudo pacman -S curl")
    elseif(_sigflow_wx_webrequest STREQUAL "0")
        set(_deepseek_reason
            "当前 wx 未启用 wxWebRequest（${_sigflow_wx_setup}）；需带 libcurl 重新构建 wx")
    elseif(_sigflow_wx_webrequest STREQUAL "unknown")
        set(_deepseek_reason
            "无法定位 wx/setup.h，无法判定 wxWebRequest（可 -DSIGFLOW_BUILD_DEEPSEEK_PLUGIN=ON 试编）")
    elseif(_sigflow_wx_webrequest STREQUAL "1")
        set(_deepseek_reason
            "缓存中记录为 OFF（当前环境其实已具备条件）：用 "
            "-U SIGFLOW_BUILD_DEEPSEEK_PLUGIN 清除后重新配置，或 -DSIGFLOW_BUILD_DEEPSEEK_PLUGIN=ON")
    else()
        set(_deepseek_reason
            "未知（curl=${CURL_FOUND}, wxUSE_WEBREQUEST=${_sigflow_wx_webrequest}, "
            "setup=${_sigflow_wx_setup}）")
    endif()
    message(STATUS "[edition] ${SIGFLOW_EDITION}（DeepSeek 插件未装配：${_deepseek_reason}）")
endif()
