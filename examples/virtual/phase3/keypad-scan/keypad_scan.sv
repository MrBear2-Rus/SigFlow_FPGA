// Phase 3 teaching fixture: one-hot active-low row scanner for a 4x4 matrix
// keypad. Rows are push-pull DUT outputs; columns are pulled-up inputs.
// Each scan slot lasts 27 clock cycles, i.e. exactly 1us at 27MHz; the slot
// divider is a teaching fixture parameter and does not change the scan
// algebra. Registers carry declaration initializers: a freshly loaded FPGA
// image starts in the reset scan state even before any asynchronous reset edge.
module keypad_scan(
    input  wire        clock_in,
    input  wire        reset_n,
    output reg  [3:0]  row_drive,
    input  wire [3:0]  col_sense,
    output reg  [5:0]  led_n
);
    reg [11:0] divider = 12'd0;
    initial begin
        row_drive = 4'b1110;
        led_n = 6'b111111;
    end
    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) begin
            divider <= 12'd0;
            row_drive <= 4'b1110;
            led_n <= 6'b111111;
        end else if (divider == 12'd26) begin
            divider <= 12'd0;
            led_n <= {2'b11, col_sense};
            row_drive <= {row_drive[2:0], row_drive[3]};
        end else begin
            divider <= divider + 12'd1;
        end
    end
endmodule

