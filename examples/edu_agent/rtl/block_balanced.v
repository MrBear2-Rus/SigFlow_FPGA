// 结构反例（必须不产生任何 finding）
//
// begin/end 配对正确；这里故意使用 `end else begin` 同行写法，
// 确认规则引擎不会因为排版把完整的 if/else 误判成锁存器。
module block_balanced (
    input  wire a,
    input  wire b,
    output reg  y
);
    always @(*) begin
        if (a) begin
            y = b;
        end else begin
            y = 1'b0;
        end
    end
endmodule
