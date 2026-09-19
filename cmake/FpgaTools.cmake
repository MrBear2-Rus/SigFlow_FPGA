# =============================================================================
# FpgaTools.cmake — 跨平台 FPGA 工具链：yosys / nextpnr / apicula(apycula)
#
# 设计目标
#   1. Windows 与 Linux **用同一套源码**：
#      由 FetchContent 统一下载（同一个 GIT_REPOSITORY / 同一个 GIT_TAG）。
#   2. **各自本地编译**：
#      下载后不再由本工程编译，而是交给各工具自己的构建系统
#      （yosys = Makefile，nextpnr = CMake，apicula = Python venv）；
#      FetchContent 在这种场景下只承担"下载器"的角色。
#   3. 产物**落位**到主程序既有的查找布局：
#         external/fpga-tools/runtime/yosys/{bin,share}/...
#         external/fpga-tools/runtime/nextpnr/{bin,share}/...
#         external/fpga-tools/runtime/apicula/{bin,Scripts,Lib}/...
#      因此 main/MainFrame.cpp 的 FindFpgaTool() 与
#      FpgaYosysRuntime.cpp 的 ValidateYosysRuntime() **无需任何改动**。
#
# 用法
#   # 默认关闭：普通构建不受影响
#   cmake -S . -B build -DSIGFLOW_FETCH_FPGA_TOOLS=ON
#   cmake --build build --target sigflow_fpga_tools -j
#
#   # 可选：让主程序构建时自动带上（会很慢，谨慎开启）
#   cmake -S . -B build -DSIGFLOW_FETCH_FPGA_TOOLS=ON \
#         -DSIGFLOW_FETCH_FPGA_TOOLS_AS_DEPENDENCY=ON
#
#   # 国内/内网镜像（与 SIGFLOW_SLANG_DEP_URL_PREFIX 同一思路）
#   -DSIGFLOW_FPGA_TOOLS_URL_PREFIX=https://gitclone.com/github.com/
#
# 注意
#   * 下载发生在 **configure 阶段**，编译发生在 **build 阶段**，
#     所以 `cmake --build ... --target sigflow_fpga_tools` 才是真正耗时的步骤。
#   * 未开启本选项时，本文件不产生任何目标，也不访问网络。
# =============================================================================

include(FetchContent)
include(ExternalProject)

option(SIGFLOW_FETCH_FPGA_TOOLS
       "用 FetchContent 下载并本地编译 FPGA 工具链(yosys/nextpnr/apicula)" OFF)
option(SIGFLOW_FETCH_FPGA_TOOLS_AS_DEPENDENCY
       "让 SigFlow 主目标依赖 sigflow_fpga_tools（构建主程序时一并编译工具链）" OFF)
option(SIGFLOW_FETCH_NEXTPNR_DEPS
       "nextpnr 的依赖(Boost/Eigen3)也交给 FetchContent 获取并本地编译" ON)

set(SIGFLOW_FPGA_TOOLS_URL_PREFIX "" CACHE STRING
    "FPGA 工具源码镜像前缀，例如 https://gitclone.com/github.com/（空=官方 GitHub）")

# 版本固定：两端必须用**同一套源码**，所以 tag 只在这里定义一次。
set(SIGFLOW_YOSYS_REPO   "YosysHQ/yosys"     CACHE STRING "yosys 源码仓库")
set(SIGFLOW_YOSYS_SUBMODULES "abc"           CACHE STRING "yosys 需要的 git 子模块（abc 提供 yosys-abc）")
set(SIGFLOW_YOSYS_SHALLOW FALSE             CACHE BOOL
    "yosys 是否浅克隆（快，但浅克隆+子模块在个别 git 版本上不稳，默认关闭）")
set(SIGFLOW_YOSYS_TAG    "v0.47"             CACHE STRING "yosys git tag/commit")
set(SIGFLOW_NEXTPNR_REPO "YosysHQ/nextpnr"   CACHE STRING "nextpnr 源码仓库")
set(SIGFLOW_NEXTPNR_TAG  "nextpnr-0.7"       CACHE STRING "nextpnr git tag/commit")
set(SIGFLOW_APICULA_REPO "YosysHQ/apicula"   CACHE STRING "apicula 源码仓库")
set(SIGFLOW_APICULA_TAG  "0.32"              CACHE STRING "apicula git tag/commit")
# nextpnr 的依赖（由本模块 FetchContent 获取并本地编译，不依赖系统安装）
set(SIGFLOW_BOOST_VERSION "1.85.0" CACHE STRING "Boost 版本（nextpnr 需要 program_options/iostreams）")
set(SIGFLOW_BOOST_URL
    "https://github.com/boostorg/boost/releases/download/boost-${SIGFLOW_BOOST_VERSION}/boost-${SIGFLOW_BOOST_VERSION}-cmake.tar.xz"
    CACHE STRING "Boost 源码包 URL（可指向内网镜像）")
