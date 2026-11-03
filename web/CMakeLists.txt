# =============================================================================
# SigFlow Web 平台 —— 唯一构建文件
#
# 一个项目只有一个 CMakeLists.txt：本文件声明全部目标。
# 执行方式永远是：
#     cmake -S web -B build/web
#     cmake --build build/web -j
#
# 分三段：
#   A. 工程、路径、选项、编译规则
#   B. C++ 目标（third_party → api → platform/core → plugins → analysis/debug/trace → cli）
#   C. JS 便捷目标、测试与安装
#
# 约定：
#   * 只面向 Linux；不联网、不用 FetchContent、不编译 yosys/nextpnr 等外部工具；
#   * 不出现全局 include_directories() / link_libraries()，依赖一律写在 target 上；
#   * 每个模块用 sigflow_add_module() 一行声明，源文件按目录 GLOB（迁移期目录为空
#     时自动退化为 INTERFACE 目标，任何阶段都能 configure + build）。
# =============================================================================
cmake_minimum_required(VERSION 3.21)

project(SigFlowWeb LANGUAGES C CXX VERSION 0.1.0)

# -----------------------------------------------------------------------------
# A-1. 路径
# -----------------------------------------------------------------------------
set(SIGFLOW_WEB_DIR       "${CMAKE_CURRENT_SOURCE_DIR}")
set(SIGFLOW_CPP_DIR       "${SIGFLOW_WEB_DIR}/backend/cpp")
set(SIGFLOW_JS_DIR        "${SIGFLOW_WEB_DIR}/backend/nodejs")
set(SIGFLOW_FRONTEND_DIR  "${SIGFLOW_WEB_DIR}/frontend")
set(SIGFLOW_STATIC_DIR    "${SIGFLOW_WEB_DIR}/backend/static")

# -----------------------------------------------------------------------------
# A-2. 选项
# -----------------------------------------------------------------------------
set(SIGFLOW_DATA_DIR "${SIGFLOW_STATIC_DIR}"
    CACHE PATH "运行时静态资源（target-profiles / rtl），C++ 侧通过 --data-dir 取得")

set(SIGFLOW_TOOL_RUNTIME_DIR "" CACHE PATH
    "预置 FPGA 工具链 runtime 根目录（yosys/nextpnr/apicula/verilator/openFPGALoader）")

option(SIGFLOW_BUILD_TESTS     "构建 C++ 冒烟测试"                    ON)
option(SIGFLOW_ENABLE_FTDI     "启用 libftdi 直连传输（需要 libftdi1）" ON)
option(SIGFLOW_STRICT_WARNINGS "开启 -Wall -Wextra"                    ON)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "本工程只面向 Linux 部署；Windows/macOS 分支已按设计移除。")
endif()

# -----------------------------------------------------------------------------
# A-3. 语言标准与编译选项
# -----------------------------------------------------------------------------
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

if(SIGFLOW_STRICT_WARNINGS)
    add_compile_options(
        -Wall
        -Wextra
        -Wno-unused-parameter     # 冻结接口里有占位参数
        -Werror=return-type
    )
endif()

# -----------------------------------------------------------------------------
# A-4. 模块辅助函数
#
#   sigflow_add_module(<name> DIR <相对目录> [DEPS ...] [INCLUDE_DIRS ...] [DEFINES ...])
#
# 头文件搜索根（与原工程 include 约定一致，保证源码可原样复制）：
#   <模块目录>              "#include "Xxx.h""
#   <模块目录>/include      插件的私有头（原 InnerPlugin/<name>/include）
#   backend/cpp/include     冻结契约 <eda/api/...>
#   backend/cpp/src         跨模块 "eda-core/JobService.h"
# -----------------------------------------------------------------------------
function(sigflow_module_include_dirs TARGET SCOPE DIR)
    target_include_directories(${TARGET} ${SCOPE}
        "${DIR}"
        "${DIR}/include"
        "${SIGFLOW_CPP_DIR}/include"
        "${SIGFLOW_CPP_DIR}/src"
        ${ARGN}
    )
endfunction()

