module top(
    input wire clk,
    input wire reset_n,
    output reg [7:0] count
);
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            count <= 8'd0;
        end else begin
            count <= count + 8'd1;
        end
    end
endmodule
