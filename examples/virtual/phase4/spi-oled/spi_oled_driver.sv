// Phase 4 teaching fixture: SPI master driving the board OLED (SSD1306
// teaching subset, mode 0). Streams an init command sequence and a fixed data
// pattern so lit_pixels and frame_sha256 are exactly predictable.
// The optional RES_n pin is not wired on the Phase 4 board, so the panel is
// released at time zero instead of through a reset pulse.
module spi_oled_driver(
    input  wire        clock_in,
    input  wire        reset_n,
    output reg         oled_cs_n,
    output reg         oled_sck,
    output reg         oled_mosi,
    output reg         oled_dc,
    output reg  [4:0]  led_n
);
    localparam int SPI_HALF = 4;
    localparam int SETTLE_CYCLES = 8;

    reg [8:0] script [0:13];   // {dc, byte[7:0]}
    initial begin
        script[0]  = {1'b0, 8'hAE};   // display off
        script[1]  = {1'b0, 8'hA6};   // normal display
        script[2]  = {1'b0, 8'hB0};   // page 0
        script[3]  = {1'b0, 8'h00};   // column low nibble
        script[4]  = {1'b0, 8'h10};   // column high nibble
        script[5]  = {1'b0, 8'hAF};   // display on
        script[6]  = {1'b1, 8'h3C};
        script[7]  = {1'b1, 8'h42};
        script[8]  = {1'b1, 8'h18};
        script[9]  = {1'b1, 8'h24};
        script[10] = {1'b1, 8'hFF};
        script[11] = {1'b1, 8'h81};
        script[12] = {1'b1, 8'h99};
        script[13] = {1'b1, 8'hC3};
    end

    reg [31:0] cyc = 0;
    reg [3:0]  byte_idx = 0;
    reg [2:0]  bit_idx = 0;     // 0..7, MSB first
    reg        high_phase = 0;  // 0: drive MOSI with SCK low; 1: SCK high
    reg        started = 0, done = 0;

    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) begin
            cyc <= 0; byte_idx <= 0; bit_idx <= 0; high_phase <= 0;
            started <= 0; done <= 0;
            oled_cs_n <= 1; oled_sck <= 0; oled_mosi <= 0; oled_dc <= 0;
            led_n <= 5'b11111;
        end else if (!done) begin
            if (!started) begin
                if (cyc == SETTLE_CYCLES-1) begin cyc <= 0; started <= 1; oled_cs_n <= 0; end
                else cyc <= cyc + 1;
            end else begin
                if (cyc == SPI_HALF-1) begin
                    cyc <= 0;
                    if (!high_phase) begin
                        // Low half: present the MSB-first bit; the peripheral
                        // samples on the rising edge (mode 0).
                        oled_mosi <= ((script[byte_idx][7:0] >> (3'd7 - bit_idx)) & 8'h01) != 0;
                        oled_dc <= script[byte_idx][8];
                        high_phase <= 1; oled_sck <= 1;
                    end else begin
                        high_phase <= 0; oled_sck <= 0;
                        if (bit_idx == 7) begin
                            bit_idx <= 0;
                            if (byte_idx == 13) begin
                                done <= 1; oled_cs_n <= 1; led_n <= 5'b11110;  // led0 done
                            end else byte_idx <= byte_idx + 1;
                        end else bit_idx <= bit_idx + 1;
                    end
                end else cyc <= cyc + 1;
            end
        end
    end
endmodule
