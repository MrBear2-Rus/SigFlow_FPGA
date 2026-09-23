include_guard(GLOBAL)

# eda_add_plugin(<name> SOURCES <files...> [KIND STATIC|MODULE])
#
#   STATIC  —— 官方进程内插件（编译进核心，零 ABI 风险）。
#   MODULE  —— 可外接动态插件（Windows .dll / Linux .so），经冻结 C ABI + 构建指纹加载。
# 进程外（JSON-RPC）分支在 P3 落地；未实现前显式报错，避免静默降级。
function(eda_add_plugin NAME)
    cmake_parse_arguments(EDA_P "" "KIND" "SOURCES" ${ARGN})
    if(NOT EDA_P_KIND)
        set(EDA_P_KIND STATIC)
    endif()
    if(NOT EDA_P_SOURCES)
        message(FATAL_ERROR "eda_add_plugin(${NAME}): SOURCES is required")
    endif()
    if(EDA_P_KIND STREQUAL "STATIC")
        add_library(${NAME} STATIC ${EDA_P_SOURCES})
        target_link_libraries(${NAME} PUBLIC eda_core)
        # 静态自注册 TU 可能无外部符号引用而被链接器丢弃：
        # 需在可执行侧显式调用各插件的锚点函数（见 eda_plugin_force_link_<name>）
        # 或使用 --whole-archive。此处保持普通静态库，由调用方负责锚点。
        return()
    endif()
    if(EDA_P_KIND STREQUAL "MODULE")
        add_library(${NAME} MODULE ${EDA_P_SOURCES})
        target_link_libraries(${NAME} PRIVATE eda_core)
        set_target_properties(${NAME} PROPERTIES PREFIX "")
        return()
    endif()
    message(FATAL_ERROR
        "eda_add_plugin(${NAME}): KIND '${EDA_P_KIND}' not implemented in P0 (STATIC|MODULE)")
endfunction()
