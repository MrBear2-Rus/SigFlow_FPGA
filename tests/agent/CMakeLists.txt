# SF-01：edu-agent v1 契约 fixtures 校验（不依赖 HTTP 库）
add_executable(edu_contract_smoke edu_contract_smoke.cpp)
target_link_libraries(edu_contract_smoke PRIVATE eda_core eda_platform)
target_include_directories(edu_contract_smoke PRIVATE "${CMAKE_SOURCE_DIR}/core/src")

add_test(NAME edu_contract_smoke
    COMMAND edu_contract_smoke "${CMAKE_SOURCE_DIR}/contracts/edu-agent/v1")

# SF-01：EDA Gateway 最小脚手架冒烟（仅 SIGFLOW_BUILD_EDU_AGENT=ON 时构建）
if(TARGET eda_agent_gateway)
    add_executable(edu_gateway_smoke edu_gateway_smoke.cpp)
    target_link_libraries(edu_gateway_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_gateway_smoke PRIVATE "${CMAKE_SOURCE_DIR}/3rd/httplib")
    add_test(NAME edu_gateway_smoke COMMAND edu_gateway_smoke)

    add_executable(edu_snapshot_grant_smoke edu_snapshot_grant_smoke.cpp)
    target_link_libraries(edu_snapshot_grant_smoke PRIVATE eda_agent_gateway)
    add_test(NAME edu_snapshot_grant_smoke COMMAND edu_snapshot_grant_smoke)

    # SF-06：报告归一化（edu.jobreport.v1）
    add_executable(edu_report_normalizer_smoke edu_report_normalizer_smoke.cpp)
    target_link_libraries(edu_report_normalizer_smoke PRIVATE eda_agent_gateway)
    add_test(NAME edu_report_normalizer_smoke COMMAND edu_report_normalizer_smoke)

    # SF-02：故障与边界注入（端口占用、关闭中请求、坏 JSON、跨项目、并发幂等、伪造/过期凭据）
    add_executable(edu_gateway_fault_smoke edu_gateway_fault_smoke.cpp)
    target_link_libraries(edu_gateway_fault_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_gateway_fault_smoke PRIVATE "${CMAKE_SOURCE_DIR}/3rd/httplib")
    add_test(NAME edu_gateway_fault_smoke COMMAND edu_gateway_fault_smoke)

    # SF-02：Job 生命周期事件源（状态变化 → 规范事件；幂等去重、不伪造终态）
    add_executable(edu_job_event_source_smoke edu_job_event_source_smoke.cpp)
    target_link_libraries(edu_job_event_source_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_job_event_source_smoke PRIVATE "${CMAKE_SOURCE_DIR}/3rd/httplib")
    add_test(NAME edu_job_event_source_smoke COMMAND edu_job_event_source_smoke)

    # NG-05：局部设计证据与未保存缓冲边界（available/ambiguous/stale/unavailable）
    add_executable(edu_design_context_smoke edu_design_context_smoke.cpp)
    target_link_libraries(edu_design_context_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_design_context_smoke PRIVATE "${CMAKE_SOURCE_DIR}/3rd/httplib")
    add_test(NAME edu_design_context_smoke COMMAND edu_design_context_smoke)

    # NG-09：旧 main/jobs 历史只读桥（legacy_unverified、不泄漏本机路径、只读）
    add_executable(edu_legacy_bridge_smoke edu_legacy_bridge_smoke.cpp)
    target_link_libraries(edu_legacy_bridge_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_legacy_bridge_smoke PRIVATE "${CMAKE_SOURCE_DIR}/3rd/httplib")
    add_test(NAME edu_legacy_bridge_smoke COMMAND edu_legacy_bridge_smoke)

    # NG-08：可信事件源与快照保留调度（引用保护、节流、逐工程报告）
    add_executable(edu_event_retention_smoke edu_event_retention_smoke.cpp)
    target_link_libraries(edu_event_retention_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_event_retention_smoke PRIVATE "${CMAKE_SOURCE_DIR}/3rd/httplib")
    add_test(NAME edu_event_retention_smoke COMMAND edu_event_retention_smoke)

    # NG-07：自研 Python sidecar 与宿主握手的真实进程联调（缺 Python 时显式 SKIP）。
    # 直接编译 AgentServiceController.cpp，用其自身的握手校验判定真实 ready 行。
    # 本地 wx 构建导出 wx::base；Windows 的预编译 wx 接入则只提供
    # SIGFLOW_WX_BASE_LIBRARY。两种受支持配置都应生成同一份真实 sidecar
    # 联调测试，不能让 Windows 安装路径静默丢失覆盖。
    if(TARGET wx::base OR (WIN32 AND SIGFLOW_WX_BASE_LIBRARY))
        add_executable(edu_agent_sidecar_smoke
            edu_agent_sidecar_smoke.cpp
            "${SIGFLOW_MAIN_DIR}/agent/AgentServiceController.cpp"
            "${SIGFLOW_3RD_DIR}/json/jsoncpp.cpp")
        target_include_directories(edu_agent_sidecar_smoke PRIVATE
            "${CMAKE_SOURCE_DIR}/core/src"
            "${CMAKE_SOURCE_DIR}"
            "${SIGFLOW_MAIN_DIR}"
            "${SIGFLOW_3RD_DIR}"
            "${CMAKE_SOURCE_DIR}/3rd/httplib")
        target_compile_definitions(edu_agent_sidecar_smoke PRIVATE
            SIGFLOW_REPO_ROOT="${CMAKE_SOURCE_DIR}")
        target_link_libraries(edu_agent_sidecar_smoke PRIVATE eda_core)
        if(TARGET wx::base)
            target_link_libraries(edu_agent_sidecar_smoke PRIVATE wx::base)
        else()
            target_include_directories(edu_agent_sidecar_smoke PRIVATE
                "${SIGFLOW_WX_INCLUDE_DIR}"
                "${SIGFLOW_WX_CONFIG_DIR}")
            target_link_libraries(edu_agent_sidecar_smoke PRIVATE
                "${SIGFLOW_WX_BASE_LIBRARY}")
        endif()
        if(WIN32)
            target_compile_definitions(edu_agent_sidecar_smoke PRIVATE
                WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
            target_link_libraries(edu_agent_sidecar_smoke PRIVATE ws2_32)
        endif()
        add_test(NAME edu_agent_sidecar_smoke COMMAND edu_agent_sidecar_smoke)
        # wx 运行时 DLL 位于构建根目录，测试进程需要从那里启动。
        set_tests_properties(edu_agent_sidecar_smoke PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif()

    # SF-03：真实 Yosys/Verilator 经官方插件与 Gateway 的端到端（工具缺失时显式 SKIP）
    add_executable(edu_real_tool_smoke edu_real_tool_smoke.cpp)
    target_link_libraries(edu_real_tool_smoke PRIVATE
        eda_agent_gateway eda_core eda_platform
        eda-synth-yosys eda-sim-verilator eda-wave-vcd)
    target_include_directories(edu_real_tool_smoke PRIVATE
        "${CMAKE_SOURCE_DIR}/3rd/httplib"
        "${CMAKE_SOURCE_DIR}/core/src"
        "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-synth-yosys/include"
        "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-sim-verilator/include"
        "${CMAKE_SOURCE_DIR}/InnerPlugin/eda-wave-vcd/include")
    target_compile_definitions(edu_real_tool_smoke PRIVATE
        SIGFLOW_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME edu_real_tool_smoke COMMAND edu_real_tool_smoke)

    # NG-10（前半）：真实响应 data 的 schema 运行期校验 + 边界/限额矩阵
    add_executable(edu_schema_runtime_smoke edu_schema_runtime_smoke.cpp)
    target_link_libraries(edu_schema_runtime_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_schema_runtime_smoke PRIVATE
        "${CMAKE_SOURCE_DIR}/3rd/httplib" "${CMAKE_SOURCE_DIR}/core/src")
    # 契约 schema 目录在运行期从仓库读取（与 edu_contract_smoke 同源）。
    target_compile_definitions(edu_schema_runtime_smoke PRIVATE
        SIGFLOW_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME edu_schema_runtime_smoke COMMAND edu_schema_runtime_smoke)

    # NG-06→NG-10 故障矩阵：跨工程隔离、产物失效、取消语义、幂等冲突、双工程并发
    add_executable(edu_fault_matrix_smoke edu_fault_matrix_smoke.cpp)
    target_link_libraries(edu_fault_matrix_smoke PRIVATE eda_agent_gateway)
    target_include_directories(edu_fault_matrix_smoke PRIVATE
        "${CMAKE_SOURCE_DIR}/3rd/httplib" "${CMAKE_SOURCE_DIR}/core/src")
    add_test(NAME edu_fault_matrix_smoke COMMAND edu_fault_matrix_smoke)
endif()