# nextpnr 的 CMakeLists 里：boost_libs = program_options iostreams，
# 并且 if(Threads_FOUND) 再追加 thread（Linux/Windows 都成立）。
set(SIGFLOW_BOOST_LIBRARIES "program_options;iostreams;thread" CACHE STRING
    "只编译 nextpnr 实际用到的 Boost 子库，避免编译整套 Boost")
set(SIGFLOW_ABC_URL
    "https://codeload.github.com/YosysHQ/abc/tar.gz/cac8f99eaa220a5e3db5caeb87cef0a975c953a2"
    CACHE STRING "yosys 的 abc 子模块（tarball，供归档源码构建时补齐）")
set(SIGFLOW_CXXOPTS_URL
    "https://codeload.github.com/jarro2783/cxxopts/tar.gz/refs/tags/v3.2.0"
    CACHE STRING "yosys 的 cxxopts 子模块（tarball，供归档源码构建时补齐）")
set(SIGFLOW_EIGEN_REPO   "https://gitlab.com/libeigen/eigen.git" CACHE STRING "Eigen3 源码仓库")
set(SIGFLOW_EIGEN_TAG    "3.4.0"             CACHE STRING "Eigen3 git tag")

set(SIGFLOW_FPGA_TOOLS_JOBS "" CACHE STRING "工具链并行编译任务数（空=自动）")
if(SIGFLOW_FPGA_TOOLS_JOBS)
    set(_sigflow_tool_jobs "${SIGFLOW_FPGA_TOOLS_JOBS}")
else()
    include(ProcessorCount)
    ProcessorCount(_sigflow_tool_jobs)
    if(_sigflow_tool_jobs EQUAL 0)
        set(_sigflow_tool_jobs 4)
    endif()
endif()

# 产物落位根目录（= FindFpgaTool 会搜索的目录）
set(SIGFLOW_FPGA_RUNTIME_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/fpga-tools/runtime"
    CACHE PATH "FPGA 工具链落位目录")

# -----------------------------------------------------------------------------
# 镜像前缀拼接
# -----------------------------------------------------------------------------
function(_sigflow_tool_url out repo)
    if(SIGFLOW_FPGA_TOOLS_URL_PREFIX)
        set(${out} "${SIGFLOW_FPGA_TOOLS_URL_PREFIX}${repo}" PARENT_SCOPE)
    else()
        set(${out} "https://github.com/${repo}" PARENT_SCOPE)
    endif()
endfunction()

if(NOT SIGFLOW_FETCH_FPGA_TOOLS)
    # 关闭时只提供状态查询目标，不做任何下载。
    add_custom_target(sigflow_fpga_tools_status
        COMMAND ${CMAKE_COMMAND} -E echo
                "SIGFLOW_FETCH_FPGA_TOOLS=OFF（未启用源码构建；使用 external/fpga-tools 预置或 PATH 中的工具）"
        VERBATIM)
    return()
endif()

message(STATUS "[FPGA tools] 启用源码构建；落位目录: ${SIGFLOW_FPGA_RUNTIME_DIR}")
message(STATUS "[FPGA tools] 并行任务数: ${_sigflow_tool_jobs}")
if(SIGFLOW_FPGA_TOOLS_URL_PREFIX)
    message(STATUS "[FPGA tools] 源码镜像前缀: ${SIGFLOW_FPGA_TOOLS_URL_PREFIX}")
endif()

# -----------------------------------------------------------------------------
# 构建工具探测（yosys 用 Makefile；nextpnr 用 CMake）
# -----------------------------------------------------------------------------
if(WIN32)
    # yosys 官方支持 GCC/Clang，不支持 MSVC。
    if(MSVC AND NOT MINGW)
        message(FATAL_ERROR
            "[FPGA tools] yosys 不能用 MSVC 构建。请改用 MinGW/MSYS2 工具链"
            "（cmake/toolchains/mingw-gcc.cmake），或在 MSYS2 shell 中运行 make。")
    endif()
    find_program(SIGFLOW_MAKE NAMES mingw32-make make gmake)
else()
    find_program(SIGFLOW_MAKE NAMES make gmake)
endif()
if(NOT SIGFLOW_MAKE)
    message(FATAL_ERROR "[FPGA tools] 找不到 make，无法构建 yosys。请先安装 make/mingw32-make。")
endif()

