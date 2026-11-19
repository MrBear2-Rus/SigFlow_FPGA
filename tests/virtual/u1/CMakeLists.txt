# The host assembles the actual production widgets, not GUI stand-ins.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(U1_GUI_SOURCES)
foreach(source IN LISTS SIGFLOW_MAIN_SOURCES)
    if(source MATCHES "/(wave|trace)/")
        list(APPEND U1_GUI_SOURCES "${source}")
    endif()
endforeach()
add_executable(u1_m0_full_product_acceptance gui_acceptance.cpp ${U1_GUI_SOURCES}
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualProjectSessionBuilder.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualSessionController.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualBoardPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/VirtualUartPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/FaultInjectionPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/virtual/BoardRenderer.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaPinBindingPanel.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaPinData.cpp"
    "${SIGFLOW_MAIN_DIR}/fpga/FpgaConstraint.cpp"
    "${SIGFLOW_MAIN_DIR}/debug/DebugBehaviorSummary.cpp"
    "${SIGFLOW_MAIN_DIR}/ui/UiTheme.cpp"
    "${SIGFLOW_3RD_DIR}/json/jsoncpp.cpp")
target_include_directories(u1_m0_full_product_acceptance PRIVATE "${SIGFLOW_MAIN_DIR}" "${SIGFLOW_3RD_DIR}" "${SIGFLOW_3RD_DIR}/vcd" "${CMAKE_SOURCE_DIR}/core/src" ${SIGFLOW_PLUGIN_INCLUDE_DIRS})
target_link_libraries(u1_m0_full_product_acceptance PRIVATE eda_virtual_board eda_core eda-cst-gowin-cst eda-target-tangnano9k eda-wave-vcd)
if(SIGFLOW_WX_LOCAL)
    target_link_libraries(u1_m0_full_product_acceptance PRIVATE wx::core wx::base wx::aui wx::gl)
elseif(WIN32)
    target_include_directories(u1_m0_full_product_acceptance PRIVATE "${SIGFLOW_WX_INCLUDE_DIR}" "${SIGFLOW_WX_CONFIG_DIR}")
    target_compile_definitions(u1_m0_full_product_acceptance PRIVATE WIN32 _WINDOWS NOPCH WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
    target_link_libraries(u1_m0_full_product_acceptance PRIVATE "${SIGFLOW_WX_CORE_LIBRARY}" "${SIGFLOW_WX_BASE_LIBRARY}" "${SIGFLOW_WX_AUI_LIBRARY}" "${SIGFLOW_WX_GL_LIBRARY}" opengl32)
else()
    target_include_directories(u1_m0_full_product_acceptance PRIVATE ${wxWidgets_INCLUDE_DIRS})
    target_link_libraries(u1_m0_full_product_acceptance PRIVATE ${wxWidgets_LIBRARIES} GL)
endif()
if(WIN32)
    foreach(WX_DLL ${JOB_TESTS_WX_RUNTIME_DLLS})
        add_custom_command(TARGET u1_m0_full_product_acceptance POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WX_DLL}" "$<TARGET_FILE_DIR:u1_m0_full_product_acceptance>/")
    endforeach()
endif()
add_dependencies(u1_m0_full_product_acceptance sigflow_session_validate sigflow_port_manifest sigflow_adapter_codegen)
add_test(NAME u1_m0_full_product_acceptance COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/virtual/verify_u1_gui.py" --build-dir "${CMAKE_BINARY_DIR}" --gui "$<TARGET_FILE:u1_m0_full_product_acceptance>")
set_tests_properties(u1_m0_full_product_acceptance PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
