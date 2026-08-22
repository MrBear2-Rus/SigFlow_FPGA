// sf_micro_ila.sv — 精简采集核（TraceBridge P1a）
//
// 单模块设计：环形采样 + 掩码触发 + 触发位置冻结 + 顺序读出。
//  - 触发仅支持掩码相等：((probe & trigger_mask) == trigger_value)；
//    trigger_mask = 0 时等价于立即触发。
//  - ARM 后环形写满 DEPTH 个样本即停止（DONE），不区分触发前后。
//  - 预触发/后触发重排由宿主（CaptureDecoder）按 trigger_index 完成：
//    相对触发时刻 k 的样本位于 mem[(trigger_index + k) % DEPTH]。
//  - 存储用 Gowin BSRAM（`ram_style="block"`，yosys 映射为 SDPX9B），
//    DEPTH/WIDTH 参数化；默认 32x1024 约占用 2 块 BSRAM。
module sf_micro_ila #(
    parameter int DEPTH = 1024,
    parameter int WIDTH = 32
) (
    input  logic clk,
    input  logic rst_n,

    // 探针（打包后的采样总线）
    input  logic [WIDTH-1:0] probe,

    // 控制（P1b 的 CDC/协议层驱动；拉高一拍 ARM）
    input  logic arm,
    input  logic [WIDTH-1:0] trigger_mask,
    input  logic [WIDTH-1:0] trigger_value,

    // 状态
    output logic busy,
    output logic done,
    output logic triggered,

    // 读出
    input  logic [$clog2(DEPTH)-1:0] rd_addr,
    output logic [WIDTH-1:0] rd_data,

    // 触发样本所在环形地址（宿主重排用）
    output logic [$clog2(DEPTH)-1:0] trigger_index
);
    localparam int AW = $clog2(DEPTH);

    (* ram_style = "block" *) logic [WIDTH-1:0] mem [DEPTH];
    logic [AW-1:0] wptr;
    logic running;
    logic done_reg;
    logic triggered_reg;
    logic [AW-1:0] trigger_index_reg;
    logic [WIDTH-1:0] rd_data_reg;

    // 存储写入：running 期间每周期采样；读为同步读（BSRAM 风格）。
    always_ff @(posedge clk) begin
        if (running) mem[wptr] <= probe;
        rd_data_reg <= mem[rd_addr];
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            running          <= 1'b0;
            done_reg         <= 1'b0;
            triggered_reg    <= 1'b0;
            wptr             <= '0;
            trigger_index_reg <= '0;
        end else if (arm) begin
            running          <= 1'b1;
            done_reg         <= 1'b0;
            triggered_reg    <= 1'b0;
            wptr             <= '0;
        end else if (running) begin
            if (!triggered_reg &&
                ((probe & trigger_mask) == trigger_value)) begin
                triggered_reg    <= 1'b1;
                trigger_index_reg <= wptr;
            end

            if (wptr == DEPTH[AW-1:0] - 1) begin
                running  <= 1'b0;
                done_reg <= 1'b1;
            end
            wptr <= wptr + 1'b1;
        end
    end

    assign busy          = running;
    assign done          = done_reg;
    assign triggered     = triggered_reg;
    assign trigger_index = trigger_index_reg;
    assign rd_data = rd_data_reg;

endmodule
