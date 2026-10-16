// sf_offline_snapshot.sv — TraceBridge 可选小型脱机现场快照核（TB33-H02）。
//
// 这是一个独立、有限容量的采样核：它只保存 WIDTH 位探针、冻结原因和
// 少量状态字段，不引入软核、通用日志处理器或默认外部存储。默认不被
// sf_micro_ila / Overlay 实例化；产品集成必须显式加入该选装模块。
//
// 行为约束：
//   * offline_arm 在没有有效现场时显式布防；不会自动重复 ARM。
//   * 布防后每拍写入一个探针样本，形成有限的顺序现场缓冲。
//   * freeze_request 冻结当前写指针位置，snapshot_valid 置位并停止写入。
//   * clear_snapshot 显式清除现场并递增 epoch；若 offline_arm 仍为高，
//     下一拍重新开始一次新的有限采集。
//   * DEPTH 必须是大于 1 的 2 的幂，便于映射为小型 BSRAM。
//
// snapshot_reason=4'h1 表示容量耗尽；其他值由上层 freeze_reason 定义。
// freeze_request 优先级高于容量耗尽，snapshot_index 指向冻结前最后一次
// 写入的地址。读端口为同步读，与现有 ILA 的回读习惯一致。
module sf_offline_snapshot #(
    parameter int DEPTH = 256,
    parameter int WIDTH = 32
) (
    input  logic clk,
    input  logic rst_n,

    input  logic offline_arm,
    input  logic freeze_request,
    input  logic [WIDTH-1:0] probe,
    input  logic [3:0] freeze_reason,
    input  logic clear_snapshot,

    input  logic [$clog2(DEPTH)-1:0] rd_addr,
    output logic [WIDTH-1:0] rd_data,

    output logic snapshot_valid,
    output logic [3:0] snapshot_reason,
    output logic [$clog2(DEPTH)-1:0] snapshot_index,
    output logic [7:0] snapshot_epoch,
    output logic armed
);
    localparam int AW = $clog2(DEPTH);
    localparam logic [AW-1:0] LAST_ADDR = DEPTH - 1;

    (* ram_style = "block" *) logic [WIDTH-1:0] mem [0:DEPTH-1];
    logic [AW-1:0] wptr;

    // 1 is reserved for a bounded-buffer exhaustion event. Zero is a valid
    // caller-defined reason and is preserved when freeze_reason is zero.
    localparam logic [3:0] REASON_DEPTH_EXHAUSTED = 4'h1;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            wptr            <= '0;
            rd_data         <= '0;
            snapshot_valid <= 1'b0;
            snapshot_reason<= 4'h0;
            snapshot_index <= '0;
            snapshot_epoch <= 8'h00;
            armed          <= 1'b0;
        end else begin
            // Synchronous read is independent of capture state.
            rd_data <= mem[rd_addr];

            if (clear_snapshot) begin
                snapshot_valid  <= 1'b0;
                snapshot_reason <= 4'h0;
                snapshot_index  <= '0;
                snapshot_epoch  <= snapshot_epoch + 8'h01;
                wptr            <= '0;
                armed           <= offline_arm;
            end else if (!snapshot_valid && !armed && offline_arm) begin
                // Explicit arm edge starts a fresh bounded ring.
                wptr  <= '0;
                armed <= 1'b1;
            end else if (armed && !snapshot_valid) begin
                mem[wptr] <= probe;

                if (freeze_request) begin
                    snapshot_valid  <= 1'b1;
                    snapshot_reason <= freeze_reason;
                    snapshot_index  <= wptr;
                    armed           <= 1'b0;
                end else if (wptr == LAST_ADDR) begin
                    // A full finite buffer freezes without requiring a second
                    // control plane. The last sample remains readable.
                    snapshot_valid  <= 1'b1;
                    snapshot_reason <= REASON_DEPTH_EXHAUSTED;
                    snapshot_index  <= wptr;
                    armed           <= 1'b0;
                end else begin
                    wptr <= wptr + {{(AW-1){1'b0}}, 1'b1};
                end
            end
        end
    end
endmodule
