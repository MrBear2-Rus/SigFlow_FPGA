module replay_demo_top (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] data,
    output logic [7:0] state
);
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) state <= 8'h00;
        else state <= state + data;
    end
endmodule
