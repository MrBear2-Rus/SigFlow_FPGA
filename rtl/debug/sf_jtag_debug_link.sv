// sf_jtag_debug_link.sv — TraceBridge JTAG 调试链路（jtag-1）。
//
// 该模块把一个 64-bit TAP 交换字接到现有 sf_micro_ila。请求/响应通过
// sf_gowin_jtag_register 在 Gowin JTAG TCK 域与采样系统时钟域之间做
// bundled-data + toggle CDC。一次只处理一个请求；主机读 ER2 直到响应
// 序号/opcode 匹配。该核不执行配置、擦除或 Flash 操作。
module sf_jtag_debug_link #(
    parameter int WIDTH = 32,
    parameter int DEPTH = 1024,
    parameter logic [31:0] IDENTITY = 32'hDEADBEEF
) (
    input  logic clk,
    input  logic rst_n,

    output logic arm_pulse,
    output logic reset_pulse,
    output logic [31:0] trigger_mask,
    output logic [31:0] trigger_value,
    output logic [7:0] decimation,

    input  logic busy,
    input  logic done,
    input  logic triggered,
    input  logic [$clog2(DEPTH)-1:0] trigger_index,
    output logic [$clog2(DEPTH)-1:0] rd_addr,
    input  logic [WIDTH-1:0] rd_data
);
    localparam int AW = $clog2(DEPTH);

    wire [63:0] request;
    wire request_toggle;
    logic [63:0] response = 64'd0;
    logic response_toggle = 1'b0;
    logic [63:0] command_reg = 64'd0;
    logic request_seen = 1'b0;
    logic read_pending = 1'b0;
    (* ASYNC_REG = "TRUE" *) logic request_sync1 = 1'b0, request_sync2 = 1'b0;

    sf_gowin_jtag_register #(
        .WIDTH(64),
        .INITIAL_VALUE(64'd0),
        .EXTERNAL_READBACK(1'b1)
    ) user_jtag_register (
        .value(request),
        .write_toggle(request_toggle),
        .read_data(response),
        .response_toggle(response_toggle)
    );

    logic [2:0] state = 3'd0;
    localparam logic [2:0] ST_IDLE = 3'd0;
    localparam logic [2:0] ST_EXEC = 3'd1;
    localparam logic [2:0] ST_READ_WAIT = 3'd2;
    localparam logic [2:0] ST_RESP = 3'd3;

    logic [7:0] response_status = 8'd0;
    logic [31:0] response_data = 32'd0;
    logic [31:0] status_word;
    logic [31:0] geometry_word;

    assign status_word = {trigger_index, 13'd0, triggered, done, busy};
    assign geometry_word = (WIDTH << 16) | DEPTH;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            request_sync1 <= 1'b0;
            request_sync2 <= 1'b0;
            request_seen  <= 1'b0;
            read_pending  <= 1'b0;
            response      <= 64'd0;
            response_toggle <= 1'b0;
            command_reg   <= 64'd0;
            state         <= ST_IDLE;
            response_status <= 8'd0;
            response_data <= 32'd0;
            arm_pulse     <= 1'b0;
            reset_pulse   <= 1'b0;
            trigger_mask  <= 32'hFFFFFFFF;
            trigger_value <= 32'd0;
            decimation    <= 8'd1;
            rd_addr       <= '0;
        end else begin
            request_sync1 <= request_toggle;
            request_sync2 <= request_sync1;
            arm_pulse     <= 1'b0;
            reset_pulse   <= 1'b0;

            case (state)
                ST_IDLE: begin
                    if (request_sync2 != request_seen) begin
                        command_reg  <= request;
                        request_seen <= request_sync2;
                        state        <= ST_EXEC;
                    end
                end

                ST_EXEC: begin
                    response_status <= 8'd0;
                    response_data   <= 32'd0;
                    if (command_reg[63:56] != 8'hA5 || command_reg[39:32] != 8'h00) begin
                        response_status <= 8'd1; // invalid command/magic
                        state <= ST_RESP;
                    end else begin
                        case (command_reg[47:40])
                            8'd0: begin // Identity
                                response_data <= IDENTITY;
                                state <= ST_RESP;
                            end
                            8'd1: begin // Arm
                                if (busy) response_status <= 8'd2;
                                else arm_pulse <= 1'b1;
                                state <= ST_RESP;
                            end
                            8'd2: begin // Status
                                response_data <= status_word;
                                state <= ST_RESP;
                            end
                            8'd3: begin // Read capture
                                if (!done || busy) begin
                                    response_status <= 8'd2;
                                    state <= ST_RESP;
                                end else if (command_reg[31:0] >= DEPTH) begin
                                    response_status <= 8'd3;
                                    state <= ST_RESP;
                                end else begin
                                    rd_addr <= command_reg[AW-1:0];
                                    state <= ST_READ_WAIT;
                                end
                            end
                            8'd4: begin // Trigger mask
                                if (busy) response_status <= 8'd2;
                                else trigger_mask <= command_reg[31:0];
                                state <= ST_RESP;
                            end
                            8'd5: begin // Trigger value
                                if (busy) response_status <= 8'd2;
                                else trigger_value <= command_reg[31:0];
                                state <= ST_RESP;
                            end
                            8'd6: begin // Decimation
                                if (busy || command_reg[31:0] == 0 || command_reg[31:0] > 255)
                                    response_status <= busy ? 8'd2 : 8'd1;
                                else decimation <= command_reg[7:0];
                                state <= ST_RESP;
                            end
                            8'd7: begin // GetInfo
                                response_data <= geometry_word;
                                state <= ST_RESP;
                            end
                            default: begin
                                response_status <= 8'd1;
                                state <= ST_RESP;
                            end
                        endcase
                    end
                end

                ST_READ_WAIT: begin
                    // sf_micro_ila exposes a synchronous read port.  Spend
                    // one full cycle after changing rd_addr before sampling
                    // rd_data; otherwise address zero returns stale BSRAM data.
                    if (!read_pending) begin
                        read_pending <= 1'b1;
                    end else begin
                        read_pending  <= 1'b0;
                        response_status <= 8'd0;
                        response_data   <= rd_data;
                        state           <= ST_RESP;
                    end
                end

                ST_RESP: begin
                    response <= {8'h5A, command_reg[55:40], response_status,
                                 response_data};
                    response_toggle <= ~response_toggle;
                    state <= ST_IDLE;
                end

                default: state <= ST_IDLE;
            endcase
        end
    end
endmodule
