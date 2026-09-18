// sf_debug_link.sv -- compact TraceBridge UART protocol core.
//
// The wire protocol remains COBS + CRC-16/CCITT.  Unlike the original
// implementation this core never stores a variable-length frame: RX parses
// fixed command fields after streaming COBS decode, and TX regenerates fixed
// response bytes while streaming COBS encode.  This avoids large LUT mux
// trees from variable indexing on GW1N devices.
module sf_debug_link #(
    parameter int CLK_HZ = 27000000,
    parameter int BAUD   = 921600,
    parameter int WIDTH  = 32,
    parameter int DEPTH  = 1024,
    parameter logic [63:0] FINGERPRINT64 = 64'h0,
    parameter int IP_VERSION = 1,
    parameter int DATA_BYTES = 4
) (
    input  logic clk,
    input  logic rst_n,
    input  logic rx,
    output logic tx,
    output logic arm_pulse,
    output logic reset_pulse,
    output logic [31:0] trigger_mask,
    output logic [31:0] trigger_value,
    output logic [7:0] decimation,
    output logic [2:0] trigger_mode,
    output logic [15:0] trigger_count,
    output logic sample_en,
    input  logic busy,
    input  logic done,
    input  logic triggered,
    input  logic [$clog2(DEPTH)-1:0] trigger_index,
    output logic [$clog2(DEPTH)-1:0] rd_addr,
    input  logic [WIDTH-1:0] rd_data
);
    localparam int AW = $clog2(DEPTH);

    localparam logic [7:0] T_PING     = 8'h01;
    localparam logic [7:0] T_PONG     = 8'h02;
    localparam logic [7:0] T_GET_INFO = 8'h03;
    localparam logic [7:0] T_INFO     = 8'h04;
    localparam logic [7:0] T_CONFIG   = 8'h05;
    localparam logic [7:0] T_ACK      = 8'h06;
    localparam logic [7:0] T_ARM      = 8'h07;
    localparam logic [7:0] T_STATUS   = 8'h09;
    localparam logic [7:0] T_READ     = 8'h0B;
    localparam logic [7:0] T_SAMPLES  = 8'h0C;
    localparam logic [7:0] T_RESET    = 8'h0D;

    localparam logic [2:0] R_ACK     = 3'd0;
    localparam logic [2:0] R_PONG    = 3'd1;
    localparam logic [2:0] R_INFO    = 3'd2;
    localparam logic [2:0] R_STATUS  = 3'd3;
    localparam logic [2:0] R_SAMPLES = 3'd4;

    localparam logic [4:0] S_RX_BEGIN          = 5'd0;
    localparam logic [4:0] S_RX_CODE           = 5'd1;
    localparam logic [4:0] S_RX_DATA           = 5'd2;
    localparam logic [4:0] S_RX_GROUP_DONE     = 5'd3;
    localparam logic [4:0] S_RX_APPLY          = 5'd4;
    localparam logic [4:0] S_RX_APPLY_CODE     = 5'd5;
    localparam logic [4:0] S_RX_FINISH         = 5'd6;
    localparam logic [4:0] S_READ_ADDR         = 5'd7;
    localparam logic [4:0] S_READ_WAIT         = 5'd8;
    localparam logic [4:0] S_READ_CAPTURE      = 5'd9;
    localparam logic [4:0] S_RESP_CRC_INIT     = 5'd10;
    localparam logic [4:0] S_RESP_CRC          = 5'd11;
    localparam logic [4:0] S_TX_START           = 5'd12;
    localparam logic [4:0] S_TX_WAIT_BUSY       = 5'd13;
    localparam logic [4:0] S_TX_WAIT_DONE       = 5'd14;
    localparam logic [4:0] S_TX_SCAN            = 5'd15;
    localparam logic [4:0] S_TX_COPY            = 5'd16;

    localparam logic [2:0] A_LEAD = 3'd0;
    localparam logic [2:0] A_CODE = 3'd1;
    localparam logic [2:0] A_DATA = 3'd2;
    localparam logic [2:0] A_TAIL = 3'd3;

    function automatic [15:0] crc16_update(input [15:0] crc, input [7:0] bdata);
        reg [15:0] c;
        integer i;
        begin
            c = crc ^ {bdata, 8'h00};
            for (i = 0; i < 8; i = i + 1)
                c = c[15] ? ((c << 1) ^ 16'h1021) : (c << 1);
            crc16_update = c;
        end
    endfunction

    logic [7:0] tx_byte;
    logic tx_start;
    logic uart_tx_busy;
    logic uart_rx_byte_valid;
    logic [7:0] uart_rx_byte;

    sf_uart_link #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_uart (
        .clk(clk), .rst_n(rst_n),
        .tx_byte(tx_byte), .tx_start(tx_start), .tx_busy(uart_tx_busy), .tx(tx),
        .rx(rx), .rx_byte_valid(uart_rx_byte_valid), .rx_byte(uart_rx_byte)
    );

    logic [4:0] state;
    logic [4:0] rx_after_apply;
    logic [7:0] rx_left;
    logic [7:0] rx_pending_code;
    logic [7:0] decoded_byte;
    logic [7:0] tail0, tail1;
    logic [7:0] decoded_count;
    logic [7:0] body_count;
    logic [15:0] rx_crc;
    logic version_ok;
    logic [7:0] req_type, req_seq, req_len;
    logic [15:0] read_start;

    logic [31:0] cfg_mask, cfg_value;
    logic [7:0] cfg_decimation;
    logic [2:0] cfg_mode;
    logic [15:0] cfg_count;
    logic arm_pulse_r, reset_pulse_r;
    logic [AW-1:0] rd_addr_r;
    logic [31:0] read_data;

    logic [2:0] response_kind;
    logic [7:0] response_type, response_len, response_data_len;
    logic [15:0] resp_crc;
    logic [7:0] resp_crc_idx, resp_total;
    logic [7:0] enc_src, enc_scan, enc_zero, enc_copy;
    logic [2:0] tx_after;

    wire [15:0] trigger_index_padded = {{(16-AW){1'b0}}, trigger_index};
    wire [7:0] status_flags = {1'b0, done, triggered, busy, 4'b0};

    function automatic [7:0] response_byte(input [7:0] index);
        begin
            response_byte = 8'h00;
            if (index == response_len)
                response_byte = resp_crc[7:0];
            else if (index == response_len + 1'b1)
                response_byte = resp_crc[15:8];
            else if (index == 0)
                response_byte = 8'h01;
            else if (index == 1)
                response_byte = response_type;
            else if (index == 2)
                response_byte = req_seq;
            else if (index == 3)
                response_byte = response_data_len;
            else begin
                case (response_kind)
                    R_PONG: begin
                        if (index == 4) response_byte = IP_VERSION[7:0];
                    end
                    R_INFO: begin
                        case (index)
                            4: response_byte = IP_VERSION[7:0];
                            5: response_byte = FINGERPRINT64[7:0];
                            6: response_byte = FINGERPRINT64[15:8];
                            7: response_byte = FINGERPRINT64[23:16];
                            8: response_byte = FINGERPRINT64[31:24];
                            9: response_byte = FINGERPRINT64[39:32];
                            10: response_byte = FINGERPRINT64[47:40];
                            11: response_byte = FINGERPRINT64[55:48];
                            12: response_byte = FINGERPRINT64[63:56];
                            13: response_byte = DEPTH[7:0];
                            14: response_byte = DEPTH[15:8];
                            15: response_byte = WIDTH[7:0];
                            16: response_byte = status_flags;
                        endcase
                    end
                    R_STATUS: begin
                        case (index)
                            4: response_byte = status_flags;
                            5: response_byte = trigger_index_padded[7:0];
                            6: response_byte = trigger_index_padded[15:8];
                        endcase
                    end
                    R_SAMPLES: begin
                        case (index)
                            4: response_byte = read_start[7:0];
                            5: response_byte = read_start[15:8];
                            6: response_byte = 8'd1;
                            7: response_byte = 8'd0;
                            8: response_byte = read_data[7:0];
                            9: response_byte = read_data[15:8];
                            10: response_byte = read_data[23:16];
                            11: response_byte = read_data[31:24];
                        endcase
                    end
                    default: response_byte = 8'h00;
                endcase
            end
        end
    endfunction

    assign arm_pulse = arm_pulse_r;
    assign reset_pulse = reset_pulse_r;
    assign trigger_mask = cfg_mask;
    assign trigger_value = cfg_value;
    assign decimation = cfg_decimation;
    assign trigger_mode = cfg_mode;
    assign trigger_count = cfg_count;
    assign sample_en = 1'b1;
    assign rd_addr = rd_addr_r;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_RX_BEGIN;
            tx_start <= 1'b0;
            arm_pulse_r <= 1'b0;
            reset_pulse_r <= 1'b0;
            rx_left <= 0;
            rx_pending_code <= 0;
            decoded_byte <= 0;
            tail0 <= 0;
            tail1 <= 0;
            decoded_count <= 0;
            body_count <= 0;
            rx_crc <= 16'hFFFF;
            version_ok <= 1'b0;
            req_type <= 0;
            req_seq <= 0;
            req_len <= 0;
            read_start <= 0;
            cfg_mask <= 32'hFFFFFFFF;
            cfg_value <= 0;
            cfg_decimation <= 8'd1;
            cfg_mode <= 0;
            cfg_count <= 16'd1;
            rd_addr_r <= 0;
            read_data <= 0;
            response_kind <= R_ACK;
            response_type <= T_ACK;
            response_len <= 4;
            response_data_len <= 0;
            resp_crc <= 0;
            resp_crc_idx <= 0;
            resp_total <= 0;
            enc_src <= 0;
            enc_scan <= 0;
            enc_zero <= 0;
            enc_copy <= 0;
            tx_after <= A_LEAD;
            tx_byte <= 0;
        end else begin
            tx_start <= 1'b0;
            arm_pulse_r <= 1'b0;
            reset_pulse_r <= 1'b0;
            case (state)
                S_RX_BEGIN: begin
                    if (uart_rx_byte_valid && uart_rx_byte == 0) begin
                        decoded_count <= 0;
                        body_count <= 0;
                        rx_crc <= 16'hFFFF;
                        version_ok <= 1'b0;
                        req_type <= 0;
                        req_seq <= 0;
                        req_len <= 0;
                        tail0 <= 0;
                        tail1 <= 0;
                        state <= S_RX_CODE;
                    end
                end

                S_RX_CODE: begin
                    if (uart_rx_byte_valid) begin
                        if (uart_rx_byte == 0)
                            state <= S_RX_BEGIN;
                        else begin
                            rx_left <= uart_rx_byte - 1'b1;
                            state <= (uart_rx_byte == 8'd1) ? S_RX_GROUP_DONE : S_RX_DATA;
                        end
                    end
                end

                S_RX_DATA: begin
                    if (uart_rx_byte_valid) begin
                        if (uart_rx_byte == 0)
                            state <= S_RX_BEGIN;
                        else begin
                            decoded_byte <= uart_rx_byte;
                            rx_left <= rx_left - 1'b1;
                            rx_after_apply <= (rx_left == 8'd1) ? S_RX_GROUP_DONE : S_RX_DATA;
                            state <= S_RX_APPLY;
                        end
                    end
                end

                S_RX_GROUP_DONE: begin
                    if (uart_rx_byte_valid) begin
                        if (uart_rx_byte == 0)
                            state <= S_RX_FINISH;
                        else begin
                            decoded_byte <= 0;
                            rx_pending_code <= uart_rx_byte;
                            rx_after_apply <= S_RX_APPLY_CODE;
                            state <= S_RX_APPLY;
                        end
                    end
                end

                S_RX_APPLY: begin
                    if (decoded_count == 0)
                        tail0 <= decoded_byte;
                    else if (decoded_count == 1)
                        tail1 <= decoded_byte;
                    else begin
                        rx_crc <= crc16_update(rx_crc, tail0);
                        case (body_count)
                            0: version_ok <= (tail0 == 8'h01);
                            1: req_type <= tail0;
                            2: req_seq <= tail0;
                            3: req_len <= tail0;
                            4: begin
                                cfg_mask[7:0] <= tail0;
                                read_start[7:0] <= tail0;
                            end
                            5: begin
                                cfg_mask[15:8] <= tail0;
                                read_start[15:8] <= tail0;
                            end
                            6: cfg_mask[23:16] <= tail0;
                            7: cfg_mask[31:24] <= tail0;
                            8: cfg_value[7:0] <= tail0;
                            9: cfg_value[15:8] <= tail0;
                            10: cfg_value[23:16] <= tail0;
                            11: cfg_value[31:24] <= tail0;
                            12: cfg_decimation <= tail0;
                            13: cfg_mode <= tail0[2:0];
                            14: cfg_count[7:0] <= tail0;
                            15: cfg_count[15:8] <= tail0;
                        endcase
                        body_count <= body_count + 1'b1;
                        tail0 <= tail1;
                        tail1 <= decoded_byte;
                    end
                    decoded_count <= decoded_count + 1'b1;
                    state <= rx_after_apply;
                end

                S_RX_APPLY_CODE: begin
                    rx_left <= rx_pending_code - 1'b1;
                    state <= (rx_pending_code == 8'd1) ? S_RX_GROUP_DONE : S_RX_DATA;
                end

                S_RX_FINISH: begin
                    if (decoded_count >= 6 && version_ok &&
                        body_count == (8'd4 + req_len) && rx_crc == {tail1, tail0}) begin
                        case (req_type)
                            T_PING: begin
                                response_kind <= R_PONG;
                                response_type <= T_PONG;
                                response_data_len <= 1;
                                response_len <= 5;
                                state <= S_RESP_CRC_INIT;
                            end
                            T_GET_INFO: begin
                                response_kind <= R_INFO;
                                response_type <= T_INFO;
                                response_data_len <= 13;
                                response_len <= 17;
                                state <= S_RESP_CRC_INIT;
                            end
                            T_CONFIG: begin
                                // 配置字段在流式解析时已写入 cfg_* 寄存器；仅接受完整
                                // 的 12 字节配置请求，避免损坏帧意外修改采集条件。
                                if (req_len == 8'd12) begin
                                    response_kind <= R_ACK;
                                    response_type <= T_ACK;
                                    response_data_len <= 0;
                                    response_len <= 4;
                                    state <= S_RESP_CRC_INIT;
                                end else begin
                                    state <= S_RX_BEGIN;
                                end
                            end
                            T_STATUS: begin
                                response_kind <= R_STATUS;
                                response_type <= T_STATUS;
                                response_data_len <= 6;
                                response_len <= 10;
                                state <= S_RESP_CRC_INIT;
                            end
                            T_READ: begin
                                if (req_len >= 4)
                                    state <= S_READ_ADDR;
                                else begin
                                    response_kind <= R_ACK;
                                    response_type <= T_ACK;
                                    response_data_len <= 0;
                                    response_len <= 4;
                                    state <= S_RESP_CRC_INIT;
                                end
                            end
                            T_ARM: begin
                                arm_pulse_r <= 1'b1;
                                response_kind <= R_ACK;
                                response_type <= T_ACK;
                                response_data_len <= 0;
                                response_len <= 4;
                                state <= S_RESP_CRC_INIT;
                            end
                            T_RESET: begin
                                reset_pulse_r <= 1'b1;
                                response_kind <= R_ACK;
                                response_type <= T_ACK;
                                response_data_len <= 0;
                                response_len <= 4;
                                state <= S_RESP_CRC_INIT;
                            end
                            default: begin
                                response_kind <= R_ACK;
                                response_type <= T_ACK;
                                response_data_len <= 0;
                                response_len <= 4;
                                state <= S_RESP_CRC_INIT;
                            end
                        endcase
                    end else begin
                        state <= S_RX_BEGIN;
                    end
                end

                S_READ_ADDR: begin
                    rd_addr_r <= read_start[AW-1:0];
                    state <= S_READ_WAIT;
                end

                S_READ_WAIT: state <= S_READ_CAPTURE;

                S_READ_CAPTURE: begin
                    read_data <= rd_data;
                    response_kind <= R_SAMPLES;
                    response_type <= T_SAMPLES;
                    response_data_len <= 8;
                    response_len <= 12;
                    state <= S_RESP_CRC_INIT;
                end

                S_RESP_CRC_INIT: begin
                    resp_crc <= 16'hFFFF;
                    resp_crc_idx <= 0;
                    state <= S_RESP_CRC;
                end

                S_RESP_CRC: begin
                    if (resp_crc_idx < response_len) begin
                        resp_crc <= crc16_update(resp_crc, response_byte(resp_crc_idx));
                        resp_crc_idx <= resp_crc_idx + 1'b1;
                    end else begin
                        resp_total <= response_len + 2;
                        tx_byte <= 0;
                        tx_after <= A_LEAD;
                        state <= S_TX_START;
                    end
                end

                S_TX_START: begin
                    if (!uart_tx_busy) begin
                        tx_start <= 1'b1;
                        state <= S_TX_WAIT_BUSY;
                    end
                end

                S_TX_WAIT_BUSY: begin
                    if (uart_tx_busy) state <= S_TX_WAIT_DONE;
                end

                S_TX_WAIT_DONE: begin
                    if (!uart_tx_busy) begin
                        case (tx_after)
                            A_LEAD: begin
                                enc_src <= 0;
                                enc_scan <= 0;
                                state <= S_TX_SCAN;
                            end
                            A_CODE: begin
                                enc_copy <= enc_src;
                                state <= S_TX_COPY;
                            end
                            A_DATA: begin
                                enc_copy <= enc_copy + 1'b1;
                                state <= S_TX_COPY;
                            end
                            default: state <= S_RX_BEGIN;
                        endcase
                    end
                end

                S_TX_SCAN: begin
                    if (enc_scan < resp_total && response_byte(enc_scan) != 0)
                        enc_scan <= enc_scan + 1'b1;
                    else begin
                        enc_zero <= enc_scan;
                        tx_byte <= (enc_scan - enc_src) + 1'b1;
                        tx_after <= A_CODE;
                        state <= S_TX_START;
                    end
                end

                S_TX_COPY: begin
                    if (enc_copy < enc_zero) begin
                        tx_byte <= response_byte(enc_copy);
                        tx_after <= A_DATA;
                        state <= S_TX_START;
                    end else if (enc_zero < resp_total) begin
                        enc_src <= enc_zero + 1'b1;
                        enc_scan <= enc_zero + 1'b1;
                        state <= S_TX_SCAN;
                    end else begin
                        tx_byte <= 0;
                        tx_after <= A_TAIL;
                        state <= S_TX_START;
                    end
                end

                default: state <= S_RX_BEGIN;
            endcase
        end
    end
endmodule
