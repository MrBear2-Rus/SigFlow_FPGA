# 平台层：IProcessHost 实现 + DynamicLibrary
#   Windows = CreateProcessW + Job Object；POSIX = fork/exec + killpg
add_library(eda_platform STATIC)
target_link_libraries(eda_platform PUBLIC eda_api)
target_include_directories(eda_platform PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-platform")
if(WIN32)
    target_sources(eda_platform PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-platform/win32/PlatformProcessHost.cpp")
else()
    target_sources(eda_platform PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-platform/posix/PlatformProcessHost.cpp")
endif()
target_sources(eda_platform PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-platform/DynamicLibrary.cpp")
target_sources(eda_platform PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-platform/Sha256.cpp")
target_sources(eda_platform PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-platform/Platform.cpp")

# 内核：日志/版本/事件/schema 校验/Job 服务/插件宿主
add_library(eda_core STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/Logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/Version.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/EventBus.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/SchemaRegistry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/CoreSchemas.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/JobService.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/PluginHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/PluginRegistry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/Toolchain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/Project.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/DeclarativeTool.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-core/CommandBus.cpp"
)

target_link_libraries(eda_core PUBLIC eda_api eda_platform)
target_include_directories(eda_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-platform")

# 静态库需 PIC：动态插件（MODULE）会链接 eda_core（Linux 下必需）。
set_target_properties(eda_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(eda_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# 教育版 Agent Gateway（D-02 选型：cpp-httplib，单头 MIT，跨平台）
#   SIGFLOW_BUILD_EDU_AGENT=ON 时装配；依赖 3rd/httplib/httplib.h（随 3rd.zip 分发）。
# ---------------------------------------------------------------------------
option(SIGFLOW_BUILD_EDU_AGENT "Build the educational-agent EDA Gateway" ON)
if(SIGFLOW_BUILD_EDU_AGENT)
    add_library(eda_agent_gateway STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-agent-gateway/GatewayDto.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/eda-agent-gateway/GatewayServer.cpp")
    target_link_libraries(eda_agent_gateway PUBLIC eda_core eda_platform eda_api)
    target_include_directories(eda_agent_gateway PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(eda_agent_gateway PRIVATE
        "${CMAKE_SOURCE_DIR}/3rd/httplib")
    if(WIN32)
        target_link_libraries(eda_agent_gateway PRIVATE ws2_32)
    endif()
    set_target_properties(eda_agent_gateway PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
