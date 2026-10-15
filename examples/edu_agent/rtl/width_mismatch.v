// 位宽正例（必须命中 RTL_WIDTH_MISMATCH，signal = y）
//
// y 声明为 4 位，却赋了一个 8 位常量：高位被静默截断，工具不报错，
// 但电路行为和学生的意图已经不一致。
module width_mismatch (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = 8'hff;
endmodule
