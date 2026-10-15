// 锁存器正例（计数器风格，必须命中 RTL_LATCH_INCOMPLETE_BRANCH，signal = count_next）
//
// rst 与 en 同时不成立时 count_next 没有赋值 → 推断锁存器。
// 学生常见的“计数器复位”写法，正是这条规则的典型命中场景。
module counter_reset_latch (
    input  wire       rst,
    input  wire       en,
    input  wire [3:0] count,
    output reg  [3:0] count_next
);
    always @(*) begin
        if (rst) begin
            count_next = 4'd0;
        end else if (en) begin
            count_next = count + 4'd1;
        end
    end
endmodule
