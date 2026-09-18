// sf_micro_ila.sv — TraceBridge 精简采集核（P1a，增强触发 + 采样门控）。
//
// 环形采样 + 增强触发 + 采样门控：
//   trigger_mode=0（默认）：掩码相等 (probe & mask) == value；
//       trigger_count=N 时第 N 次匹配触发（N=1 即原行为，mask=0 立即触发）。
//   trigger_mode=1：停滞——掩码子集连续 N 个采样周期不变
//       ((probe & mask) == 上一采样周期的 (probe & mask))。
//   trigger_mode=2：握手超时——hs_valid && !hs_ready 连续 N 个采样周期。
//   trigger_mode=3：上升沿——选中位出现 0→1，累计第 N 次触发。
//   trigger_mode=4：下降沿——选中位出现 1→0，累计第 N 次触发。
//   decimation=N：每 N 个采样时钟保留一个样本（N=0/1 等价于不抽取）。
//   sample_en=0：暂停采样（写指针、触发计数、prev 均不更新），读端口不受影响。
//
// 存储为 Gowin BSRAM（ram_style="block"，yosys 映射 DPX9B 真双端口）：
//   写端口（wptr）与读端口（rd_addr）完全独立，busy=1 采集进行中即可并发读回，
//   支持“边采边传”（streaming upload）。
// 预触发/后触发重排由主机按 trigger_index 完成，RTL 只冻结触发位置。
//
// 资源：默认 32x1024 约 2 块 BSRAM + 少量 LUT/DFF（见 README 实测）。
module sf_micro_ila #(
    parameter int DEPTH = 1024,
    parameter int WIDTH = 32
) (
    input  logic clk,
    input  logic rst_n,

    // 探针（打包后的采样总线）
    input  logic [WIDTH-1:0] probe,

    // 控制（P1b 协议层驱动；新增端口带默认值，旧例化无需改动）
    input  logic arm,
    input  logic [WIDTH-1:0] trigger_mask,
    input  logic [WIDTH-1:0] trigger_value,
    input  logic [2:0] trigger_mode  = 3'd0,
    input  logic [15:0] trigger_count = 16'd1,
    input  logic [7:0] decimation = 8'd1,
    input  logic sample_en = 1'b1,
    input  logic hs_valid  = 1'b0,
    input  logic hs_ready  = 1'b1,

    // 状态
    output logic busy,
    output logic done,
    output logic triggered,

    // 读出（同步读，1 拍延迟；独立于写端口）
    input  logic [$clog2(DEPTH)-1:0] rd_addr,
    output logic [WIDTH-1:0] rd_data,

    // 触发样本所在环形地址（主机重排用）
    output logic [$clog2(DEPTH)-1:0] trigger_index
);
    localparam int AW = $clog2(DEPTH);
    localparam logic [AW-1:0] LAST_ADDR = DEPTH - 1;

    (* ram_style = "block" *) logic [WIDTH-1:0] mem [0:DEPTH-1];
    logic [AW-1:0] wptr;
    logic running;
    logic done_reg;
    logic triggered_reg;
    logic [AW-1:0] trigger_index_reg;
    logic [WIDTH-1:0] rd_data_reg;

    // 触发引擎：共享 16-bit 计数器 + 复用掩码比较器
    logic [15:0] trig_cnt;
    logic [15:0] thresh;
    logic [7:0] decim_cnt;
    logic [WIDTH-1:0] prev_masked;   // 停滞模式：上一采样周期的 (probe & mask)

    logic match_eq;     // 掩码相等
    logic match_stall;  // 掩码子集未变化
    logic match_to;     // 握手超时成立
    logic match_rising; // 选中位出现 0->1
    logic match_falling;// 选中位出现 1->0
    logic fire;         // 本周期触发

    wire [WIDTH-1:0] masked_probe = probe & trigger_mask;

    assign match_eq     = (masked_probe == trigger_value);
    assign match_stall  = (masked_probe == prev_masked);
    assign match_to     = hs_valid && !hs_ready;
    assign match_rising = (((~prev_masked) & masked_probe) != {WIDTH{1'b0}});
    assign match_falling = ((prev_masked & (~masked_probe)) != {WIDTH{1'b0}});

    // N<=1 视为第一次匹配；thresh 运行时配置，避免综合常量折叠。
    assign thresh = (trigger_count <= 16'd1) ? 16'd1 : trigger_count;
    wire sample_tick = (decimation <= 8'd1) || (decim_cnt == 8'd0);

    always_comb begin
        fire = 1'b0;
        case (trigger_mode)
            3'd0: fire = match_eq    && (trig_cnt == thresh - 16'd1);
            3'd1: fire = match_stall && (trig_cnt == thresh - 16'd1);
            3'd2: fire = match_to    && (trig_cnt == thresh - 16'd1);
            3'd3: fire = match_rising && (trig_cnt == thresh - 16'd1);
            3'd4: fire = match_falling && (trig_cnt == thresh - 16'd1);
            default: fire = match_eq && (trig_cnt == thresh - 16'd1);
        endcase
    end

    // 计数下一拍：mode0 累加匹配次数；mode1/2 连续成立才累加，否则清零（饱和于 thresh-1）。
    logic [15:0] trig_cnt_next;
    always_comb begin
        trig_cnt_next = trig_cnt;
        case (trigger_mode)
            3'd0:
                if (match_eq && trig_cnt < thresh - 16'd1)
                    trig_cnt_next = trig_cnt + 16'd1;
            3'd1:
                if (match_stall) begin
                    if (trig_cnt < thresh - 16'd1) trig_cnt_next = trig_cnt + 16'd1;
                end else begin
                    trig_cnt_next = 16'd0;
                end
            3'd2:
                if (match_to) begin
                    if (trig_cnt < thresh - 16'd1) trig_cnt_next = trig_cnt + 16'd1;
                end else begin
                    trig_cnt_next = 16'd0;
                end
            3'd3:
                if (match_rising && trig_cnt < thresh - 16'd1)
                    trig_cnt_next = trig_cnt + 16'd1;
            3'd4:
                if (match_falling && trig_cnt < thresh - 16'd1)
                    trig_cnt_next = trig_cnt + 16'd1;
            default:
                if (match_eq && trig_cnt < thresh - 16'd1)
                    trig_cnt_next = trig_cnt + 16'd1;
        endcase
    end

    // 存储写入（写端口）+ 同步读（读端口，独立，可边采边读）
    always_ff @(posedge clk) begin
        if (running && sample_en && sample_tick) mem[wptr] <= probe;
        rd_data_reg <= mem[rd_addr];
    end

    // 采集状态机 + 触发引擎
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            running           <= 1'b0;
            done_reg          <= 1'b0;
            triggered_reg     <= 1'b0;
            wptr              <= '0;
            trigger_index_reg <= '0;
            trig_cnt          <= 16'd0;
            decim_cnt         <= 8'd0;
            prev_masked       <= '0;
        end else if (arm) begin
            running           <= 1'b1;
            done_reg          <= 1'b0;
            triggered_reg     <= 1'b0;
            wptr              <= '0;
            trig_cnt          <= 16'd0;
            decim_cnt         <= 8'd0;
            prev_masked       <= '0;
        end else if (running && sample_en) begin
            if (decimation <= 8'd1)
                decim_cnt <= 8'd0;
            else if (decim_cnt >= decimation - 8'd1)
                decim_cnt <= 8'd0;
            else
                decim_cnt <= decim_cnt + 8'd1;

            if (sample_tick) begin
                if (!triggered_reg) begin
                    trig_cnt <= trig_cnt_next;
                    if (fire) begin
                        triggered_reg    <= 1'b1;
                        trigger_index_reg <= wptr;
                    end
                end
                prev_masked <= masked_probe;
                if (wptr == LAST_ADDR) begin
                    running  <= 1'b0;
                    done_reg <= 1'b1;
                end
                wptr <= wptr + 1'b1;
            end
        end
    end

    assign busy          = running;
    assign done          = done_reg;
    assign triggered     = triggered_reg;
    assign trigger_index = trigger_index_reg;
    assign rd_data       = rd_data_reg;

endmodule
