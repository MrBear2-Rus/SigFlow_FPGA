module top(
    input wire clk,
    input wire write_enable,
    input wire [3:0] address,
    input wire [7:0] data_in,
    output reg [7:0] data_out
);
    reg [7:0] memory [0:15];

    always @(posedge clk) begin
        if (write_enable) begin
            memory[address] <= data_in;
        end
        data_out <= memory[address];
    end
endmodule