# =============================================================================
# 1) FetchContent：只下载，不参与 add_subdirectory
#
#    CMake 4 已移除单参 FetchContent_Populate()，因此这里用
#    SOURCE_SUBDIR 指向一个**不存在**的子目录：FetchContent_MakeAvailable
#    找不到该目录下的 CMakeLists.txt，就只会下载并设置 <name>_SOURCE_DIR，
#    而 _SOURCE_DIR 仍指向源码根（已验证于 CMake 4.4）。
#    这样既满足"用 FetchContent 下载同一套源码"，又避免把第三方工程
#    拉进本工程的配置/编译（否则会污染选项、拖慢 configure）。
# =============================================================================
set(FETCHCONTENT_QUIET OFF)
set(_sigflow_fetch_only_subdir ".sigflow-fetch-only-no-cmakelists")

# ---- yosys：需要 abc 子模块（yosys-abc 由它编译而来）----
_sigflow_tool_url(_sigflow_yosys_url "${SIGFLOW_YOSYS_REPO}")
FetchContent_Declare(yosys
    GIT_REPOSITORY "${_sigflow_yosys_url}"
    GIT_TAG        "${SIGFLOW_YOSYS_TAG}"
    GIT_SUBMODULES ${SIGFLOW_YOSYS_SUBMODULES}
    GIT_SUBMODULES_RECURSE TRUE
    GIT_SHALLOW    ${SIGFLOW_YOSYS_SHALLOW}   # 默认关闭：浅克隆与子模块组合在个别 git 版本上不稳
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  "${_sigflow_fetch_only_subdir}")

