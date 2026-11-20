// Phase 3 teaching fixture: passive keypad prober. Row lines are manual
// scan stimulus (open-drain style: released lines float to the board
// pull-up), so the classic ghost-key path stays observable without any
// fighting scan drivers. LEDs mirror two row lines and four columns.
module keypad_probe(
    input  wire        clock_in,
    input  wire        reset_n,
    input  wire [3:0]  row_sense,
    input  wire [3:0]  col_sense,
    output reg  [5:0]  led_n
);
    initial led_n = 6'b111111;
    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) led_n <= 6'b111111;
        else led_n <= {row_sense[1:0], col_sense[3:0]};
    end
endmodule
