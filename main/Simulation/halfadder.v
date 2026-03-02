// 测试用半加器模块
// 用于测试 SigFlow 的 Verilator 仿真功能

module halfadder (
    input a,
    input b,
    output sum,
    output cout
);

    assign sum = a ^ b;
    assign cout = a & b;

endmodule
