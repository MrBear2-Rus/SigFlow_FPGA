// Phase 4 teaching fixture: upper-layer UART packet CRC checker plus a
// two-flop synchronizer sampling an asynchronous stimulus input.
//
// Packets: [0xAA, len, payload(len), crc8-0x07]. The virtual peer sends fully
// legal 8N1 frames; only the crc_tamper fault flips the upper-layer CRC byte,
// so the bottom-layer bit timing stays legal in every case.
//
// The receiver phase-locks to the start-bit falling edge (8 clock cycles per
// bit on the Phase 4 CRC board) and samples in the middle of every bit.
// The synchronizer exhibits functional CDC sampling behaviour for the seeded
// cdc_sampling_approx fault (never an analog metastability claim).
module crc_cdc_lab(
    input  wire        clock_in,
    input  wire        reset_n,
    input  wire        uart_rx,
    input  wire        async_in,
    output reg  [4:0]  led_n
);
    localparam int MID = 4;        // mid-bit offset in clock cycles
    localparam int PERIOD = 8;     // 27 MHz / 3.375 Mbaud = 8 cycles per bit

    // ---- receiver ----
    reg [1:0] rx_sync = 2'b11;
    reg        running = 0;
    reg [2:0]  phase = 0;
    reg [3:0]  index = 0;          // 0 start, 1..8 data, 9 stop
    reg [7:0]  shift = 0;
    reg [7:0]  byte_value = 0;
    reg        byte_complete = 0;
    reg        framing_error = 0;

    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) begin
            rx_sync <= 2'b11; running <= 0; phase <= 0; index <= 0;
            shift <= 0; byte_value <= 0; byte_complete <= 0; framing_error <= 0;
        end else begin
            rx_sync <= {rx_sync[0], uart_rx};
            byte_complete <= 1'b0;
            if (!running) begin
                if (rx_sync[1] == 1'b1 && rx_sync[0] == 1'b0) begin
                    running <= 1; phase <= 0; index <= 0; shift <= 0;
                end
            end else begin
                phase <= phase + 3'd1;
                if (phase == MID[2:0]) begin
                    if (index == 0) begin
                        if (rx_sync[0]) running <= 0;        // false start
                    end else if (index <= 8) begin
                        shift <= {rx_sync[0], shift[7:1]};   // LSB first
                    end
                    if (index == 9) begin
                        byte_value <= shift;
                        byte_complete <= 1'b1;
                        framing_error <= ~rx_sync[0];
                        running <= 0;
                    end
                    index <= index + 4'd1;
                end
            end
        end
    end

    // ---- packet assembler + crc8-0x07 checker ----
    function automatic [7:0] crc8_byte(input [7:0] data, input [7:0] seed);
        reg [7:0] c; integer b;
        begin
            c = seed ^ data;
            for (b = 0; b < 8; b = b + 1)
                c = c[7] ? ((c << 1) ^ 8'h07) : (c << 1);
            crc8_byte = c;
        end
    endfunction

    reg [7:0]  pkt_crc = 0, pkt_len = 0, pkt_index = 0;
    reg        pkt_active = 0, pkt_done = 0;
    reg [15:0] good_count = 0, bad_count = 0;

    // ---- two-flop synchronizer for the asynchronous stimulus ----
    reg sync_a = 1'b1, sync_b = 1'b1;

    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) begin
            pkt_crc <= 0; pkt_len <= 0; pkt_index <= 0; pkt_active <= 0; pkt_done <= 0;
            good_count <= 0; bad_count <= 0; sync_a <= 1'b1; sync_b <= 1'b1;
            led_n <= 5'b11111;
        end else begin
            sync_a <= async_in; sync_b <= sync_a;
            if (byte_complete) begin
                if (!pkt_active && byte_value == 8'hAA) begin
                    pkt_active <= 1; pkt_len <= 0; pkt_index <= 0;
                    pkt_crc <= crc8_byte(8'hAA, 8'h00);
                end else if (pkt_active && pkt_index == 0) begin
                    pkt_len <= byte_value; pkt_index <= 1;
                    pkt_crc <= crc8_byte(byte_value, crc8_byte(8'hAA, 8'h00));
                end else if (pkt_active && pkt_index <= pkt_len) begin
                    pkt_crc <= crc8_byte(byte_value, pkt_crc);
                    pkt_index <= pkt_index + 1;
                end else if (pkt_active) begin
                    pkt_active <= 0; pkt_done <= 1;
                    if (byte_value == pkt_crc) good_count <= good_count + 1;
                    else bad_count <= bad_count + 1;
                end
            end
            led_n[0] <= ~pkt_done;
            led_n[1] <= (good_count != 0) ? 1'b0 : 1'b1;
            led_n[2] <= (bad_count != 0) ? 1'b0 : 1'b1;
            led_n[3] <= ~sync_b;
            led_n[4] <= framing_error;
        end
    end
endmodule
