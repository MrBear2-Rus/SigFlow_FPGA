module tracebridge_multiclock_domain #(
    parameter int DEPTH = 8,
    parameter int WIDTH = 8
) (
    input logic clk,
    input logic rst_n,
    input logic [WIDTH-1:0] probe,
    input logic arm,
    input logic [$clog2(DEPTH)-1:0] rd_addr,
    output logic busy,
    output logic done,
    output logic triggered,
    output logic [$clog2(DEPTH)-1:0] trigger_index,
    output logic [WIDTH-1:0] rd_data
);
    localparam int AW = $clog2(DEPTH);
    localparam logic [AW-1:0] LAST_ADDR = AW'(DEPTH - 1);
    logic [WIDTH-1:0] mem [0:DEPTH-1];
    logic [AW-1:0] write_addr;

    always_ff @(posedge clk) rd_data <= mem[rd_addr];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            busy <= 1'b0;
            done <= 1'b0;
            triggered <= 1'b0;
            trigger_index <= '0;
            write_addr <= '0;
        end else if (arm) begin
            busy <= 1'b1;
            done <= 1'b0;
            triggered <= 1'b0;
            trigger_index <= '0;
            write_addr <= '0;
        end else if (busy) begin
            mem[write_addr] <= probe;
            if (!triggered) begin
                triggered <= 1'b1;
                trigger_index <= write_addr;
            end
            if (write_addr == LAST_ADDR) begin
                busy <= 1'b0;
                done <= 1'b1;
            end else begin
                write_addr <= write_addr + 1'b1;
            end
        end
    end
endmodule
