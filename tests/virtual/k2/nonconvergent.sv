module k2_nonconvergent(input wire pulse_clock, output wire unstable);
    wire loop_a;
    wire loop_b;
    assign loop_a = ~loop_b;
    assign loop_b = loop_a;
    assign unstable = loop_a ^ pulse_clock;
endmodule
