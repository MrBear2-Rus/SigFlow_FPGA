# =============================================================================
# DeepSeek AI 助手插件（跨平台 MODULE：Windows = .dll，Linux = .so）
#
# 由根 CMakeLists 的 -DSIGFLOW_BUILD_DEEPSEEK_PLUGIN=ON 控制（默认 OFF）。
# 产物输出到 <可执行文件目录>/plugins，主程序 PluginManager 会扫描该目录并
# 按 CreateSigPlugin 导出函数加载（见 main/PluginManager.cpp）。
#
# 依赖：
#   * wxWidgets（core/base）—— 插件自带 UI，且 HTTP 走 wxWebRequest
#   * nlohmann/json         —— 3rd/nlohmann/json.hpp
#
# 注意：wxWebRequest 在 Linux 上依赖 libcurl；若 wx 构建时未找到 libcurl
# （wxUSE_WEBREQUEST=0），Plug_DeepSeek.cpp 会以 #error 明确报出。
# =============================================================================

add_library(sigflow_deepseek MODULE
    Plug_DeepSeek.cpp
    Plug_DeepSeek_helpers.cpp
)

target_include_directories(sigflow_deepseek PRIVATE
    "${CMAKE_SOURCE_DIR}/plugin_sdk"
    "${CMAKE_SOURCE_DIR}/3rd"          # <nlohmann/json.hpp>
    "${SIGFLOW_MAIN_DIR}"              # main/platform/PlatformPaths.h 等
)

if(WIN32)
    target_compile_definitions(sigflow_deepseek PRIVATE
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

if(SIGFLOW_WX_LOCAL)
    # wxWebRequest 在 wxNet（net/webrequest.cpp），必须一并链接。
    target_link_libraries(sigflow_deepseek PRIVATE wx::core wx::base wx::net)
elseif(WIN32)
    # 非本地源码构建：显式给 wx 头目录（<wx/wx.h> / <wx/string.h> 等）。
    target_include_directories(sigflow_deepseek PRIVATE
        "${SIGFLOW_WX_INCLUDE_DIR}"
        "${SIGFLOW_WX_CONFIG_DIR}"
    )
    # wxWebRequest 在 wxNet（net/webrequest.cpp），必须一并链接。
    find_library(SIGFLOW_DEEPSEEK_WX_NET_LIBRARY
        NAMES wxbase33ud_net wxbase32ud_net wx_mswu_net-3.2
        PATHS "${SIGFLOW_WX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(sigflow_deepseek PRIVATE
        "${SIGFLOW_WX_CORE_LIBRARY}"
        "${SIGFLOW_WX_BASE_LIBRARY}"
        "${SIGFLOW_DEEPSEEK_WX_NET_LIBRARY}")
else()
    target_link_libraries(sigflow_deepseek PRIVATE ${wxWidgets_LIBRARIES})
endif()

set_target_properties(sigflow_deepseek PROPERTIES
    PREFIX ""
    OUTPUT_NAME "DeepSeek_Assistant"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:sigflow>/plugins"   # Windows .dll
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:sigflow>/plugins"   # Linux .so
)
