// min_led_uart_top.v — TraceBridge minimal acceptance user design.
//
// A tiny LED/UART target used by DebugOverlayBuilder to generate the 32-bit x
// 1024-depth ILA overlay. The design intentionally contains observable state:
//   - led[3:0] : free-running 4-bit counter
//   - state[3:0] : registered counter value (same value, internal probe)
//   - tx_busy   : UART heartbeat transmitter busy flag
//   - uart_tx   : UART TX output (heartbeat 0x55)
//
// This is not a complete UART IP; it is just enough to exercise the overlay
// probes and demonstrate hardware-vs-simulation alignment.
module min_led_uart_top (
    input  wire clk,
    input  wire rst_n,
    output wire [3:0] led,
    output wire uart_tx
);
    reg [3:0] state;
    reg [31:0] tick;
    reg tx_busy;
    reg [7:0] tx_shift;
    reg [3:0] bit_count;
    reg [15:0] bit_timer;
    reg uart_tx_r;

    assign led = state;
    assign uart_tx = uart_tx_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) state <= 4'd0;
        else state <= state + 4'd1;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tick <= 32'd0;
            tx_busy <= 1'b0;
            tx_shift <= 8'h55;
            bit_count <= 4'd0;
            bit_timer <= 16'd0;
            uart_tx_r <= 1'b1;
        end else begin
            if (!tx_busy) begin
                tick <= tick + 32'd1;
                if (tick >= 32'd270000) begin
                    tick <= 32'd0;
                    tx_busy <= 1'b1;
                    tx_shift <= 8'h55;
                    bit_count <= 4'd0;
                    bit_timer <= 16'd0;
                    uart_tx_r <= 1'b0; // start bit
                end
            end else begin
                bit_timer <= bit_timer + 16'd1;
                if (bit_timer >= 16'd2700) begin // ~100 us/bit at 27 MHz for 10 kbaud heartbeat
                    bit_timer <= 16'd0;
                    bit_count <= bit_count + 4'd1;
                    case (bit_count)
                        4'd0,4'd1,4'd2,4'd3,4'd4,4'd5,4'd6,4'd7:
                            uart_tx_r <= tx_shift[bit_count];
                        4'd8: uart_tx_r <= 1'b1; // stop bit
                        4'd9: begin
                            tx_busy <= 1'b0;
                            uart_tx_r <= 1'b1;
                        end
                        default: uart_tx_r <= 1'b1;
                    endcase
                end
            end
        end
    end
endmodule
