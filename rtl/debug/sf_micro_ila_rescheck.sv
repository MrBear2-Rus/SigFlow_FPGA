// sf_micro_ila_rescheck.sv — 资源检查 wrapper（非交付 RTL）
// probe 用内部计数器、触发条件用常量，仅暴露真实外部引脚，
// 供 nextpnr 布局布线评估采集核的实际 LUT/DFF 占用。
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
        .busy(busy),
        .done(done),
        .triggered(triggered),
        .rd_addr(rd_addr),
        .rd_data(rd_data),
        .trigger_index(trigger_index)
    );
endmodule
