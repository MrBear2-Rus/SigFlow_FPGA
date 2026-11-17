module p1_debounce(input wire clock_in, reset_n, key_n, output wire [5:0] led_n);
    reg [2:0] raw_count, clean_count;
    reg stable_level;
    reg [2:0] stable_ticks;
    always @(negedge key_n or negedge reset_n)
        if (!reset_n) raw_count <= 0;
        else raw_count <= raw_count + 1'b1;
    always @(posedge clock_in or negedge reset_n)
        if (!reset_n) begin
            clean_count <= 0; stable_level <= 1; stable_ticks <= 0;
        end else if (key_n == stable_level) stable_ticks <= 0;
        else if (stable_ticks == 2) begin
            stable_level <= key_n; stable_ticks <= 0;
            if (!key_n) clean_count <= clean_count + 1'b1;
        end else stable_ticks <= stable_ticks + 1'b1;
    assign led_n = ~{clean_count, raw_count};
endmodule
