create_clock -name sys_clk -period 37.037 [get_ports {clk}]
create_clock -name jtag_tck -period 166.667 [get_pins {link/user_jtag/tck_pad_i}]
set_clock_groups -asynchronous -group {sys_clk} -group {jtag_tck}
