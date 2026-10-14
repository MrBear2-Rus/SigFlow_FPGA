// Keep a real output so Gowin synthesis retains the user register.
module register_top(output wire [3:0] dbg_o);
    wire [31:0] user_value;
    sf_gowin_jtag_register user_register(.value(user_value));
    assign dbg_o = user_value[3:0];
endmodule