# ---- nextpnr：himbaechel/gowin 架构 ----
_sigflow_tool_url(_sigflow_nextpnr_url "${SIGFLOW_NEXTPNR_REPO}")
FetchContent_Declare(nextpnr
    GIT_REPOSITORY "${_sigflow_nextpnr_url}"
    GIT_TAG        "${SIGFLOW_NEXTPNR_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  "${_sigflow_fetch_only_subdir}")

# ---- apicula：纯 Python，提供 gowin_pack ----
_sigflow_tool_url(_sigflow_apicula_url "${SIGFLOW_APICULA_REPO}")
FetchContent_Declare(apicula
    GIT_REPOSITORY "${_sigflow_apicula_url}"
    GIT_TAG        "${SIGFLOW_APICULA_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  "${_sigflow_fetch_only_subdir}")

FetchContent_MakeAvailable(yosys nextpnr apicula)

# =============================================================================
# 2) ExternalProject：交给各自的原生构建系统本地编译
#    DOWNLOAD_COMMAND/UPDATE_COMMAND 置空 —— 源码已由 FetchContent 下好。
# =============================================================================
set(_sigflow_stage_yosys    "${SIGFLOW_FPGA_RUNTIME_DIR}/yosys")
set(_sigflow_stage_nextpnr  "${SIGFLOW_FPGA_RUNTIME_DIR}/nextpnr")
# nextpnr 的构建目录必须**显式指定**：ExternalProject 默认会用
# <prefix>/src/<name>-build，与 FetchContent 的 <lower>_BINARY_DIR 并非同一处。
set(_sigflow_nextpnr_build_dir "${CMAKE_CURRENT_BINARY_DIR}/fpga-tools-build/nextpnr")
set(_sigflow_stage_apicula  "${SIGFLOW_FPGA_RUNTIME_DIR}/apicula")

# -----------------------------------------------------------------------------
# 2.1 yosys（Makefile 工程，BUILD_IN_SOURCE）
#     make config-gcc  -> 生成 Makefile.conf
#     make -j N
#     make install PREFIX=<stage>  -> 得到 <stage>/bin/yosys 与 <stage>/share/*
#     （<stage>/share 正是 ValidateYosysRuntime 通过 <yosysDir>/../share 查找的位置）
# -----------------------------------------------------------------------------
ExternalProject_Add(sigflow_yosys_build
    SOURCE_DIR        "${yosys_SOURCE_DIR}"
    DOWNLOAD_COMMAND  ""
    UPDATE_COMMAND    ""
    PATCH_COMMAND     ""
    CONFIGURE_COMMAND "${SIGFLOW_MAKE}" config-gcc
    BUILD_COMMAND     "${SIGFLOW_MAKE}" -j${_sigflow_tool_jobs}
    INSTALL_COMMAND   "${SIGFLOW_MAKE}" install PREFIX=${_sigflow_stage_yosys}
    BUILD_IN_SOURCE   TRUE
    LOG_DOWNLOAD      TRUE
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
    USES_TERMINAL_BUILD TRUE)

# BUILD_BYPRODUCTS 需要确定的文件名，按平台给出（Windows 带 .exe）
if(WIN32)
    set(_sigflow_yosys_bin "${_sigflow_stage_yosys}/bin/yosys.exe")
    set(_sigflow_yosys_abc "${_sigflow_stage_yosys}/bin/yosys-abc.exe")
else()
    set(_sigflow_yosys_bin "${_sigflow_stage_yosys}/bin/yosys")
    set(_sigflow_yosys_abc "${_sigflow_stage_yosys}/bin/yosys-abc")
endif()
set_property(TARGET sigflow_yosys_build PROPERTY BUILD_BYPRODUCTS
             "${_sigflow_yosys_bin}" "${_sigflow_yosys_abc}")

# -----------------------------------------------------------------------------
# 2.0 nextpnr 的两个硬依赖：Boost(program_options, iostreams) + Eigen3
#
# nextpnr 0.11 的 CMakeLists 是**硬要求**，且没有内置回退：
#     set(boost_libs program_options iostreams)
#     find_package(Boost REQUIRED COMPONENTS ${boost_libs})   # 编译型库
#     find_package(Eigen3 REQUIRED NO_MODULE)                 # 需要 Eigen3Config.cmake
# 本工程不依赖系统安装，因此两者都由 FetchContent 获取、本地编译并 install 到
# external/fpga-tools/runtime/deps/ 下，再用 BOOST_ROOT / Boost_DIR / Eigen3_DIR
# 把位置告知 nextpnr。Windows 与 Linux 走完全相同的流程。
# -----------------------------------------------------------------------------
set(_sigflow_deps_stage "${SIGFLOW_FPGA_RUNTIME_DIR}/deps")

if(SIGFLOW_FETCH_NEXTPNR_DEPS)
    FetchContent_Declare(sigflow_boost
        URL           "${SIGFLOW_BOOST_URL}"
        SOURCE_SUBDIR "${_sigflow_fetch_only_subdir}")

    FetchContent_Declare(sigflow_eigen
        GIT_REPOSITORY "${SIGFLOW_EIGEN_REPO}"
        GIT_TAG        "${SIGFLOW_EIGEN_TAG}"
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SOURCE_SUBDIR  "${_sigflow_fetch_only_subdir}")

    FetchContent_MakeAvailable(sigflow_boost sigflow_eigen)

    # Boost：只编译 nextpnr 用到的子库（BOOST_INCLUDE_LIBRARIES），产物为静态库
    #（nextpnr 内部设了 Boost_USE_STATIC_LIBS ON）。
    ExternalProject_Add(sigflow_boost_build
        SOURCE_DIR        "${sigflow_boost_SOURCE_DIR}"
        DOWNLOAD_COMMAND  ""
        UPDATE_COMMAND    ""
        PATCH_COMMAND     ""
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${_sigflow_deps_stage}/boost
            -DBOOST_ENABLE_CMAKE=ON
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_TESTING=OFF
        # ★ BOOST_INCLUDE_LIBRARIES 是**分号分隔的列表**，且 Boost 把它定义为
        #   CACHE 变量（tools/cmake/include/BoostRoot.cmake）。
        #   用 CMAKE_ARGS 传会被 ExternalProject 写进 cfgcmd.txt 后按分号重新拆开，
        #   iostreams 会掉成独立参数（症状：configure 只报
        #   "Boost: libraries included: program_options"，之后 nextpnr 报
        #   找不到 boost_iostreams）。CMAKE_CACHE_ARGS 是写进 -C 初始化文件，
        #   不经过命令行，分号能原样保留 —— 这是 CMake 官方为此给出的方式。
        CMAKE_CACHE_ARGS
            "-DBOOST_INCLUDE_LIBRARIES:STRING=${SIGFLOW_BOOST_LIBRARIES}"
        BUILD_COMMAND     ${CMAKE_COMMAND} --build . -j${_sigflow_tool_jobs}
        INSTALL_COMMAND   ${CMAKE_COMMAND} --install .
        BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/fpga-tools-build/boost"
        BUILD_IN_SOURCE   FALSE
        LOG_CONFIGURE     TRUE
        LOG_BUILD         TRUE
        LOG_INSTALL       TRUE
        USES_TERMINAL_BUILD TRUE)

    # Eigen：纯头文件库，但 nextpnr 用 NO_MODULE 查找 Eigen3Config.cmake，
    # 所以必须 install 一次，不能只丢头文件。
    ExternalProject_Add(sigflow_eigen_build
        SOURCE_DIR        "${sigflow_eigen_SOURCE_DIR}"
        DOWNLOAD_COMMAND  ""
        UPDATE_COMMAND    ""
        PATCH_COMMAND     ""
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${_sigflow_deps_stage}/eigen
            -DBUILD_TESTING=OFF
            -DEIGEN_BUILD_DOC=OFF
            -DEIGEN_BUILD_PKGCONFIG=OFF
        BUILD_COMMAND     ${CMAKE_COMMAND} --build . -j${_sigflow_tool_jobs}
        INSTALL_COMMAND   ${CMAKE_COMMAND} --install .
        BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/fpga-tools-build/eigen"
        BUILD_IN_SOURCE   FALSE
        LOG_CONFIGURE     TRUE
        LOG_BUILD         TRUE
        LOG_INSTALL       TRUE
        USES_TERMINAL_BUILD TRUE)
else()
    message(STATUS "[FPGA tools] SIGFLOW_FETCH_NEXTPNR_DEPS=OFF："
                   "nextpnr 将使用系统/预设的 Boost 与 Eigen3")
endif()

# -----------------------------------------------------------------------------
# 2.2 nextpnr-himbaechel（CMake 工程）
#     依赖：boost(program_options/filesystem/thread/system) + eigen3
#     Windows 下默认用 FetchContent 取依赖，避免要求用户手工装 boost/eigen。
#     chipdb 由 gowin uarch 在编译期用 apycula 生成，因此 PYTHONPATH 指向 apicula 源码。
# -----------------------------------------------------------------------------
# nextpnr 的前置依赖：gowin 的 chipdb 由 nextpnr 调
# himbaechel/uarch/gowin/gowin_arch_gen.py 生成，而该脚本要 import apycula，
# 所以 apicula 必须先建好。但 apicula 的 ExternalProject 定义在本文件后面，
# 不能写进 DEPENDS（那时目标还不存在，CMake 会报 get_property 错误），
# 因此在 apicula 定义之后用 add_dependencies 关联。
set(_sigflow_nextpnr_dep_targets "")
if(SIGFLOW_FETCH_NEXTPNR_DEPS)
    set(_sigflow_nextpnr_dep_targets sigflow_boost_build sigflow_eigen_build)
endif()

set(_sigflow_nextpnr_cmake_args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${_sigflow_stage_nextpnr}
    -DARCH=himbaechel
    -DHIMBAECHEL_UARCH=gowin
    -DBUILD_GUI=OFF          # 不引入 Qt
    -DBUILD_PYTHON=OFF
    -DUSE_SYSTEM_LIBPNG=OFF
    # gowin chipdb 由 gowin_arch_gen.py 生成，脚本里 `from apycula import chipdb`。
    # nextpnr 自带的 cmake/FindApycula.cmake 支持用 APYCULA_INSTALL_PREFIX
    # 指向一个虚拟环境目录（会用它下面的 bin/python），否则会退回系统 python
    # 而系统里没有 apycula —— 症状就是 build 阶段满屏
    # "ModuleNotFoundError: No module named 'apycula'"。
    -DAPYCULA_INSTALL_PREFIX=${_sigflow_stage_apicula})

if(SIGFLOW_FETCH_NEXTPNR_DEPS)
    # 指向本模块自己编译出来的 Boost / Eigen3（不使用系统安装）
    list(APPEND _sigflow_nextpnr_cmake_args
        -DBOOST_ROOT=${_sigflow_deps_stage}/boost
        -DBoost_DIR=${_sigflow_deps_stage}/boost/lib/cmake/Boost-${SIGFLOW_BOOST_VERSION}
        -DBoost_NO_SYSTEM_PATHS=ON
        -DEigen3_DIR=${_sigflow_deps_stage}/eigen/share/eigen3/cmake)
endif()

ExternalProject_Add(sigflow_nextpnr_build
    SOURCE_DIR        "${nextpnr_SOURCE_DIR}"
    DOWNLOAD_COMMAND  ""
    UPDATE_COMMAND    ""
    PATCH_COMMAND     ""
    CMAKE_ARGS        ${_sigflow_nextpnr_cmake_args}
    BUILD_COMMAND     ${CMAKE_COMMAND} --build . -j${_sigflow_tool_jobs}
    INSTALL_COMMAND   ${CMAKE_COMMAND} --install .
    BINARY_DIR        "${_sigflow_nextpnr_build_dir}"
    BUILD_IN_SOURCE   FALSE
    DEPENDS           ${_sigflow_nextpnr_dep_targets}
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
    USES_TERMINAL_BUILD TRUE)

# nextpnr 的 chipdb 安装位置各版本不一；统一搬成主程序期望的布局。
if(WIN32)
    set(_sigflow_nextpnr_bin "${_sigflow_stage_nextpnr}/bin/nextpnr-himbaechel.exe")
else()
    set(_sigflow_nextpnr_bin "${_sigflow_stage_nextpnr}/bin/nextpnr-himbaechel")
endif()
add_custom_command(TARGET sigflow_nextpnr_build POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_sigflow_stage_nextpnr}/share/himbaechel/gowin"
    # 如果构建目录里生成了 chipdb，就补齐到主程序查找的位置（已存在则跳过）
    COMMAND ${CMAKE_COMMAND}
            -Dsrc1=${_sigflow_nextpnr_build_dir}
            -Dsrc2=${_sigflow_stage_nextpnr}
            -Ddst=${_sigflow_stage_nextpnr}/share/himbaechel/gowin
            -P "${CMAKE_CURRENT_LIST_DIR}/FpgaToolsStageChipdb.cmake"
    VERBATIM)

# -----------------------------------------------------------------------------
# 2.3 apicula / gowin_pack（Python）
#     用独立 venv 安装，避免污染系统 Python。
#     注意 venv 的脚本目录平台不同：Windows=Scripts/，Linux=bin/。
#     主程序已同时支持这两处（见 FindFpgaTool 的 apicula 候选）。
# -----------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "[FPGA tools] 找不到 python3，无法构建 apicula/gowin_pack。")
endif()

if(WIN32)
    set(_sigflow_venv_python "${_sigflow_stage_apicula}/Scripts/python.exe")
else()
    set(_sigflow_venv_python "${_sigflow_stage_apicula}/bin/python")
endif()

ExternalProject_Add(sigflow_apicula_build
    SOURCE_DIR        "${apicula_SOURCE_DIR}"
    DOWNLOAD_COMMAND  ""
    UPDATE_COMMAND    ""
    PATCH_COMMAND     ""
    CONFIGURE_COMMAND ${Python3_EXECUTABLE} -m venv "${_sigflow_stage_apicula}"
    # apicula 的 setup.py 用 use_scm_version=True 从 **git 元数据**取版本号；
    # 用归档源码（tarball，没有 .git）时 setuptools_scm 会直接崩在
    # "_version_missing() got an unexpected keyword argument 'tool'"。
    # 显式给出版本即可绕过。
    BUILD_COMMAND     ${CMAKE_COMMAND} -E env
                          "SETUPTOOLS_SCM_PRETEND_VERSION=${SIGFLOW_APICULA_TAG}"
                          "${_sigflow_venv_python}" -m pip install --upgrade pip
    INSTALL_COMMAND   ${CMAKE_COMMAND} -E env
                          "SETUPTOOLS_SCM_PRETEND_VERSION=${SIGFLOW_APICULA_TAG}"
                          "${_sigflow_venv_python}" -m pip install .
    BUILD_IN_SOURCE   TRUE
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
    USES_TERMINAL_BUILD TRUE)

# -----------------------------------------------------------------------------
# 2.4 yosys 的两个 git 子模块：abc 与 cxxopts
#
# yosys 用 git 子模块携带它们（.gitmodules 里有两条），而 **归档 tarball 不含子模块**，
# 从 tarball 构建时会缺：
#     abc/                                 -> 提供 yosys-abc
#     libs/cxxopts/include/cxxopts.hpp     -> kernel/driver.cc 直接 include
# 这里在**确实缺失时**才补，并且用 URL/tarball 方式（走 codeload），
# 完全不依赖 git 子模块机制 —— 之前的失败就出在 abc 子模块克隆上。
# 用 git 克隆方式获取 yosys 时两者本就存在，这段不会触发。
# -----------------------------------------------------------------------------
set(_sigflow_yosys_submodule_targets "")
set(_sigflow_need_abc FALSE)
set(_sigflow_need_cxxopts FALSE)

if(NOT EXISTS "${yosys_SOURCE_DIR}/abc/Makefile")
    set(_sigflow_need_abc TRUE)
    message(STATUS "[FPGA tools] yosys 缺 abc 子模块，用 tarball 补齐")
    FetchContent_Declare(sigflow_abc
        URL           "${SIGFLOW_ABC_URL}"
        SOURCE_SUBDIR "${_sigflow_fetch_only_subdir}")
    list(APPEND _sigflow_yosys_submodule_targets sigflow_abc)
endif()

if(NOT EXISTS "${yosys_SOURCE_DIR}/libs/cxxopts/include/cxxopts.hpp")
    set(_sigflow_need_cxxopts TRUE)
    message(STATUS "[FPGA tools] yosys 缺 cxxopts 子模块，用 tarball 补齐")
    FetchContent_Declare(sigflow_cxxopts
        URL           "${SIGFLOW_CXXOPTS_URL}"
        SOURCE_SUBDIR "${_sigflow_fetch_only_subdir}")
    list(APPEND _sigflow_yosys_submodule_targets sigflow_cxxopts)
endif()

if(_sigflow_yosys_submodule_targets)
    FetchContent_MakeAvailable(${_sigflow_yosys_submodule_targets})
    if(_sigflow_need_abc)
        file(COPY "${sigflow_abc_SOURCE_DIR}/" DESTINATION "${yosys_SOURCE_DIR}/abc")
    endif()
    if(_sigflow_need_cxxopts)
        file(COPY "${sigflow_cxxopts_SOURCE_DIR}/"
             DESTINATION "${yosys_SOURCE_DIR}/libs/cxxopts")
    endif()
    message(STATUS "[FPGA tools] yosys 子模块已补齐")
endif()

# apicula 必须先于 nextpnr 完成（chipdb 生成依赖 apycula）
add_dependencies(sigflow_nextpnr_build sigflow_apicula_build)

# -----------------------------------------------------------------------------
# 2.5 openFPGALoader —— "下载/烧录到板"
#
# 依赖链：openFPGALoader → libftdi1 → libusb-1.0（系统已有）
#
# 为什么必须自建 libftdi1：
#   Tang Nano 9K 在 openFPGALoader 的 src/board.hpp 里定义为 cable "ft2232"：
#       JTAG_BOARD("tangnano9k", "", "ft2232", SPI_FLASH, 0, 0, CABLE_DEFAULT)
#   而 CMakeLists.txt 只要下列任一为 ON 就**强制** USE_LIBFTDI=ON
#       if (ENABLE_FTDI_BASED_CABLE OR ENABLE_USB_BLASTERI OR ENABLE_XILINX_VIRTUAL_CABLE_SERVER)
#           set(USE_LIBFTDI ON)     # 普通变量，-DUSE_LIBFTDI=OFF 覆盖不掉
#   FTDI 类线缆没有 libusb 回退实现（src/ftdiJtagMPSSE.cpp 等直接依赖 libftdi1）。
#   所以不装 libftdi 就只能砍掉 ft2232，等于不支持 Tang Nano 全系。
#
# 注入方式：openFPGALoader 先 find_package(LibFTDI1 QUIET)，失败才回退
# pkg_check_modules(libftdi1)。libftdi 会同时安装 LibFTDI1Config.cmake 与
# libftdi1.pc，因此把它的安装前缀交给 CMAKE_PREFIX_PATH / PKG_CONFIG_PATH 即可。
# 运行期还需要能找到 libftdi1.so，故给 openFPGALoader 写入 INSTALL_RPATH。
# -----------------------------------------------------------------------------
option(SIGFLOW_FETCH_OPENFPGALOADER "下载并本地编译 openFPGALoader（含 libftdi1）" ON)

set(SIGFLOW_LIBFTDI_URL
    "https://www.intra2net.com/en/developer/libftdi/download/libftdi1-1.5.tar.bz2"
    CACHE STRING "libftdi 源码包 URL（openFPGALoader 的 FTDI 线缆依赖）")
set(SIGFLOW_OPENFPGALOADER_URL
    "https://codeload.github.com/trabucayre/openFPGALoader/tar.gz/refs/tags/v1.1.1"
    CACHE STRING "openFPGALoader 源码包 URL")

set(_sigflow_stage_libftdi          "${SIGFLOW_FPGA_RUNTIME_DIR}/deps/libftdi")
set(_sigflow_stage_openfpgaloader   "${SIGFLOW_FPGA_RUNTIME_DIR}/openfpgaloader")
set(_sigflow_openfpgaloader_build_dir "${CMAKE_CURRENT_BINARY_DIR}/fpga-tools-build/openfpgaloader")
set(_sigflow_openfpgaloader_dep_targets "")

if(SIGFLOW_FETCH_OPENFPGALOADER)
    FetchContent_Declare(sigflow_libftdi
        URL           "${SIGFLOW_LIBFTDI_URL}"
        SOURCE_SUBDIR "${_sigflow_fetch_only_subdir}")
    FetchContent_Declare(sigflow_openfpgaloader
        URL           "${SIGFLOW_OPENFPGALOADER_URL}"
        SOURCE_SUBDIR "${_sigflow_fetch_only_subdir}")
    FetchContent_MakeAvailable(sigflow_libftdi sigflow_openfpgaloader)

    # libftdi1：只用核心库。FTDIPP/PYTHON_BINDINGS/FTDI_EEPROM/EXAMPLES/BUILD_TESTS
    # 全部关掉 —— 其中 FTDI_EEPROM 需要 libconfuse、FTDIPP/BUILD_TESTS 需要 Boost，
    # 我们都不需要（这些开关定义在 libftdi 的 CMakeOptions.txt 里）。
    ExternalProject_Add(sigflow_libftdi_build
        SOURCE_DIR        "${sigflow_libftdi_SOURCE_DIR}"
        DOWNLOAD_COMMAND  ""
        UPDATE_COMMAND    ""
        PATCH_COMMAND     ""
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${_sigflow_stage_libftdi}
            # libftdi 1.5 仍写着 cmake_minimum_required(VERSION 2.6)，而 CMake 4
            # 已移除对 <3.5 的兼容，直接 configure 会报
            #   "Compatibility with CMake < 3.5 has been removed from CMake."
            # 官方给出的逃生口就是这个变量。
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DFTDIPP=OFF
            -DPYTHON_BINDINGS=OFF
            -DFTDI_EEPROM=OFF
            -DEXAMPLES=OFF
            -DBUILD_TESTS=OFF
            -DDOC=OFF
        BUILD_COMMAND     ${CMAKE_COMMAND} --build . -j${_sigflow_tool_jobs}
        INSTALL_COMMAND   ${CMAKE_COMMAND} --install .
        BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/fpga-tools-build/libftdi"
        BUILD_IN_SOURCE   FALSE
        LOG_CONFIGURE     TRUE
        LOG_BUILD         TRUE
        LOG_INSTALL       TRUE
        USES_TERMINAL_BUILD TRUE)

    # openFPGALoader：整条 configure 用 `cmake -E env` 包起来，把 PKG_CONFIG_PATH
    # 指向自建的 libftdi（pkg-config 会在此基础上继续搜索系统默认路径，所以
    # libusb-1.0 / zlib / libudev 仍能找到）。
    ExternalProject_Add(sigflow_openfpgaloader_build
        SOURCE_DIR        "${sigflow_openfpgaloader_SOURCE_DIR}"
        DOWNLOAD_COMMAND  ""
        UPDATE_COMMAND    ""
        PATCH_COMMAND     ""
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
            "PKG_CONFIG_PATH=${_sigflow_stage_libftdi}/lib/pkgconfig"
            ${CMAKE_COMMAND}
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_INSTALL_PREFIX=${_sigflow_stage_openfpgaloader}
                -DCMAKE_PREFIX_PATH=${_sigflow_stage_libftdi}
                -DCMAKE_INSTALL_RPATH=${_sigflow_stage_libftdi}/lib
                -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
                -DENABLE_OPTIM=ON
                -DENABLE_FTDI_BASED_CABLE=ON
                -DENABLE_CMSISDAP_V1=OFF
                -DENABLE_LIBGPIOD=OFF
                -S "${sigflow_openfpgaloader_SOURCE_DIR}"
                -B "${_sigflow_openfpgaloader_build_dir}"
        BUILD_COMMAND     ${CMAKE_COMMAND} --build "${_sigflow_openfpgaloader_build_dir}"
                              -j${_sigflow_tool_jobs}
        INSTALL_COMMAND   ${CMAKE_COMMAND} --install "${_sigflow_openfpgaloader_build_dir}"
        BINARY_DIR        "${_sigflow_openfpgaloader_build_dir}"
        BUILD_IN_SOURCE   FALSE
        DEPENDS           sigflow_libftdi_build
        LOG_CONFIGURE     TRUE
        LOG_BUILD         TRUE
        LOG_INSTALL       TRUE
        USES_TERMINAL_BUILD TRUE)

    set(_sigflow_openfpgaloader_dep_targets sigflow_libftdi_build sigflow_openfpgaloader_build)
    message(STATUS "[FPGA tools] openFPGALoader: 启用（含 libftdi1，Tang Nano 9K 的 ft2232 线缆需要）")
else()
    message(STATUS "[FPGA tools] SIGFLOW_FETCH_OPENFPGALOADER=OFF：烧录功能将使用预置/系统 openFPGALoader")
endif()

# =============================================================================
# 3) 汇总目标
# =============================================================================
add_custom_target(sigflow_fpga_tools
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "== FPGA 工具链就绪 =="
    COMMAND ${CMAKE_COMMAND} -E echo "  yosys   : ${_sigflow_yosys_bin}"
    COMMAND ${CMAKE_COMMAND} -E echo "  nextpnr : ${_sigflow_nextpnr_bin}"
    COMMAND ${CMAKE_COMMAND} -E echo "  apicula : ${_sigflow_stage_apicula}"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    VERBATIM)
add_dependencies(sigflow_fpga_tools
    sigflow_yosys_build sigflow_nextpnr_build sigflow_apicula_build
    ${_sigflow_nextpnr_dep_targets}
    ${_sigflow_openfpgaloader_dep_targets})

if(SIGFLOW_FETCH_FPGA_TOOLS_AS_DEPENDENCY)
    add_dependencies(sigflow sigflow_fpga_tools)
    message(STATUS "[FPGA tools] 已挂到 sigflow 目标上（每次构建都会检查工具链）")
else()
    message(STATUS "[FPGA tools] 请用 `cmake --build <build> --target sigflow_fpga_tools` 单独构建工具链")
endif()
