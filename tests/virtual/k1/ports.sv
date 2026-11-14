module k1_ports (
    input wire scalar_pin,
    input wire [0:0] one_bit_vector,
    output wire [3:0] down,
    output wire [7:4] shifted,
    output wire [4:7] up,
    output wire [0:-2] negative_index
);
    assign down = {4{scalar_pin}};
    assign shifted = {4{one_bit_vector[0]}};
    assign up = down;
    assign negative_index = down[2:0];
endmodule
