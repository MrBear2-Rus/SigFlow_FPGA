// Tang Nano 9K / GW1NR-9C user-JTAG register, built with Gowin EDA.
// ER1=0x42: WIDTH-bit exchange (returns old value, commits TDI at Update-DR).
// ER2=0x43: read-only snapshot. All transfers are LSB first.
//
// The value output belongs to the JTAG TCK domain. Cross it into a system
// clock with a bundled-data handshake before using it as a control register.
// TAP reset deliberately preserves value; FPGA configuration initializes it.
// Do not use test_logic_reset_o as an active-high asynchronous reset.
module sf_gowin_jtag_register #(
    parameter integer WIDTH = 32,
    parameter [WIDTH-1:0] INITIAL_VALUE = 32'hDEADBEEF,
    parameter EXTERNAL_READBACK = 1'b0
) (
    output wire [WIDTH-1:0] value,
    output wire write_toggle,
    input wire [WIDTH-1:0] read_data = {WIDTH{1'b0}},
    input wire response_toggle = 1'b0
);
    wire tck, tdi, idle_er1, idle_er2, shift_capture, update_dr, sel1, sel2;
    reg [WIDTH-1:0] dr = INITIAL_VALUE;
    reg [WIDTH-1:0] value_hold = INITIAL_VALUE;
    reg request_toggle = 1'b0;
    reg [WIDTH-1:0] receive_dr = {WIDTH{1'b0}};
    reg [WIDTH-1:0] response_hold = {WIDTH{1'b0}};
    (* ASYNC_REG = "TRUE" *) reg response_sync1 = 1'b0, response_sync2 = 1'b0;
    reg response_seen = 1'b0;
    assign value = value_hold;
    assign write_toggle = request_toggle;

    always @(posedge tck) begin
        // read_data must remain stable from response_toggle until the next
        // request. The toggle crosses two flops before its bundled payload
        // is captured, so ER2 never samples a changing system-clock bus.
        response_sync1 <= response_toggle;
        response_sync2 <= response_sync1;
        if (response_sync2 != response_seen) begin
            response_seen <= response_sync2;
            response_hold <= read_data;
        end

        if (idle_er1)
            dr <= value_hold;
        else if (idle_er2)
            dr <= EXTERNAL_READBACK ? response_hold : value_hold;
        else if (shift_capture && (sel1 || sel2))
            dr <= {tdi, dr[WIDTH-1:1]};

        if (shift_capture && sel1)
            receive_dr <= {tdi, receive_dr[WIDTH-1:1]};

        // Update-DR is a state qualifier, not a separate fabric clock.
        if (update_dr && sel1) begin
            value_hold <= receive_dr;
            request_toggle <= ~request_toggle;
        end
    end

    GW_JTAG user_jtag (
        // Gowin connects the dedicated pads implicitly. Tie unused logical
        // pad inputs to the same zero chosen by synthesis for open inputs.
        .tck_pad_i(1'b0), .tms_pad_i(1'b0), .tdi_pad_i(1'b0), .tdo_pad_o(),
        .tck_o(tck), .tdi_o(tdi), .test_logic_reset_o(),
        .run_test_idle_er1_o(idle_er1), .run_test_idle_er2_o(idle_er2),
        .shift_dr_capture_dr_o(shift_capture), .update_dr_o(update_dr),
        .pause_dr_o(), .enable_er1_o(sel1), .enable_er2_o(sel2),
        .tdo_er1_i(dr[0]), .tdo_er2_i(dr[0])
    );
endmodule
