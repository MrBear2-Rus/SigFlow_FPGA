// sf_uart_link.sv — 8N1 UART 字节收发引擎（TraceBridge P1b）
//  - 单时钟域：clk 即 27 MHz 采样时钟，波特率由分频系数生成；
//    27 MHz / 921600 ≈ 29.3 → BAUD_DIV=29，实际波特率 931034，误差约 1.07%（UART 可接受）。
//  - TX：tx_start 拉高一拍发送 tx_byte[7:0]，tx_busy 期间不可再发送。
//  - RX：检测起始位下降沿，半位偏移后在每位中点采样；rx_byte_valid 输出一拍。
module sf_uart_link #(
    parameter int CLK_HZ = 27000000,
    parameter int BAUD   = 921600
) (
    input  logic clk,
    input  logic rst_n,

    // TX
    input  logic [7:0] tx_byte,
    input  logic tx_start,
    output logic tx_busy,
    output logic tx,          // 串行输出，idle = 1

    // RX
    input  logic rx,
    output logic rx_byte_valid,
    output logic [7:0] rx_byte
);
    localparam int DIV = (CLK_HZ + BAUD / 2) / BAUD;  // 每 bit 时钟数（四舍五入）
    localparam int HALF_DIV = DIV / 2;

    // ── TX ──
    logic [9:0] tx_shift;
    logic [3:0] tx_bits;
    int tx_cnt;
    logic tx_running;

    assign tx_busy = tx_running;
    assign tx = tx_running ? tx_shift[0] : 1'b1;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_running <= 1'b0;
            tx_shift   <= '1;
            tx_bits    <= 4'd0;
            tx_cnt     <= 0;
        end else if (!tx_running) begin
            if (tx_start) begin
                tx_running <= 1'b1;
                tx_shift   <= {1'b1, tx_byte, 1'b0};  // 起始位、数据位、停止位
                tx_bits    <= 4'd0;
                tx_cnt     <= DIV - 1;
            end
        end else begin
            if (tx_cnt == 0) begin
                tx_shift <= {1'b1, tx_shift[9:1]};  // 每个 bit 周期末移位
                tx_bits  <= tx_bits + 4'd1;
                tx_cnt   <= DIV - 1;
            end else begin
                tx_cnt <= tx_cnt - 1;
            end
            if (tx_bits == 4'd9 && tx_cnt == 0) begin
                tx_running <= 1'b0;  // 起始位 + 8 数据 + 停止位
            end
        end
    end

    // ── RX ──
    logic rx_sync;
    logic rx_prev;
    logic rx_running;
    int rx_cnt;
    logic [3:0] rx_bits;
    logic [7:0] rx_shift;
    logic rx_valid;
    logic [7:0] rx_data;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_sync     <= 1'b1;
            rx_prev     <= 1'b1;
            rx_running  <= 1'b0;
            rx_cnt      <= 0;
            rx_bits     <= 4'd0;
            rx_shift    <= '0;
            rx_valid    <= 1'b0;
            rx_data     <= '0;
        end else begin
            rx_sync <= rx;
            rx_prev <= rx_sync;
            rx_valid <= 1'b0;

            if (!rx_running) begin
                // 起始位：rx_sync=0 且上一拍为 1（下降沿）
                if (!rx_sync && rx_prev) begin
                    rx_running <= 1'b1;
                    rx_cnt     <= HALF_DIV;  // 半位后采样起始位中点
                    rx_bits    <= 4'd0;
                    rx_shift   <= '0;
                end
            end else begin
                if (rx_cnt != 0) begin
                    rx_cnt <= rx_cnt - 1;
                end else if (rx_bits == 4'd0) begin
                    if (rx_sync) begin
                        rx_running <= 1'b0;
                    end else begin
                        rx_cnt <= DIV - 1;
                        rx_bits <= 4'd1;
                    end
                end else if (rx_bits <= 4'd8) begin
                    rx_shift <= {rx_sync, rx_shift[7:1]};
                    rx_cnt <= DIV - 1;
                    if (rx_bits == 4'd8) begin
                        rx_bits <= 4'd9;
                    end else begin
                        rx_bits <= rx_bits + 4'd1;
                    end
                end else begin
                    if (rx_sync) begin
                        rx_valid <= 1'b1;
                        rx_data  <= rx_shift;
                    end
                    rx_running <= 1'b0;
                end
            end
        end
    end

    assign rx_byte_valid = rx_valid;
    assign rx_byte       = rx_data;

endmodule
