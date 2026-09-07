// sf_cdc_control.sv -- bundled-data toggle handshake for TraceBridge control.
//
// ctrl_payload is captured only when ctrl_req is accepted.  It remains stable
// until the sample domain acknowledges the request, so the sample domain never
// samples a changing multi-bit control bus.  The two toggle synchronizers are
// the only signals that cross clock domains.
module sf_cdc_control #(
    parameter int WIDTH = 32
) (
    input  logic             ctrl_clk,
    input  logic             ctrl_rst_n,
    input  logic             ctrl_req,
    input  logic [WIDTH-1:0] ctrl_payload,
    output logic             ctrl_busy,
    output logic             ctrl_ack,

    input  logic             sample_clk,
    input  logic             sample_rst_n,
    output logic             sample_pulse,
    output logic [WIDTH-1:0] sample_payload
);
    logic [WIDTH-1:0] payload_hold;
    logic req_toggle;
    logic ack_toggle;

    (* ASYNC_REG = "TRUE" *) logic ack_sync1, ack_sync2;
    (* ASYNC_REG = "TRUE" *) logic req_sync1, req_sync2;
    logic ack_seen;
    logic req_seen;

    assign ctrl_busy = req_toggle != ack_sync2;

    always_ff @(posedge ctrl_clk or negedge ctrl_rst_n) begin
        if (!ctrl_rst_n) begin
            payload_hold <= '0;
            req_toggle   <= 1'b0;
            ack_sync1    <= 1'b0;
            ack_sync2    <= 1'b0;
            ack_seen     <= 1'b0;
            ctrl_ack     <= 1'b0;
        end else begin
            ack_sync1 <= ack_toggle;
            ack_sync2 <= ack_sync1;
            ctrl_ack <= 1'b0;

            if (ctrl_req && !ctrl_busy) begin
                payload_hold <= ctrl_payload;
                req_toggle   <= ~req_toggle;
            end
            if (ack_sync2 != ack_seen && ack_sync2 == req_toggle) begin
                ack_seen <= ack_sync2;
                ctrl_ack <= 1'b1;
            end
        end
    end

    always_ff @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            req_sync1     <= 1'b0;
            req_sync2     <= 1'b0;
            req_seen      <= 1'b0;
            ack_toggle    <= 1'b0;
            sample_pulse  <= 1'b0;
            sample_payload <= '0;
        end else begin
            req_sync1    <= req_toggle;
            req_sync2    <= req_sync1;
            sample_pulse <= 1'b0;
            if (req_sync2 != req_seen) begin
                sample_payload <= payload_hold;
                sample_pulse   <= 1'b1;
                req_seen       <= req_sync2;
                ack_toggle     <= req_sync2;
            end
        end
    end
endmodule
