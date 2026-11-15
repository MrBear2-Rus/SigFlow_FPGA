module k2_shifted (
    input wire pulse_clock,
    input wire [7:4] keys_data,
    input wire [0:0] one_flag,
    input wire [63:0] wide_bits,
    output wire [4:7] lamps_up,
    output wire [0:0] one_result,
    output wire [63:0] wide_out
);
    assign lamps_up = keys_data;
    assign one_result = ~one_flag;
    assign wide_out = ~wide_bits;
endmodule
