add_library(sigflow_virtual_debug STATIC
    "${SIGFLOW_MAIN_DIR}/debug/DebugProtocol.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/CaptureDecoder.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugContract.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugAcquisition.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugSession.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugFingerprint.cpp"
    "${SIGFLOW_MAIN_DIR}/jobs/Sha256.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualCaptureArtifacts.cpp"
    "${SIGFLOW_3RD_DIR}/json/jsoncpp.cpp")
target_include_directories(sigflow_virtual_debug PUBLIC "${SIGFLOW_MAIN_DIR}" "${SIGFLOW_3RD_DIR}" "${CMAKE_SOURCE_DIR}/core/src")
target_link_libraries(sigflow_virtual_debug PUBLIC eda_api eda_platform)
if(SIGFLOW_WX_LOCAL)
    target_link_libraries(sigflow_virtual_debug PUBLIC wx::base wx::core)
elseif(WIN32)
    target_include_directories(sigflow_virtual_debug PUBLIC "${SIGFLOW_WX_INCLUDE_DIR}" "${SIGFLOW_WX_CONFIG_DIR}")
    target_compile_definitions(sigflow_virtual_debug PUBLIC WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
    target_link_libraries(sigflow_virtual_debug PUBLIC "${SIGFLOW_WX_BASE_LIBRARY}" "${SIGFLOW_WX_CORE_LIBRARY}")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/p2-debug-link.json" CONTENT "{\"library\":\"$<TARGET_FILE:sigflow_virtual_debug>\",\"wx_base\":\"${SIGFLOW_WX_BASE_LIBRARY}\",\"wx_core\":\"${SIGFLOW_WX_CORE_LIBRARY}\"}")
endif()
