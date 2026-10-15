// 敏感列表正例（必须命中 RTL_SENSITIVITY_INCOMPLETE，signal = valid）
//
// 块内读取了 valid，但敏感列表只写了 ready：valid 变化时块不会重算，
// 仿真波形会和综合结果不一致。
module sensitivity_incomplete (
    input  wire ready,
    input  wire valid,
    output reg  y
);
    always @(ready) begin
        y = ready & valid;
    end
endmodule