function(sigflow_add_module NAME)
    cmake_parse_arguments(ARG "" "DIR" "DEPS;INCLUDE_DIRS;DEFINES" ${ARGN})
    if(NOT ARG_DIR)
        message(FATAL_ERROR "sigflow_add_module(${NAME}): DIR 必填")
    endif()

    set(_dir "${SIGFLOW_CPP_DIR}/${ARG_DIR}")
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${_dir}/*.c" "${_dir}/*.cpp")

    if(_sources)
        add_library(${NAME} STATIC ${_sources})
        sigflow_module_include_dirs(${NAME} PUBLIC "${_dir}" ${ARG_INCLUDE_DIRS})
        target_compile_features(${NAME} PUBLIC cxx_std_20)
        if(ARG_DEPS)
            target_link_libraries(${NAME} PUBLIC ${ARG_DEPS})
        endif()
        if(ARG_DEFINES)
            target_compile_definitions(${NAME} PUBLIC ${ARG_DEFINES})
        endif()
        list(LENGTH _sources _source_count)
        message(STATUS "[sigflow] ${NAME}: ${_source_count} 个源文件 (${ARG_DIR})")
    else()
        add_library(${NAME} INTERFACE)
        sigflow_module_include_dirs(${NAME} INTERFACE "${_dir}" ${ARG_INCLUDE_DIRS})
        target_compile_features(${NAME} INTERFACE cxx_std_20)
        if(ARG_DEPS)
            target_link_libraries(${NAME} INTERFACE ${ARG_DEPS})
        endif()
        if(ARG_DEFINES)
            target_compile_definitions(${NAME} INTERFACE ${ARG_DEFINES})
        endif()
        message(STATUS "[sigflow] ${NAME}: ${ARG_DIR} 暂无源文件，暂为 INTERFACE（等待迁移）")
    endif()
endfunction()

# =============================================================================
# B. C++ 目标
# =============================================================================

# ---- B-1. 第三方（源码直编，随 web/ 分发，不联网）--------------------------
# 布局保证原代码的 #include 不用改：
#   third_party/json/json.h        -> <json/json.h>
#   third_party/vcd/vcd.h          -> <vcd/vcd.h>
#   third_party/nlohmann/...       -> <nlohmann/json.hpp>
#   third_party/tree-sitter/lib/include/tree_sitter/api.h
#   third_party/tree-sitter/tree-sitter-verilog/src/parser.c
set(SIGFLOW_TP_DIR "${SIGFLOW_CPP_DIR}/third_party")
set(SIGFLOW_TS_LIB "${SIGFLOW_TP_DIR}/tree-sitter/lib")
set(SIGFLOW_TS_VERILOG "${SIGFLOW_TP_DIR}/tree-sitter/tree-sitter-verilog")

file(GLOB _json_sources CONFIGURE_DEPENDS "${SIGFLOW_TP_DIR}/json/*.cpp")
if(_json_sources)
    add_library(sigflow_jsoncpp STATIC ${_json_sources})
    target_include_directories(sigflow_jsoncpp PUBLIC "${SIGFLOW_TP_DIR}")
else()
    add_library(sigflow_jsoncpp INTERFACE)
    target_include_directories(sigflow_jsoncpp INTERFACE "${SIGFLOW_TP_DIR}")
    message(STATUS "[sigflow] sigflow_jsoncpp: 尚未迁入 third_party/json/")
endif()

add_library(sigflow_nlohmann INTERFACE)
target_include_directories(sigflow_nlohmann INTERFACE "${SIGFLOW_TP_DIR}")
target_compile_features(sigflow_nlohmann INTERFACE cxx_std_20)

file(GLOB _vcd_sources CONFIGURE_DEPENDS "${SIGFLOW_TP_DIR}/vcd/*.cpp")
if(_vcd_sources)
    add_library(sigflow_vcd STATIC ${_vcd_sources})
    target_include_directories(sigflow_vcd PUBLIC "${SIGFLOW_TP_DIR}")
else()
    add_library(sigflow_vcd INTERFACE)
    target_include_directories(sigflow_vcd INTERFACE "${SIGFLOW_TP_DIR}")
    message(STATUS "[sigflow] sigflow_vcd: 尚未迁入 third_party/vcd/")
endif()

file(GLOB _ts_sources CONFIGURE_DEPENDS "${SIGFLOW_TS_LIB}/src/lib.c")
if(_ts_sources)
    add_library(sigflow_tree_sitter STATIC ${_ts_sources})
    target_include_directories(sigflow_tree_sitter PUBLIC
        "${SIGFLOW_TS_LIB}/include"
        "${SIGFLOW_TS_LIB}/src"
    )
    target_compile_definitions(sigflow_tree_sitter PRIVATE _GNU_SOURCE)
else()
    add_library(sigflow_tree_sitter INTERFACE)
    message(STATUS "[sigflow] sigflow_tree_sitter: 尚未迁入 third_party/tree-sitter/lib/")
endif()

file(GLOB _tsv_sources CONFIGURE_DEPENDS "${SIGFLOW_TS_VERILOG}/src/parser.c")
if(_tsv_sources)
    add_library(sigflow_tree_sitter_verilog STATIC ${_tsv_sources})
    target_include_directories(sigflow_tree_sitter_verilog PUBLIC "${SIGFLOW_TS_VERILOG}/src")
    # parser.c 是 47MB 生成代码，-O1 足够，-O2 会把编译时间拉长数倍。
    target_compile_options(sigflow_tree_sitter_verilog PRIVATE -O1)
    target_link_libraries(sigflow_tree_sitter_verilog PUBLIC sigflow_tree_sitter)
else()
    add_library(sigflow_tree_sitter_verilog INTERFACE)
    message(STATUS "[sigflow] sigflow_tree_sitter_verilog: 尚未迁入 Verilog 语法")
endif()

# ---- B-2. 冻结契约（backend/cpp/include/eda/api）---------------------------
add_library(sigflow_api INTERFACE)
target_include_directories(sigflow_api INTERFACE "${SIGFLOW_CPP_DIR}/include")
target_compile_features(sigflow_api INTERFACE cxx_std_20)
target_link_libraries(sigflow_api INTERFACE sigflow_nlohmann)

# ---- B-3. 平台层与内核 ------------------------------------------------------
find_package(Threads REQUIRED)

# eda-platform: 进程托管(POSIX)/动态库/sha256/路径
sigflow_add_module(sigflow_platform
    DIR src/eda-platform
    DEPS sigflow_api Threads::Threads ${CMAKE_DL_LIBS}
)

# eda-core: 日志/工具发现/单任务执行器/报告落盘
#   不迁 PluginHost、EventBus、CommandBus、SchemaRegistry、Project（详见 README 3.2）
sigflow_add_module(sigflow_core
    DIR src/eda-core
    DEPS sigflow_platform sigflow_jsoncpp
)

# ---- B-4. 八个工具模块 ------------------------------------------------------
sigflow_add_module(sigflow_plugin_synth
    DIR src/plugins/synth-yosys
    DEPS sigflow_core
)
sigflow_add_module(sigflow_plugin_pnr
    DIR src/plugins/pnr-nextpnr
    DEPS sigflow_core
)
sigflow_add_module(sigflow_plugin_pack
    DIR src/plugins/pack-gowin
    DEPS sigflow_core
)
sigflow_add_module(sigflow_plugin_flash
    DIR src/plugins/program-openfpgaloader
    DEPS sigflow_core
)
sigflow_add_module(sigflow_plugin_sim
    DIR src/plugins/sim-verilator
    DEPS sigflow_core
)
sigflow_add_module(sigflow_plugin_target
    DIR src/plugins/target-tangnano9k
    DEPS sigflow_core
)
sigflow_add_module(sigflow_plugin_cst
    DIR src/plugins/cst-gowin-cst
    DEPS sigflow_core sigflow_plugin_target
)
sigflow_add_module(sigflow_plugin_wave
    DIR src/plugins/wave-vcd
    DEPS sigflow_core sigflow_vcd
)

# ---- B-5. 分析与宿主侧模块 --------------------------------------------------
# analysis: 结构树 / 语法诊断（tree-sitter），供 cli 的 structure|lint 使用
sigflow_add_module(sigflow_analysis
    DIR src/analysis
    DEPS sigflow_core sigflow_tree_sitter sigflow_tree_sitter_verilog
)

# trace: 波形查询 / sidecar 索引
sigflow_add_module(sigflow_trace
    DIR src/trace
    DEPS sigflow_core sigflow_vcd
)

# debug: TraceBridge 采集/传输/分析；libftdi 可选
set(_debug_deps sigflow_core)
set(_debug_includes "")
set(_debug_defines "")
if(SIGFLOW_ENABLE_FTDI)
    find_path(SIGFLOW_FTDI_INCLUDE_DIR NAMES libftdi1/ftdi.h)
    find_library(SIGFLOW_FTDI_LIBRARY NAMES ftdi1)
    if(SIGFLOW_FTDI_INCLUDE_DIR AND SIGFLOW_FTDI_LIBRARY)
        list(APPEND _debug_deps "${SIGFLOW_FTDI_LIBRARY}")
        list(APPEND _debug_includes "${SIGFLOW_FTDI_INCLUDE_DIR}")
        list(APPEND _debug_defines SIGFLOW_HAVE_LIBFTDI=1)
        message(STATUS "[sigflow] libftdi 直连传输: 启用 (${SIGFLOW_FTDI_LIBRARY})")
    else()
        message(STATUS "[sigflow] libftdi 直连传输: 未找到，回退到串口")
    endif()
endif()
sigflow_add_module(sigflow_debug
    DIR src/debug
    DEPS ${_debug_deps}
    INCLUDE_DIRS ${_debug_includes}
    DEFINES ${_debug_defines}
)

# ---- B-6. 可执行：sigflow-tool ---------------------------------------------
# 唯一 C++ 产物；无状态、一次性；子命令见 README 3.5。
file(GLOB_RECURSE _cli_sources CONFIGURE_DEPENDS "${SIGFLOW_CPP_DIR}/src/cli/*.cpp")
if(_cli_sources)
    add_executable(sigflow-tool ${_cli_sources})
    target_link_libraries(sigflow-tool PRIVATE
        sigflow_analysis
        sigflow_debug
        sigflow_trace
        sigflow_plugin_synth
        sigflow_plugin_pnr
        sigflow_plugin_pack
        sigflow_plugin_flash
        sigflow_plugin_sim
        sigflow_plugin_target
        sigflow_plugin_cst
        sigflow_plugin_wave
    )
    target_include_directories(sigflow-tool PRIVATE
        "${SIGFLOW_CPP_DIR}/include"
        "${SIGFLOW_CPP_DIR}/src"
    )
    target_compile_definitions(sigflow-tool PRIVATE
        SIGFLOW_TOOL_VERSION="${PROJECT_VERSION}"
        SIGFLOW_DEFAULT_DATA_DIR="${SIGFLOW_DATA_DIR}"
        SIGFLOW_TOOL_RUNTIME_DIR="${SIGFLOW_TOOL_RUNTIME_DIR}"
    )
    set_target_properties(sigflow-tool PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    message(STATUS "[sigflow] sigflow-tool: 可执行目标已生成")
else()
    message(STATUS "[sigflow] sigflow-tool: src/cli 暂无源文件，跳过可执行目标")
endif()

# =============================================================================
# C. JS、测试与安装
# =============================================================================

# ---- C-1. C++ 冒烟测试 ------------------------------------------------------
if(SIGFLOW_BUILD_TESTS)
    enable_testing()
    set(SIGFLOW_TEST_TARGETS "")
    file(GLOB _test_sources CONFIGURE_DEPENDS "${SIGFLOW_CPP_DIR}/tests/test_*.cpp")
    if(_test_sources)
        foreach(_test_source IN LISTS _test_sources)
            get_filename_component(_test_name "${_test_source}" NAME_WE)
            add_executable(${_test_name} "${_test_source}")
            target_link_libraries(${_test_name} PRIVATE
                sigflow_analysis sigflow_debug sigflow_trace
                sigflow_plugin_synth sigflow_plugin_pnr sigflow_plugin_pack
                sigflow_plugin_flash sigflow_plugin_sim sigflow_plugin_target
                sigflow_plugin_cst sigflow_plugin_wave)
            if(_test_name STREQUAL "test_job_protocol")
                add_test(NAME ${_test_name} COMMAND ${_test_name}
                    "${SIGFLOW_CPP_DIR}/contracts/run-v1/fixtures"
                    "${CMAKE_BINARY_DIR}/contract-output")
            else()
                add_test(NAME ${_test_name} COMMAND ${_test_name})
            endif()
            list(APPEND SIGFLOW_TEST_TARGETS ${_test_name})
            set_tests_properties(${_test_name} PROPERTIES TIMEOUT 20)
            set_target_properties(${_test_name} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        endforeach()
    else()
        message(STATUS "[sigflow] tests: 暂无测试源文件")
    endif()
endif()

# Fixture provider is compiled only into this test executable, never production/install.
if(SIGFLOW_BUILD_TESTS)
    add_executable(sigflow-tool-fixture ${_cli_sources})
    target_link_libraries(sigflow-tool-fixture PRIVATE sigflow_core)
    target_include_directories(sigflow-tool-fixture PRIVATE "${SIGFLOW_CPP_DIR}/tests")
    target_compile_definitions(sigflow-tool-fixture PRIVATE SIGFLOW_TEST_PROVIDER=1
        SIGFLOW_DEFAULT_DATA_DIR="${SIGFLOW_DATA_DIR}" SIGFLOW_TOOL_RUNTIME_DIR="${SIGFLOW_TOOL_RUNTIME_DIR}")
    set_target_properties(sigflow-tool-fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Optional offline schema checks; never install Python packages during configure.
if(SIGFLOW_BUILD_TESTS)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_analysis_integration COMMAND "${Python3_EXECUTABLE}"
            "${SIGFLOW_CPP_DIR}/tests/analysis_integration.py"
            "$<TARGET_FILE:sigflow-tool>" "${CMAKE_BINARY_DIR}")
        set_tests_properties(test_analysis_integration PROPERTIES TIMEOUT 60)
        add_test(NAME test_board_integration COMMAND "${Python3_EXECUTABLE}"
            "${SIGFLOW_CPP_DIR}/tests/board_integration.py"
            "$<TARGET_FILE:sigflow-tool>" "${CMAKE_BINARY_DIR}" "${SIGFLOW_STATIC_DIR}")
        set_tests_properties(test_board_integration PROPERTIES TIMEOUT 60)
        add_test(NAME test_sim_integration COMMAND "${Python3_EXECUTABLE}"
            "${SIGFLOW_CPP_DIR}/tests/sim_integration.py"
            "$<TARGET_FILE:sigflow-tool>" "${CMAKE_BINARY_DIR}")
        set_tests_properties(test_sim_integration PROPERTIES TIMEOUT 60)
        add_test(NAME test_pack_integration COMMAND "${Python3_EXECUTABLE}"
            "${SIGFLOW_CPP_DIR}/tests/pack_integration.py"
            "$<TARGET_FILE:sigflow-tool>" "${CMAKE_BINARY_DIR}")
        set_tests_properties(test_pack_integration PROPERTIES TIMEOUT 60)
        add_test(NAME test_pnr_integration COMMAND "${Python3_EXECUTABLE}"
            "${SIGFLOW_CPP_DIR}/tests/pnr_integration.py"
            "$<TARGET_FILE:sigflow-tool>" "${CMAKE_BINARY_DIR}")
        set_tests_properties(test_pnr_integration PROPERTIES TIMEOUT 60)
        add_test(NAME test_synth_integration COMMAND "${Python3_EXECUTABLE}"
            "${SIGFLOW_CPP_DIR}/tests/synth_integration.py"
            "$<TARGET_FILE:sigflow-tool>" "${CMAKE_BINARY_DIR}")
        set_tests_properties(test_synth_integration PROPERTIES TIMEOUT 60)
        add_test(NAME test_job_runtime COMMAND "${Python3_EXECUTABLE}"
            "${SIGFLOW_CPP_DIR}/tests/runtime_integration.py"
            "$<TARGET_FILE:sigflow-tool-fixture>" "$<TARGET_FILE:sigflow-tool>"
            "${CMAKE_BINARY_DIR}" "${SIGFLOW_CPP_DIR}/contracts/run-v1")
        set_tests_properties(test_job_runtime PROPERTIES TIMEOUT 90)

        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import jsonschema"
            RESULT_VARIABLE _schema_available OUTPUT_QUIET ERROR_QUIET)
        if(_schema_available EQUAL 0)
            add_test(NAME test_run_schemas COMMAND "${Python3_EXECUTABLE}"
                "${SIGFLOW_CPP_DIR}/tests/validate_run_contracts.py"
                "${SIGFLOW_CPP_DIR}/contracts/run-v1" "${CMAKE_BINARY_DIR}/contract-output")
            set_tests_properties(test_run_schemas PROPERTIES DEPENDS test_job_protocol TIMEOUT 20)
        else()
            message(STATUS "[sigflow] JSON Schema checks unavailable: Python jsonschema missing")
        endif()
    endif()
endif()

# ---- C-2. 便捷命令 ----------------------------------------------------------
# 约定：全部 EXCLUDE_FROM_ALL，只有显式 `cmake --build <dir> --target <name>` 才执行；
#       不使用 npm（当前没有 JS 依赖，第三方 JS 已 vendor 在 frontend/vendor/）。

# build：构建 C++（含测试目标）。JS 侧不需要构建步骤。
set(_sigflow_build_deps "")
if(TARGET sigflow-tool)
    list(APPEND _sigflow_build_deps sigflow-tool)
endif()
if(TARGET sigflow-tool-fixture)
    list(APPEND _sigflow_build_deps sigflow-tool-fixture)
endif()
list(APPEND _sigflow_build_deps ${SIGFLOW_TEST_TARGETS})
add_custom_target(build
    DEPENDS ${_sigflow_build_deps}
    COMMENT "构建 C++（sigflow-tool 与测试）"
    EXCLUDE_FROM_ALL
)

# distclean：清理编译产物，并删除 web/ 下其它 build* 目录。
# 不删除当前构建目录本身：CMake 的 recipe 以它为工作目录，删掉会让 make 失败。
set(SIGFLOW_DISTCLEAN_SCRIPT "${CMAKE_BINARY_DIR}/sigflow-distclean.sh")
file(WRITE "${SIGFLOW_DISTCLEAN_SCRIPT}" [=[#!/bin/sh
set -eu
web="${SIGFLOW_WEB_DIR:?}"
current="${SIGFLOW_CURRENT_BUILD:?}"
cmake_bin="${SIGFLOW_CMAKE:?}"
current_real=$(cd "$current" && pwd -P)
for d in "$web"/build "$web"/build-*; do
    [ -d "$d" ] || continue
    if [ "$(cd "$d" && pwd -P)" = "$current_real" ]; then
        continue
    fi
    echo "[sigflow] 删除 $d"
    rm -rf "$d"
done
echo "[sigflow] 清理当前构建目录中的编译产物：$current"
"$cmake_bin" --build "$current" --target clean >/dev/null
echo "[sigflow] 当前构建目录已清理（保留 CMake 配置）"
echo "[sigflow] 如需彻底删除：rm -rf '$current'"
]=])
add_custom_target(distclean
    COMMAND ${CMAKE_COMMAND} -E env
            "SIGFLOW_WEB_DIR=${SIGFLOW_WEB_DIR}"
            "SIGFLOW_CURRENT_BUILD=${CMAKE_BINARY_DIR}"
            "SIGFLOW_CMAKE=${CMAKE_COMMAND}"
            sh "${SIGFLOW_DISTCLEAN_SCRIPT}"
    COMMENT "清理编译产物（保留用户工程与数据库）"
    EXCLUDE_FROM_ALL
    USES_TERMINAL
)

# proj-empty：清空所有用户工程。破坏性操作，必须显式 export SIGFLOW_WIPE_PROJECTS=yes。
# 脚本在 configure 期生成，避免把多行内容塞进 Makefile recipe。
set(SIGFLOW_WIPE_SCRIPT "${CMAKE_BINARY_DIR}/sigflow-wipe-projects.sh")
file(WRITE "${SIGFLOW_WIPE_SCRIPT}" [=[#!/bin/sh
set -eu
if [ "${SIGFLOW_WIPE_PROJECTS:-}" != "yes" ]; then
    echo "[sigflow] 拒绝执行：这会删除所有用户工程。" >&2
    echo "[sigflow] 确认后请执行：export SIGFLOW_WIPE_PROJECTS=yes" >&2
    exit 1
fi
root="${SIGFLOW_PROJECTS_ROOT}"
[ -d "$root" ] || { echo "[sigflow] 目录不存在：$root" >&2; exit 1; }
count=$(find "$root" -mindepth 1 -maxdepth 1 ! -name .gitkeep | wc -l)
echo "[sigflow] 将清空 $root 下的 $count 项"
find "$root" -mindepth 1 -delete
: > "$root/.gitkeep"
echo "[sigflow] 已清空用户工程"
]=])
add_custom_target(proj-empty
    COMMAND ${CMAKE_COMMAND} -E env
            "SIGFLOW_PROJECTS_ROOT=${SIGFLOW_WEB_DIR}/backend/userprojects"
            sh "${SIGFLOW_WIPE_SCRIPT}"
    COMMENT "清空所有用户工程（需 SIGFLOW_WIPE_PROJECTS=yes）"
    EXCLUDE_FROM_ALL
    USES_TERMINAL
)

# 运行与测试依赖 node；未安装时只提示，不影响 C++ 构建。
find_program(SIGFLOW_NODE_EXECUTABLE NAMES node)
# Node 默认从 web/build/bin 找 sigflow-tool；这里显式指向当前构建产物，
# 避免 -B 到其它目录（如 build/web）时找不到。
set(_sigflow_node_env "")
if(TARGET sigflow-tool)
    set(_sigflow_node_env ${CMAKE_COMMAND} -E env
        "SIGFLOW_TOOL_BIN=$<TARGET_FILE:sigflow-tool>")
endif()
if(SIGFLOW_NODE_EXECUTABLE)
    add_custom_target(run
        COMMAND ${_sigflow_node_env} "${SIGFLOW_NODE_EXECUTABLE}" src/index.js
        WORKING_DIRECTORY "${SIGFLOW_JS_DIR}"
        COMMENT "启动 Node 主进程（读取 web/config.txt）"
        EXCLUDE_FROM_ALL
        USES_TERMINAL
    )
    add_custom_target(web_js_test
        COMMAND ${_sigflow_node_env} "${SIGFLOW_NODE_EXECUTABLE}" --test "${SIGFLOW_JS_DIR}/test/"
        WORKING_DIRECTORY "${SIGFLOW_JS_DIR}"
        COMMENT "运行 Node 侧测试"
        EXCLUDE_FROM_ALL
        USES_TERMINAL
    )
    message(STATUS "[sigflow] node: ${SIGFLOW_NODE_EXECUTABLE}（run / web_js_test 可用）")
else()
    message(STATUS "[sigflow] 未找到 node：run / web_js_test 不可用")
endif()

# ---- C-3. 安装布局 ----------------------------------------------------------
#   <prefix>/bin/sigflow-tool
#   <prefix>/share/sigflow/{config.txt,frontend,static,nodejs}
if(TARGET sigflow-tool)
    install(TARGETS sigflow-tool RUNTIME DESTINATION bin)
endif()
install(FILES "${SIGFLOW_WEB_DIR}/config.txt" DESTINATION share/sigflow)
install(DIRECTORY "${SIGFLOW_FRONTEND_DIR}/" DESTINATION share/sigflow/frontend)
install(DIRECTORY "${SIGFLOW_STATIC_DIR}/" DESTINATION share/sigflow/static)
if(EXISTS "${SIGFLOW_JS_DIR}")
    install(DIRECTORY "${SIGFLOW_JS_DIR}/" DESTINATION share/sigflow/nodejs
        PATTERN "node_modules" EXCLUDE
        PATTERN "test" EXCLUDE)
endif()
