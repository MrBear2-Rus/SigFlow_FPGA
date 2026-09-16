// top.v — 无 GUI FPGA 工具链探针用设计
//
// 目标器件：Sipeed Tang Nano 9K（GW1N-9C / GW1NR-LV9QN88PC6/I5）
// 刻意只用到最基础的语法（同步复位计数器 + 位选 + 复制拼接），
// 确保 yosys / nextpnr 的真实实现与桩实现都能稳定处理。
`default_nettype none

module top (
    input  wire       clk,    // 27 MHz 板载时钟
    input  wire       rst_n,  // 低有效复位
    input  wire       btn,    // 按键（用于让组合逻辑不是常量）
    output wire       led,    // 板载 LED（最高位，约 0.6 Hz 闪烁）
    output wire [5:0] leds    // 另外 6 个 LED，按键按下时整体取反
);

    reg [23:0] counter;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            counter <= 24'd0;
        else
            counter <= counter + 24'd1;
    end

    assign led  = counter[23];
    assign leds = counter[22:17] ^ {6{btn}};

endmodule

`default_nettype wire
