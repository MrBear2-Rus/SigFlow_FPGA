// Course 05 fixture: open-drain I2C temperature monitor with an alarm LED.
// The sensor sits at 0x48; register 0x00 holds the temperature byte. The alarm
// lights when the reading exceeds 25 degrees. SDA is split into an open-drain
// output (0 = pull low, 1 = release to the board pull-up) plus a sense input,
// so the ACK slot really travels through the 13-state bus.
module i2c_alarm_monitor(
    input  wire        clock_in,
    input  wire        reset_n,
    output reg         i2c_scl,
    output reg         i2c_sda,
    input  wire        i2c_sda_in,
    output reg  [4:0]  led_n
);
    localparam int HALF_CYCLES = 8;
    localparam [7:0] ADDR_READ = 8'h91;    // 7'h48 << 1 | read
    localparam [31:0] MAX_RETRIES = 1;     // a single unacknowledged frame parks the bus
    localparam [7:0]  ALARM_THRESHOLD = 8'd25;

    reg [31:0] cyc = 0;
    reg [7:0]  step = 8'd0;
    reg        done = 0, error_state = 0;
    reg [31:0] retries = 0;
    reg [7:0]  value = 0;

    function automatic [1:0] bus(input [7:0] s);   // {scl, sda}
        begin
            bus = 2'b11;
            if (s == 8'd0)       bus = 2'b11;                        // idle
            else if (s == 8'd1)  bus = 2'b10;                        // START
            else if (s >= 8'd2 && s <= 8'd17)
                bus = {s[0], ADDR_READ[7 - ((s - 8'd2) >> 1)]};       // address + R/W
            else if (s == 8'd18) bus = 2'b01;                        // ACK slot low
            else if (s == 8'd19) bus = 2'b11;                        // ACK sample
            else if (s >= 8'd20 && s <= 8'd35) bus = {s[0], 1'b1};   // read byte
            else if (s == 8'd36) bus = 2'b01;                        // NAK low
            else if (s == 8'd37) bus = 2'b11;                        // NAK high
            else if (s == 8'd38) bus = 2'b00;                        // STOP prep
            else if (s == 8'd39) bus = 2'b10;
            else if (s == 8'd40) bus = 2'b11;                        // STOP
        end
    endfunction

    always @(*) begin
        {i2c_scl, i2c_sda} = bus(step);
    end

    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) begin
            cyc <= 0; step <= 8'd0; done <= 0; error_state <= 0; retries <= 0;
            value <= 0; led_n <= 5'b11111;
        end else if (!done && !error_state) begin
            if (cyc != HALF_CYCLES-1) cyc <= cyc + 1;
            else begin
                cyc <= 0;
                if (step == 8'd19) begin
                    if (i2c_sda_in) begin
                        if (retries < MAX_RETRIES-1) begin
                            retries <= retries + 1; step <= 8'd0;
                        end else begin
                            error_state <= 1; step <= 8'd41;
                            led_n <= 5'b11111;   // bus parked: no alarm, no evidence
                        end
                    end else step <= 8'd20;
                end else if (step >= 8'd20 && step <= 8'd35 && step[0]) begin
                    value <= {value[6:0], i2c_sda_in}; step <= step + 8'd1;
                end else if (step == 8'd40) begin
                    done <= 1; step <= 8'd41;
                    // bit0: alarm (active low), bits 4:1: inverted low nibble.
                    led_n <= {~value[3:0], (value > ALARM_THRESHOLD) ? 1'b0 : 1'b1};
                end else step <= step + 8'd1;
            end
        end
    end
endmodule
