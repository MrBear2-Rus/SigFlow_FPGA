// Phase 5 teaching fixture: open-drain I2C master executing a write-pointer
// transaction (0x00) followed by a 3-byte burst read (Master ACK -> Master ACK
// -> Master NAK -> STOP) against the board sensor (0x48).
//
// SDA is split into an open-drain output (0 = pull SLO, 1 = release to FLT/WHI)
// and a sense input sharing the junction. Bus outputs depend strictly on step so
// SCL and SDA change cleanly across half-period boundaries.
module i2c_burst_master(
    input  wire        clock_in,
    input  wire        reset_n,
    output reg         i2c_scl,
    output reg         i2c_sda,
    input  wire        i2c_sda_in,
    output reg  [4:0]  led_n
);
    localparam int HALF_CYCLES = 8;
    localparam [7:0] ADDR_WRITE = 8'h90; // 7'h48 << 1 | write
    localparam [7:0] ADDR_READ  = 8'h91; // 7'h48 << 1 | read
    localparam [7:0] REG_PTR    = 8'h00;
    localparam [31:0] MAX_RETRIES = 4;

    reg [31:0] cyc = 0;
    reg [7:0]  step = 8'd0;
    reg        done = 0, error_state = 0;
    reg [31:0] retries = 0;
    reg [7:0]  byte0 = 0, byte1 = 0, byte2 = 0;

    always @(*) begin
        {i2c_scl, i2c_sda} = 2'b11;
        case (step)
            8'd1, 8'd43:                  {i2c_scl, i2c_sda} = 2'b10; // START
            8'd18, 8'd36, 8'd60, 8'd114:  {i2c_scl, i2c_sda} = 2'b01; // ACK / MNAK low
            8'd19, 8'd37, 8'd61, 8'd115:  {i2c_scl, i2c_sda} = 2'b11; // ACK / MNAK high
            8'd78, 8'd96:                 {i2c_scl, i2c_sda} = 2'b00; // Master ACK low
            8'd79, 8'd97:                 {i2c_scl, i2c_sda} = 2'b10; // Master ACK high
            8'd38, 8'd116:                {i2c_scl, i2c_sda} = 2'b01; // STOP prep SCL low
            8'd39, 8'd117:                {i2c_scl, i2c_sda} = 2'b00; // STOP prep SDA low
            8'd40, 8'd118:                {i2c_scl, i2c_sda} = 2'b10; // STOP hold SCL high
            default:                      {i2c_scl, i2c_sda} = 2'b11; // idle / STOP
        endcase
        if (step >= 8'd2 && step <= 8'd17)
            {i2c_scl, i2c_sda} = {step[0], ADDR_WRITE[7 - ((step - 8'd2) >> 1)]};
        if (step >= 8'd20 && step <= 8'd35)
            {i2c_scl, i2c_sda} = {step[0], REG_PTR[7 - ((step - 8'd20) >> 1)]};
        if (step >= 8'd44 && step <= 8'd59)
            {i2c_scl, i2c_sda} = {step[0], ADDR_READ[7 - ((step - 8'd44) >> 1)]};
        if ((step >= 8'd62 && step <= 8'd77) || (step >= 8'd80 && step <= 8'd95) || (step >= 8'd98 && step <= 8'd113))
            {i2c_scl, i2c_sda} = {step[0], 1'b1};
    end

    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) begin
            cyc <= 0; step <= 8'd0; done <= 0; error_state <= 0; retries <= 0;
            byte0 <= 0; byte1 <= 0; byte2 <= 0; led_n <= 5'b11111;
        end else if (!done && !error_state) begin
            if (cyc != HALF_CYCLES - 1) cyc <= cyc + 1;
            else begin
                cyc <= 0;
                if (step == 8'd19 || step == 8'd37 || step == 8'd61) begin
                    if (i2c_sda_in) begin
                        if (retries < MAX_RETRIES - 1) begin
                            retries <= retries + 1;
                            step <= 8'd0;
                        end else begin
                            error_state <= 1;
                            step <= 8'd126;
                            led_n <= 5'b10101;
                        end
                    end else begin
                        step <= step + 8'd1;
                    end
                end else if (step >= 8'd62 && step <= 8'd77 && step[0]) begin
                    byte0 <= {byte0[6:0], i2c_sda_in};
                    step <= step + 8'd1;
                end else if (step >= 8'd80 && step <= 8'd95 && step[0]) begin
                    byte1 <= {byte1[6:0], i2c_sda_in};
                    step <= step + 8'd1;
                end else if (step >= 8'd98 && step <= 8'd113 && step[0]) begin
                    byte2 <= {byte2[6:0], i2c_sda_in};
                    step <= step + 8'd1;
                end else if (step == 8'd119) begin
                    done <= 1;
                    step <= 8'd126;
                    led_n <= ~(byte0[4:0] ^ byte1[4:0] ^ byte2[4:0]);
                end else begin
                    step <= step + 8'd1;
                end
            end
        end
    end
endmodule
