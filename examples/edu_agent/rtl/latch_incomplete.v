// 锁存器正例（必须命中 RTL_LATCH_INCOMPLETE_BRANCH，signal = q）
//
// en 为 0 时 q 没有任何赋值，综合工具会为 q 推断出一个锁存器。
// 教学期望：规则引擎必须报出这条问题，并指出是 q 没有覆盖所有分支。
module latch_incomplete (
    input  wire       en,
    input  wire [3:0] d,
    output reg  [3:0] q
);
    always @(*) begin
        if (en) begin
            q = d;
        end
    end
endmodule
