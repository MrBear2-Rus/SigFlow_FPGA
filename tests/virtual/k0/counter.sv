`timescale 1ps/1ps
// K0 harness fixture only: these explicit port accesses are NOT a virtual-board
// mapping. Product board wiring must be supplied by the CST/PadMap work in K1/K4.
module k0_counter (
    input logic clock_in,
    input logic reset_n,
    input logic gate_in,
    output logic [3:0] observed
);
    always_ff @(posedge clock_in) begin
        if (!reset_n) observed <= 4'd0;
        else if (gate_in) observed <= observed + 4'd1;
    end
endmodule
