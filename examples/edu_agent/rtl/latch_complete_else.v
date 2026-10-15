// 锁存器反例（完整 if/else，必须不产生任何 finding）
//
// 两条控制路径都给了 q 一个值，组合块不会推断出锁存器。
module latch_complete_else (
    input  wire       en,
    input  wire [3:0] d,
    output reg  [3:0] q
);
    always @(*) begin
        if (en) begin
            q = d;
        end else begin
            q = 4'd0;
        end
    end
endmodule
