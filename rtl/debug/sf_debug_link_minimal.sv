// sf_debug_link_minimal.sv -- low-resource dedicated UART debug link.
//
// This profile is intentionally not wire-compatible with sf_debug_link:
// it uses fixed-size packets and no COBS/CRC.  It is appropriate only when
// dbg_rx/dbg_tx are dedicated to TraceBridge.  Commands:
//   A5 10 mask[31:0] value[31:0]  -> 5A 90
//   A5 11                          -> 5A 90  (arm)
//   A5 12                          -> 5A 92 flags trig_lo trig_hi
//   A5 13 addr_lo addr_hi          -> 5A 93 data[31:0]
//   A5 14                          -> 5A 90  (reset)
module sf_debug_link_minimal #(
    parameter int CLK_HZ = 27000000,
    parameter int BAUD = 921600,
    parameter int WIDTH = 32,
    parameter int DEPTH = 1024,
    parameter logic [63:0] FINGERPRINT64 = 64'h0,
    parameter int IP_VERSION = 1,
    parameter int DATA_BYTES = 4
) (
    input logic clk, input logic rst_n,
    input logic rx, output logic tx,
    output logic arm_pulse, output logic reset_pulse,
    output logic [31:0] trigger_mask, output logic [31:0] trigger_value,
    output logic [7:0] decimation,
    output logic [2:0] trigger_mode, output logic [15:0] trigger_count,
    output logic sample_en,
    input logic busy, input logic done, input logic triggered,
    input logic [$clog2(DEPTH)-1:0] trigger_index,
    output logic [$clog2(DEPTH)-1:0] rd_addr, input logic [WIDTH-1:0] rd_data
);
    localparam int AW = $clog2(DEPTH);
    localparam logic [7:0] RX_SYNC = 8'hA5;
    localparam logic [7:0] CMD_CONFIG = 8'h10;
    localparam logic [7:0] CMD_ARM = 8'h11;
    localparam logic [7:0] CMD_STATUS = 8'h12;
    localparam logic [7:0] CMD_READ = 8'h13;
    localparam logic [7:0] CMD_RESET = 8'h14;
    localparam logic [7:0] RESP_ACK = 8'h90;
    localparam logic [7:0] RESP_STATUS = 8'h92;
    localparam logic [7:0] RESP_SAMPLE = 8'h93;

    localparam logic [3:0] S_IDLE = 0, S_CMD = 1, S_CONFIG = 2,
                           S_READ_LO = 3, S_READ_HI = 4, S_READ_ADDR = 5,
                           S_READ_WAIT = 6, S_READ_CAPTURE = 7,
                           S_TX_START = 8, S_TX_WAIT_BUSY = 9,
                           S_TX_WAIT_DONE = 10;

    logic [3:0] state;
    logic [2:0] cfg_index;
    logic [7:0] response_code;
    logic [2:0] response_len, response_index;
    logic [15:0] read_start;
    logic [31:0] read_data;
    logic [AW-1:0] rd_addr_r;
    logic [31:0] cfg_mask, cfg_value;
    logic [7:0] tx_byte;
    logic tx_start, tx_busy;
    logic rx_valid;
    logic [7:0] rx_byte;
    logic arm_pulse_r, reset_pulse_r;

    sf_uart_link #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_uart (
        .clk(clk), .rst_n(rst_n), .tx_byte(tx_byte), .tx_start(tx_start),
        .tx_busy(tx_busy), .tx(tx), .rx(rx), .rx_byte_valid(rx_valid), .rx_byte(rx_byte)
    );

    wire [7:0] status_flags = {1'b0, done, triggered, busy, 4'b0};
    wire [15:0] trigger_index_padded = {{(16-AW){1'b0}}, trigger_index};

    function automatic [7:0] response_byte(input [2:0] index);
        begin
            response_byte = 8'h00;
            case (index)
                0: response_byte = 8'h5A;
                1: response_byte = response_code;
                default: begin
                    if (response_code == RESP_STATUS) begin
                        case (index)
                            2: response_byte = status_flags;
                            3: response_byte = trigger_index_padded[7:0];
                            4: response_byte = trigger_index_padded[15:8];
                        endcase
                    end else if (response_code == RESP_SAMPLE) begin
                        case (index)
                            2: response_byte = read_data[7:0];
                            3: response_byte = read_data[15:8];
                            4: response_byte = read_data[23:16];
                            5: response_byte = read_data[31:24];
                        endcase
                    end
                end
            endcase
        end
    endfunction

    assign arm_pulse = arm_pulse_r;
    assign reset_pulse = reset_pulse_r;
    assign trigger_mask = cfg_mask;
    assign trigger_value = cfg_value;
    assign decimation = 8'd1;
    assign trigger_mode = 3'd0;
    assign trigger_count = 16'd1;
    assign sample_en = 1'b1;
    assign rd_addr = rd_addr_r;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            cfg_index <= 0;
            response_code <= RESP_ACK;
            response_len <= 2;
            response_index <= 0;
            read_start <= 0;
            read_data <= 0;
            rd_addr_r <= 0;
            cfg_mask <= 32'hFFFFFFFF;
            cfg_value <= 0;
            tx_byte <= 0;
            tx_start <= 0;
            arm_pulse_r <= 0;
            reset_pulse_r <= 0;
        end else begin
            tx_start <= 1'b0;
            arm_pulse_r <= 1'b0;
            reset_pulse_r <= 1'b0;
            case (state)
                S_IDLE: if (rx_valid && rx_byte == RX_SYNC) state <= S_CMD;
                S_CMD: if (rx_valid) begin
                    case (rx_byte)
                        CMD_CONFIG: begin cfg_index <= 0; state <= S_CONFIG; end
                        CMD_ARM: begin
                            arm_pulse_r <= 1'b1; response_code <= RESP_ACK;
                            response_len <= 2; response_index <= 0; state <= S_TX_START;
                        end
                        CMD_STATUS: begin
                            response_code <= RESP_STATUS; response_len <= 5;
                            response_index <= 0; state <= S_TX_START;
                        end
                        CMD_READ: state <= S_READ_LO;
                        CMD_RESET: begin
                            reset_pulse_r <= 1'b1; response_code <= RESP_ACK;
                            response_len <= 2; response_index <= 0; state <= S_TX_START;
                        end
                        default: state <= S_IDLE;
                    endcase
                end
                S_CONFIG: if (rx_valid) begin
                    case (cfg_index)
                        0: cfg_mask[7:0] <= rx_byte;
                        1: cfg_mask[15:8] <= rx_byte;
                        2: cfg_mask[23:16] <= rx_byte;
                        3: cfg_mask[31:24] <= rx_byte;
                        4: cfg_value[7:0] <= rx_byte;
                        5: cfg_value[15:8] <= rx_byte;
                        6: cfg_value[23:16] <= rx_byte;
                        7: cfg_value[31:24] <= rx_byte;
                    endcase
                    if (cfg_index == 7) begin
                        response_code <= RESP_ACK; response_len <= 2;
                        response_index <= 0; state <= S_TX_START;
                    end else cfg_index <= cfg_index + 1'b1;
                end
                S_READ_LO: if (rx_valid) begin read_start[7:0] <= rx_byte; state <= S_READ_HI; end
                S_READ_HI: if (rx_valid) begin read_start[15:8] <= rx_byte; state <= S_READ_ADDR; end
                S_READ_ADDR: begin rd_addr_r <= read_start[AW-1:0]; state <= S_READ_WAIT; end
                S_READ_WAIT: state <= S_READ_CAPTURE;
                S_READ_CAPTURE: begin
                    read_data <= rd_data; response_code <= RESP_SAMPLE; response_len <= 6;
                    response_index <= 0; state <= S_TX_START;
                end
                S_TX_START: if (!tx_busy) begin
                    tx_byte <= response_byte(response_index); tx_start <= 1'b1;
                    state <= S_TX_WAIT_BUSY;
                end
                S_TX_WAIT_BUSY: if (tx_busy) state <= S_TX_WAIT_DONE;
                S_TX_WAIT_DONE: if (!tx_busy) begin
                    if (response_index + 1'b1 >= response_len) state <= S_IDLE;
                    else begin response_index <= response_index + 1'b1; state <= S_TX_START; end
                end
                default: state <= S_IDLE;
            endcase
        end
    end
endmodule
