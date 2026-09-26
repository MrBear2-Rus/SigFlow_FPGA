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
# 第三方获取策略（方案 C：本地优先 → 系统 → FetchContent 兜底）
#   SIGFLOW_ALLOW_FETCH  缺失时是否允许从网络拉取（麒麟离线交付设 OFF）
#   SIGFLOW_FETCH_MIRROR 镜像前缀，替换 https://github.com/（空=直连；可指内网镜像）
# 统一使用 FetchContent_Declare + FetchContent_MakeAvailable
# （兼容 CMake 3.21–3.28–4.x；不使用已弃用的单参 FetchContent_Populate）。
# ---------------------------------------------------------------------------
option(SIGFLOW_ALLOW_FETCH "Allow FetchContent from network when local/system is missing" ON)
set(SIGFLOW_FETCH_MIRROR "" CACHE STRING
    "URL prefix that replaces https://github.com/ for FetchContent (e.g. internal mirror)")

# eda_mirror_url(<var>): 就地把 https://github.com/ 前缀替换为镜像前缀。
function(eda_mirror_url var)
    if(SIGFLOW_FETCH_MIRROR)
        string(REPLACE "https://github.com/" "${SIGFLOW_FETCH_MIRROR}/" _u "${${var}}")
        set(${var} "${_u}" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 本地源码构建开关（双模式）：
#   ON  + 源码存在 → 从本地源码编译 wxWidgets（Win/Linux 同一套）
#   ON  + 源码缺失 → 允许 FetchContent 拉取（受 SIGFLOW_ALLOW_FETCH 约束），
#                    否则回退预编译/系统 wx
#   OFF            → 一律用预编译/系统
# ---------------------------------------------------------------------------
option(SIGFLOW_USE_LOCAL_WX
    "Build wxWidgets from SIGFLOW_WX_SRC (local, or FetchContent fallback)" ON)

# wxWidgets 源码：本地优先；缺失且允许拉取时 FetchContent（3.28/4.x 兼容写法）。
set(SIGFLOW_WX_FETCH_TAG "v3.2.9" CACHE STRING "wxWidgets git tag for FetchContent fallback")
set(SIGFLOW_WX_FETCH_REPO "https://github.com/wxWidgets/wxWidgets.git" CACHE STRING
    "wxWidgets git repository for FetchContent fallback")

# ---------------------------------------------------------------------------
# wxWidgets 源码获取：FetchContent（取代"系统库 / 预编译库"）
#
# 规则：
#   1) SIGFLOW_WX_SRC 里已有源码（例如 3rd/wxWidgets-src）→ 直接用，不下载；
#   2) 否则 SIGFLOW_FETCH_WX=ON（默认）→ FetchContent 下载到构建树，并把
#      SIGFLOW_WX_SRC 指向它 → 于是下面的 SIGFLOW_WX_LOCAL 分支照常生效；
#   3) 否则（显式关掉下载）→ 回退到原来的"预编译/系统 wx"分支，向后兼容。
#
# 两个关键实现细节：
#   * 这里只用 FetchContent 把源码【取下来】，真正的 add_subdirectory 仍由下面
#     原有的 SIGFLOW_WX_LOCAL 分支负责 —— 否则同一个 wx 会被加入两次（目标重名）。
#     CMake 4 已移除单参 FetchContent_Populate()，因此用 SOURCE_SUBDIR 指向一个
#     不存在的子目录，实现"只下载、不 add_subdirectory"。
#   * 本块必须放在 SIGFLOW_WX_LOCAL 判定【之前】，否则它仍会是 OFF，下载完也用不上。
#
# 离线/内网：-DSIGFLOW_FETCH_WX=OFF 关闭下载；或
#           -DFETCHCONTENT_SOURCE_DIR_SIGFLOW_WX=<已解压的 wx 源码目录> 直接喂源码。
# ---------------------------------------------------------------------------
option(SIGFLOW_FETCH_WX
    "Fetch wxWidgets source via FetchContent when no local source tree is present" ON)
set(SIGFLOW_WX_TAG "v3.2.9" CACHE STRING
    "wxWidgets git tag to fetch (应与 3rd/wxWidgets-* 的版本一致)")
set(SIGFLOW_WX_URL
    "https://codeload.github.com/wxWidgets/wxWidgets/tar.gz/refs/tags/${SIGFLOW_WX_TAG}"
    CACHE STRING "wxWidgets 源码包 URL")
# wx 的 src/generic/bmpsvg.cpp 需要 3rdparty/nanosvg/src/{nanosvg.h,nanosvgrast.h}，
# 而 codeload 的 tarball 不含 git 子模块内容。这里额外取一份 nanosvg 源码补进去，
# 避免依赖 `git submodule update`（也不需要 GitHub API，codeload 直连即可）。
set(SIGFLOW_NANOSVG_URL
    "https://codeload.github.com/memononen/nanosvg/tar.gz/refs/heads/master"
    CACHE STRING "nanosvg 源码包 URL（用于补齐 wx 的 3rdparty/nanosvg）")

if(SIGFLOW_USE_LOCAL_WX AND SIGFLOW_FETCH_WX AND NOT EXISTS "${SIGFLOW_WX_SRC}/CMakeLists.txt")
    include(FetchContent)
    message(STATUS "wxWidgets: 本地无源码，改用 FetchContent 下载 ${SIGFLOW_WX_TAG}")
    FetchContent_Declare(sigflow_wx
        URL           "${SIGFLOW_WX_URL}"
        SOURCE_SUBDIR "_sigflow_fetch_only_"   # 不存在的子目录 = 只下载不构建
    )
    FetchContent_MakeAvailable(sigflow_wx)
    if(EXISTS "${sigflow_wx_SOURCE_DIR}/CMakeLists.txt")
        # 用普通变量覆盖同名 CACHE 项：普通变量优先级更高，且会传递给子目录
        set(SIGFLOW_WX_SRC "${sigflow_wx_SOURCE_DIR}")
        message(STATUS "wxWidgets: 源码就绪 -> ${SIGFLOW_WX_SRC}")

        # ---- 补齐 wx 需要的 3rdparty/nanosvg ----
        # bmpsvg.cpp 的守卫是：
        #   #if defined(wxHAS_SVG) && !wxUSE_NANOSVG_EXTERNAL
        #       #if !__has_include("../../3rdparty/nanosvg/src/nanosvg.h")
        #           #error You need to run "git submodule update --init 3rdparty/nanosvg".
        # tarball 不含子模块 → 必须自己放进去，否则 wxcore 编不过。
        # 需要两个头：nanosvg.h（解析）与 nanosvgrast.h（栅格化）。
        if(NOT EXISTS "${SIGFLOW_WX_SRC}/3rdparty/nanosvg/src/nanosvg.h")
            FetchContent_Declare(sigflow_nanosvg
                URL           "${SIGFLOW_NANOSVG_URL}"
                SOURCE_SUBDIR "_sigflow_fetch_only_"   # 只下载不构建
            )
            FetchContent_MakeAvailable(sigflow_nanosvg)
            if(EXISTS "${sigflow_nanosvg_SOURCE_DIR}/src/nanosvg.h")
                file(MAKE_DIRECTORY "${SIGFLOW_WX_SRC}/3rdparty/nanosvg/src")
                file(COPY
                     "${sigflow_nanosvg_SOURCE_DIR}/src/nanosvg.h"
                     "${sigflow_nanosvg_SOURCE_DIR}/src/nanosvgrast.h"
                     DESTINATION "${SIGFLOW_WX_SRC}/3rdparty/nanosvg/src")
                message(STATUS "wxWidgets: 已补齐 3rdparty/nanosvg（nanosvg.h + nanosvgrast.h）")
            else()
                message(WARNING
                    "wxWidgets: 未能取得 nanosvg 源码，wxcore 编译会报 "
                    "#error You need to run \"git submodule update --init 3rdparty/nanosvg\"。\n"
                    "  可手动放置到 ${SIGFLOW_WX_SRC}/3rdparty/nanosvg/src/ ，"
                    "或改用 -DSIGFLOW_FETCH_WX=OFF -DSIGFLOW_USE_LOCAL_WX=OFF（系统 wx）。")
            endif()
        else()
            message(STATUS "wxWidgets: 3rdparty/nanosvg 已存在，跳过补齐")
        endif()
    else()
        message(WARNING "wxWidgets: FetchContent 未取得源码，将回退到预编译/系统 wx")
    endif()
endif()

if(SIGFLOW_USE_LOCAL_WX AND EXISTS "${SIGFLOW_WX_SRC}/CMakeLists.txt")
    set(SIGFLOW_WX_LOCAL ON)
    set(SIGFLOW_WX_FETCHED OFF)
elseif(SIGFLOW_USE_LOCAL_WX AND SIGFLOW_ALLOW_FETCH AND NOT WIN32)
    # 麒麟/Linux：本地源码缺失时自动拉取 wx 源码（绕开发行版沙箱的运行时路径问题）。
    include(FetchContent)
    set(_wx_repo "${SIGFLOW_WX_FETCH_REPO}")
    eda_mirror_url(_wx_repo)
    message(STATUS "wxWidgets: local source missing, fetching ${_wx_repo}@${SIGFLOW_WX_FETCH_TAG}")
    FetchContent_Declare(wxWidgets
        GIT_REPOSITORY "${_wx_repo}"
        GIT_TAG "${SIGFLOW_WX_FETCH_TAG}"
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(wxWidgets)
    if(EXISTS "${wxWidgets_SOURCE_DIR}/CMakeLists.txt")
        set(SIGFLOW_WX_SRC "${wxWidgets_SOURCE_DIR}" CACHE PATH "" FORCE)
        set(SIGFLOW_WX_LOCAL ON)
        set(SIGFLOW_WX_FETCHED ON)
    else()
        set(SIGFLOW_WX_LOCAL OFF)
    endif()
else()
    set(SIGFLOW_WX_LOCAL OFF)
    set(SIGFLOW_WX_FETCHED OFF)
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
    if(UNIX AND NOT APPLE)
        # wxGTK 的源码构建依赖系统 GTK3 开发包 —— 这部分无法由 FetchContent 提供。
        # 缺了会在 wx 的 configure 阶段报错，这里提前给出可操作的提示。
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(_sigflow_gtk3 QUIET gtk+-3.0)
            if(NOT _sigflow_gtk3_FOUND)
                message(WARNING
                    "wxWidgets 从源码构建需要 GTK3 开发包，但未找到 gtk+-3.0。\n"
                    "  Arch:   sudo pacman -S gtk3\n"
                    "  Debian: sudo apt install libgtk-3-dev\n"
                    "  （若想改用系统 wxWidgets：-DSIGFLOW_FETCH_WX=OFF -DSIGFLOW_USE_LOCAL_WX=OFF）")
            endif()
        endif()
    endif()
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
    if(SIGFLOW_WX_FETCHED)
        # 已由 FetchContent_MakeAvailable 加入构建，勿重复 add_subdirectory。
        message(STATUS "wxWidgets: already added via FetchContent")
    else()
        add_subdirectory("${SIGFLOW_WX_SRC}" "${CMAKE_BINARY_DIR}/wxWidgets" EXCLUDE_FROM_ALL)
    endif()
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

# -----------------------------------------------------------------------------
# 插件化（P0）后，main/ 下很多 .cpp 变成薄封装，直接 #include 插件/核心的头：
#   main/fpga/FpgaPinData.cpp         -> PinDatabaseStore.h      (eda-target-tangnano9k)
#   main/FpgaYosysScriptGenerator.cpp -> YosysScriptGenerator.h  (eda-synth-yosys)
#   main/fpga/CstValidator.cpp        -> CstFileValidator.h      (eda-cst-gowin-cst)
#   main/fpga/FpgaConstraint.cpp      -> ConstraintSheet.h       (eda-cst-gowin-cst)
#   main/fpga/NextpnrLogParser.cpp    -> NextpnrLogModel.h       (eda-pnr-nextpnr)
#   main/jobs/Sha256.cpp              -> eda-platform/Sha256.h   (core/src)
# 这些路径此前只加给了 sigflow 目标，于是【其它目标】(job_tests / fpga_flow_probe ...)
# 全部编译失败；实现委托给插件静态库后还会出现 undefined reference。
# 这里统一加入 —— 新增目标不会再漏（Windows 走 main.vcxproj，不受影响）。
# -----------------------------------------------------------------------------
file(GLOB SIGFLOW_PLUGIN_INCLUDE_DIRS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/InnerPlugin/*/include")
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/core/src"
    # 插件/核心头里会 #include "eda/api/*.hpp"（冻结 C ABI 与类型定义）
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${SIGFLOW_PLUGIN_INCLUDE_DIRS}
)

# -----------------------------------------------------------------------------
# tree-sitter 抽成共享静态库：sig_tree_sync_smoke 之前自编的 parser.c.o 里
# 没有 tree_sitter_verilog 符号（与 sigflow 编译选项不一致），改为复用同一份。
# -----------------------------------------------------------------------------
add_library(sigflow_tree_sitter STATIC
    "${SIGFLOW_3RD_DIR}/tree-sitter-verilog/src/parser.c"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/src/lib.c")
target_include_directories(sigflow_tree_sitter PUBLIC
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/include"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/src"
    "${SIGFLOW_3RD_DIR}/tree-sitter-verilog/src")

# 同理：所有可执行目标都要链接 core 与官方插件静态库。
# 放在 add_subdirectory(InnerPlugin/...) 之后：插件自身不会被重复链接。
foreach(_eda_dep
        eda_platform
        eda-synth-yosys
        eda-pnr-nextpnr
        eda-pack-gowin
        eda-program-openfpgaloader
        eda-target-tangnano9k
        eda-cst-gowin-cst
        eda-wave-vcd
        eda-lib-basic
        eda-sim-verilator)
    if(TARGET ${_eda_dep})
        link_libraries(${_eda_dep})
    endif()
endforeach()

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
# 教育版 Agent（SIGFLOW_BUILD_EDU_AGENT=ON 时链接 EDA Gateway）。
if(TARGET eda_agent_gateway)
    target_compile_definitions(sigflow PRIVATE SIGFLOW_BUILD_EDU_AGENT=1)
    target_link_libraries(sigflow PRIVATE eda_agent_gateway)
endif()
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

# =============================================================================
# 运行库自洽落位（Linux）
#
# sigflow 链接的系统库（libpcre2-32、libftdi1 等"不太常用"的包）并非所有
# 运行环境都装了——典型是 openKylin 的 KARE 沙箱终端，/usr 是独立快照，
# 启动即报 "cannot open shared object file: libXXX.so.N"。
# 链接后对二进制做一次 ldd，把非基础、非项目内的依赖整体拷进 RUNPATH
# 指向的目录（见 cmake/StageRuntimeLibs.cmake），使 build 产物自洽。
# =============================================================================
set(SIGFLOW_RUNTIME_LIBS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/fpga-tools/runtime/deps/sigflow-libs"
    CACHE PATH "sigflow 系统侧运行库的落位目录（RPATH 指向）")
set_property(TARGET sigflow APPEND PROPERTY BUILD_RPATH "${SIGFLOW_RUNTIME_LIBS_DIR}")
set_property(TARGET sigflow APPEND PROPERTY INSTALL_RPATH "${SIGFLOW_RUNTIME_LIBS_DIR}")
if(UNIX AND NOT APPLE)
    # 关键：DT_RUNPATH 只作用于可执行文件的**直接** NEEDED；间接依赖
    #（如 libpcre2-32，NEEDED 于 libwx_baseu，而 wx 库自身无 RPATH）
    # 不查主程序的 RUNPATH → 落位目录形同虚设。改用旧式 DT_RPATH
    #（沿 loader 链继承，对全部传递依赖生效），使落位目录真正兜底。
    target_link_options(sigflow PRIVATE "-Wl,--disable-new-dtags")
endif()
if(UNIX)
    add_custom_command(TARGET sigflow POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:sigflow>
                -DDST=${SIGFLOW_RUNTIME_LIBS_DIR}
                -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeLibs.cmake"
        COMMENT "[debug] 运行库自洽落位 -> ${SIGFLOW_RUNTIME_LIBS_DIR}"
        VERBATIM)
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
