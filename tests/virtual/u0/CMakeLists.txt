add_executable(u0_virtual_board_gui_smoke gui_smoke.cpp
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualProjectSessionBuilder.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualSessionController.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/BoardRenderer.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualBoardPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/FaultInjectionPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/trace/LiveTraceSource.cpp"
    "${SIGFLOW_MAIN_DIR}/ui/UiTheme.cpp")
target_include_directories(u0_virtual_board_gui_smoke PRIVATE "${SIGFLOW_MAIN_DIR}")
target_link_libraries(u0_virtual_board_gui_smoke PRIVATE eda_virtual_board)
if(SIGFLOW_WX_LOCAL)
    target_link_libraries(u0_virtual_board_gui_smoke PRIVATE wx::core wx::base wx::aui)
elseif(WIN32)
    target_include_directories(u0_virtual_board_gui_smoke PRIVATE "${SIGFLOW_WX_INCLUDE_DIR}" "${SIGFLOW_WX_CONFIG_DIR}")
    target_compile_definitions(u0_virtual_board_gui_smoke PRIVATE WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
    target_link_libraries(u0_virtual_board_gui_smoke PRIVATE "${SIGFLOW_WX_CORE_LIBRARY}" "${SIGFLOW_WX_BASE_LIBRARY}" "${SIGFLOW_WX_AUI_LIBRARY}")
else()
    target_include_directories(u0_virtual_board_gui_smoke PRIVATE ${wxWidgets_INCLUDE_DIRS})
    target_link_libraries(u0_virtual_board_gui_smoke PRIVATE ${wxWidgets_LIBRARIES})
endif()
if(WIN32)
    foreach(WX_DLL ${JOB_TESTS_WX_RUNTIME_DLLS})
        add_custom_command(TARGET u0_virtual_board_gui_smoke POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WX_DLL}" "$<TARGET_FILE_DIR:u0_virtual_board_gui_smoke>/")
    endforeach()
endif()
add_test(NAME u0_virtual_board_gui_smoke COMMAND u0_virtual_board_gui_smoke "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/.work/k45/gui"
    "${CMAKE_SOURCE_DIR}/.work/k45/real/k2_counter/runner.exe" "${CMAKE_SOURCE_DIR}/.work/k45/real/normal.json")
set_tests_properties(u0_virtual_board_gui_smoke PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED k45_runner RUN_SERIAL TRUE)
