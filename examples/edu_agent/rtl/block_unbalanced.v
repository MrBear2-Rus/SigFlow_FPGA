// 结构正例（必须命中 RTL_BLOCK_UNBALANCED）
//
// 组合块少了一个 end，begin/end 数量对不上：源码被截断或编辑时删掉了结尾，
// 这种情况下其它诊断结论都不可靠。
module block_unbalanced (
    input  wire a,
    input  wire b,
    output reg  y
);
    always @(*) begin
        if (a)
            y = b;
        else
            y = 1'b0;
endmodule
