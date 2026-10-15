// 敏感列表反例（必须不产生任何 finding）
//
// 1) 电平敏感列表写全           → 不该报
// 2) always @(*) 通配            → 按定义覆盖全部信号
// 3) always @(posedge clk) 时钟块 → 是时序块，不是“组合敏感列表”
module sensitivity_complete (
    input  wire       clk,
    input  wire       ready,
    input  wire       valid,
    input  wire [7:0] data_in,
    output reg        y,
    output reg  [7:0] data_out
);
    always @(ready or valid) begin
        y = ready & valid;
    end

    always @(*) begin
        y = ready & valid;
    end

    always @(posedge clk) begin
        data_out <= data_in;
    end
endmodule
