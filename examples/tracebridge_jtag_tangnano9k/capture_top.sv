// Existing sf_micro_ila controlled over user JTAG, without UART.
// Single outstanding 64-bit command; see README.md for the wire protocol.
module capture_top(input wire clk, output wire [3:0] dbg_o);
    localparam integer DEPTH = 256;
    localparam integer AW = $clog2(DEPTH);
    reg [4:0] startup = 0;
    wire rst_n = &startup;
    always @(posedge clk) if (!rst_n) startup <= startup + 1'b1;

    reg [31:0] counter = 0;
    always @(posedge clk) if (rst_n) counter <= counter + 1'b1;

    wire [63:0] request;
    wire request_toggle;
    reg [63:0] response = 0;
    reg response_toggle = 0;
    sf_gowin_jtag_register #(.WIDTH(64), .INITIAL_VALUE(64'd0), .EXTERNAL_READBACK(1)) link (
        .value(request), .write_toggle(request_toggle),
        .read_data(response), .response_toggle(response_toggle)
    );

    (* ASYNC_REG = "TRUE" *) reg req_sync1 = 0, req_sync2 = 0;
    reg req_seen = 0;
    reg [63:0] command = 0;
    reg [2:0] state = 0;
    reg arm = 0;
    reg [31:0] trigger_mask = 0, trigger_value = 0;
    reg [7:0] decimation = 1;
    reg [AW-1:0] rd_addr = 0;
    wire [31:0] rd_data;
    wire busy, done, triggered;
    wire [AW-1:0] trigger_index;

    sf_micro_ila #(.DEPTH(DEPTH), .WIDTH(32)) ila (
        .clk(clk), .rst_n(rst_n), .probe(counter), .arm(arm),
        .trigger_mask(trigger_mask), .trigger_value(trigger_value),
        .trigger_mode(3'd0), .trigger_count(16'd1), .decimation(decimation),
        .sample_en(1'b1), .hs_valid(1'b0), .hs_ready(1'b1),
        .busy(busy), .done(done), .triggered(triggered),
        .rd_addr(rd_addr), .rd_data(rd_data), .trigger_index(trigger_index)
    );

    always @(posedge clk) begin
        req_sync1 <= request_toggle;
        req_sync2 <= req_sync1;
        arm <= 0;
        if (!rst_n) begin
            req_seen <= 0;
            state <= 0;
        end else case (state)
            0: if (req_sync2 != req_seen) begin
                // request remains held in TCK domain until response arrives.
                command <= request;
                req_seen <= req_sync2;
                state <= 1;
            end
            1: begin
                response <= {8'h5A, command[55:40], 8'd0, 32'd0};
                state <= 4;
                if (command[63:56] != 8'hA5 || command[39:32] != 0)
                    response[39:32] <= 1;
                else case (command[47:40])
                    0: response[31:0] <= 32'hDEADBEEF;
                    1: if (busy) response[39:32] <= 2; else arm <= 1;
                    2: response[31:0] <= {8'd0, trigger_index, 13'd0, triggered, done, busy};
                    3: if (!done || busy) response[39:32] <= 2;
                       else if (command[31:0] >= DEPTH) response[39:32] <= 3;
                       else begin rd_addr <= command[AW-1:0]; state <= 2; end
                    4: trigger_mask <= command[31:0];
                    5: trigger_value <= command[31:0];
                    6: decimation <= command[7:0];
                    7: response[31:0] <= {16'd32, 16'd256};
                    default: response[39:32] <= 1;
                endcase
            end
            2: state <= 3; // synchronous BSRAM read latency
            3: begin response[31:0] <= rd_data; state <= 4; end
            4: begin
                // Payload is stable before publishing its response toggle.
                response_toggle <= ~response_toggle;
                state <= 0;
            end
            default: state <= 0;
        endcase
    end
    assign dbg_o = {request_toggle, triggered, done, busy};
endmodule
