module k2_counter (
    input wire clock_in,
    input wire reset_n,
    input wire key_n,
    output wire [5:0] led_n
);
    reg [5:0] count;
    always @(posedge clock_in or negedge reset_n)
        if (!reset_n) count <= 0;
        else if (!key_n) count <= count + 1'b1;
    assign led_n = ~count;
endmodule
