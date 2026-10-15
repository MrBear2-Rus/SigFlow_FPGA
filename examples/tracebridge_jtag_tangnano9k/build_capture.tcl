set repo $env(SIGFLOW_JTAG_REPO)
set gowin $env(SIGFLOW_JTAG_GOWIN)
set out $env(SIGFLOW_JTAG_OUT)
create_project -name capture_top -dir $out -pn GW1NR-LV9QN88PC6/I5 -device_version C
add_file -type verilog "$repo/rtl/debug/sf_gowin_jtag_register.sv"
add_file -type verilog "$repo/rtl/debug/sf_micro_ila.sv"
add_file -type verilog "$repo/examples/tracebridge_jtag_tangnano9k/capture_top.sv"
add_file -type cst "$repo/examples/tracebridge_jtag_tangnano9k/capture.cst"
add_file -type sdc "$repo/examples/tracebridge_jtag_tangnano9k/capture.sdc"
add_file -type verilog "$gowin/IDE/data/ipcores/gw_jtag.v"
set_option -output_base_name capture_top
set_option -synthesis_tool gowinsynthesis
set_option -top_module capture_top
set_option -verilog_std sysv2017
set_option -use_jtag_as_gpio 0
set_option -gen_verilog_sim_netlist 1
run all
