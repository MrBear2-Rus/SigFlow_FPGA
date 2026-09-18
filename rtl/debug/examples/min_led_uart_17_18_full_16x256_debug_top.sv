module sf_debug_full_17_18_16x256_top (
    input wire clk,
    input wire rst_n,
    output wire [3:0] led,
    output wire uart_tx,
    input wire dbg_rx,
    output wire dbg_tx
);
    min_led_uart_top u_dut (
        .clk(clk), .rst_n(rst_n), .led(led), .uart_tx(uart_tx)
    );

    (* keep *) wire [15:0] probe_bus;
    assign probe_bus[3:0] = led;
    assign probe_bus[7:4] = led;
    assign probe_bus[15:8] = 8'b0;

    wire arm, reset_pulse;
    wire [31:0] trigger_mask, trigger_value;
    wire [7:0] decimation;
    wire [2:0] trigger_mode;
    wire [15:0] trigger_count;
    wire sample_en, busy, done, triggered;
    wire [7:0] trigger_index, rd_addr;
    wire [15:0] rd_data;

    sf_micro_ila #(.DEPTH(256), .WIDTH(16)) u_ila (
        .clk(clk), .rst_n(1'b1), .probe(probe_bus), .arm(arm),
        .trigger_mask(trigger_mask[15:0]), .trigger_value(trigger_value[15:0]),
        .trigger_mode(trigger_mode), .trigger_count(trigger_count),
        .decimation(decimation),
        .sample_en(sample_en), .hs_valid(1'b0), .hs_ready(1'b1),
        .busy(busy), .done(done), .triggered(triggered), .rd_addr(rd_addr),
        .rd_data(rd_data), .trigger_index(trigger_index)
    );

    sf_debug_link #(
        .CLK_HZ(27000000), .BAUD(921600), .WIDTH(16), .DEPTH(256)
    ) u_link (
        .clk(clk), .rst_n(1'b1), .rx(dbg_rx), .tx(dbg_tx),
        .arm_pulse(arm), .reset_pulse(reset_pulse),
        .trigger_mask(trigger_mask), .trigger_value(trigger_value),
        .decimation(decimation), .trigger_mode(trigger_mode),
        .trigger_count(trigger_count), .sample_en(sample_en),
        .busy(busy), .done(done), .triggered(triggered),
        .trigger_index(trigger_index), .rd_addr(rd_addr), .rd_data(rd_data)
    );
endmodule
