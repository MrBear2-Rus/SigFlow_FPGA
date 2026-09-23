# ---------------------------------------------------------------------------
# P2 双向同步回归样例（非 GUI，桩 wxEvtHandler 驱动 SigFlowTree）
# ---------------------------------------------------------------------------
set(SIG_TREE_SYNC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/sig_tree_sync_smoke.cpp"
    "${SIGFLOW_MAIN_DIR}/SigTree.cpp"
    "${SIGFLOW_3RD_DIR}/tree-sitter-verilog/src/parser.c"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/src/lib.c"
)

add_executable(sig_tree_sync_smoke ${SIG_TREE_SYNC_SOURCES})

target_include_directories(sig_tree_sync_smoke PRIVATE
    "${SIGFLOW_MAIN_DIR}"
    "${SIGFLOW_3RD_DIR}"
    "${SIGFLOW_3RD_DIR}/vcd"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/include"
    "${SIGFLOW_3RD_DIR}/tree-sitter/lib/src"
    "${SIGFLOW_3RD_DIR}/tree-sitter-verilog/src"
)

if(WIN32)
    target_compile_definitions(sig_tree_sync_smoke PRIVATE
        WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
endif()

if(SIGFLOW_WX_LOCAL)
    target_link_libraries(sig_tree_sync_smoke PRIVATE wx::core wx::base)
elseif(WIN32)
    target_include_directories(sig_tree_sync_smoke PRIVATE
        "${SIGFLOW_WX_INCLUDE_DIR}"
        "${SIGFLOW_WX_CONFIG_DIR}")
    target_link_libraries(sig_tree_sync_smoke PRIVATE
        "${SIGFLOW_WX_CORE_LIBRARY}"
        "${SIGFLOW_WX_BASE_LIBRARY}")
else()
    target_link_libraries(sig_tree_sync_smoke PRIVATE ${wxWidgets_LIBRARIES})
endif()

if(NOT WIN32)
    target_link_libraries(sig_tree_sync_smoke PRIVATE Threads::Threads)
endif()

# Windows：把 wx 运行时 DLL 拷到测试可执行文件旁（否则 STATUS_DLL_NOT_FOUND）。
if(WIN32)
    if(SIGFLOW_WX_LOCAL)
        file(GLOB_RECURSE SIG_TREE_SYNC_WX_DLLS
            "${CMAKE_BINARY_DIR}/wxWidgets/*.dll"
            "${CMAKE_BINARY_DIR}/wxWidgets/**/*.dll")
    elseif(DEFINED SIGFLOW_WX_LIB_DIR)
        file(GLOB SIG_TREE_SYNC_WX_DLLS
            "${SIGFLOW_WX_LIB_DIR}/../bin/*.dll"
            "${SIGFLOW_WX_LIB_DIR}/*.dll")
    endif()
    foreach(WX_DLL ${SIG_TREE_SYNC_WX_DLLS})
        add_custom_command(TARGET sig_tree_sync_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WX_DLL}"
                    "$<TARGET_FILE_DIR:sig_tree_sync_smoke>/")
    endforeach()
endif()

add_test(NAME sig_tree_sync_smoke COMMAND sig_tree_sync_smoke)
