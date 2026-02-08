// 测试用全加器模块
// 用于测试 SigFlow 的 Verilator 仿真功能

module fulladder (
    input a,
    input b,
    input cin,
    output sum,
    output cout
);

    assign sum = a ^ b ^ cin;
    assign cout = (a & b) | (b & cin) | (a & cin);

endmodule
