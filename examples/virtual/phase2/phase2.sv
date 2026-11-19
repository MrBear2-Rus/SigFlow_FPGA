// Teaching reference only. Every external pin is routed through phase1.cst.
module p1_lab (
    input wire clock_in, reset_n, key_n,
    input wire uart_rx,
    input wire [1:0] dip,
    output wire [5:0] led_n,
    output reg [7:0] seg_n,
    output wire [3:0] dig_en,
    output wire uart_tx
);
    reg [15:0] count;
    always @(posedge clock_in or negedge reset_n)
        if (!reset_n) count <= 0;
        else count <= count + 1'b1;
    assign led_n[0] = !(count[1:0] < 1); // 25%
    assign led_n[1] = !(count[1:0] < 2); // 50%
    assign led_n[2] = !(count[1:0] < 3); // 75%
    assign led_n[3] = key_n;
    assign led_n[5:4] = ~dip;
    assign dig_en = 4'b0001 << count[4:3];
    always @* case (count[4:3]) // a..g,dp, common anode; display 1 2 3 4
        0: seg_n = ~8'b00000110;
        1: seg_n = ~8'b01011011;
        2: seg_n = ~8'b01001111;
        default: seg_n = ~8'b01100110;
    endcase
    // Actual RTL bit path: pad18 -> DUT -> pad17. No register/byte shortcut.
    assign uart_tx = uart_rx;
endmodule
