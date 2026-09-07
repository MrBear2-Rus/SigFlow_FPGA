// sf_micro_ila_rescheck.sv — 资源检查 wrapper（非交付 RTL）
// probe 用内部计数器、触发条件用常量，仅暴露真实外部引脚，
// 供 nextpnr 布局布线评估采集核的实际 LUT/DFF 占用。
// 增强触发输入（mode/count/sample_en/hs_*）由 probe 位动态驱动，
// 防止综合常量折叠，从而如实测量触发引擎的资源与时序成本。
module sf_micro_ila_rescheck #(
    parameter int DEPTH = 1024,
    parameter int WIDTH = 32
) (
    input  logic clk,
    input  logic rst_n,
    input  logic arm,
    input  logic [$clog2(DEPTH)-1:0] rd_addr,
    output logic busy,
    output logic done,
    output logic triggered,
    output logic [$clog2(DEPTH)-1:0] trigger_index,
    output logic [31:0] rd_data
);
    logic [WIDTH-1:0] probe;
    localparam logic [WIDTH-1:0] kTriggerMask = 32'h000000FF;
    localparam logic [WIDTH-1:0] kTriggerValue = 32'h0000002A;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) probe <= '0;
        else probe <= probe + 1'b1;
    end

    sf_micro_ila #(
        .DEPTH(DEPTH),
        .WIDTH(WIDTH)
    ) u_ila (
        .clk(clk),
        .rst_n(rst_n),
        .probe(probe),
        .arm(arm),
        .trigger_mask(kTriggerMask),
        .trigger_value(kTriggerValue),
        .trigger_mode(probe[1:0]),
        .trigger_count({12'h000, probe[15:12]}),
        .sample_en(probe[2]),
        .hs_valid(probe[3]),
        .hs_ready(~probe[4]),
        .busy(busy),
        .done(done),
        .triggered(triggered),
        .rd_addr(rd_addr),
        .rd_data(rd_data),
        .trigger_index(trigger_index)
    );
endmodule
