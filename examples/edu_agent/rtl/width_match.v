// 位宽反例（必须不产生任何 finding）
//
// 常量位宽（4 位）与目标信号（4 位）一致，不需要报位宽问题。
module width_match (
    input  wire [3:0] a,
    output reg  [3:0] y
);
    always @(*) begin
        y = 4'hf;
    end
endmodule
