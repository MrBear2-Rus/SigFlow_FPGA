module multiclock_demo_top (
    input  logic       ctrl_clk,
    input  logic       clk_a,
    input  logic       clk_b,
    input  logic       rst_n,
    input  logic       ctrl_req_a,
    input  logic       ctrl_req_b,
    input  logic [7:0] ctrl_payload_a,
    input  logic [7:0] ctrl_payload_b,
    input  logic       arm_a,
    input  logic       arm_b,
    input  logic [2:0] rd_addr_a,
    input  logic [2:0] rd_addr_b,
    output logic       ctrl_busy_a,
    output logic       ctrl_busy_b,
    output logic       ctrl_ack_a,
    output logic       ctrl_ack_b,
    output logic       sample_pulse_a,
    output logic       sample_pulse_b,
    output logic [7:0] sample_payload_a,
    output logic [7:0] sample_payload_b,
    output logic [7:0] probe_a,
    output logic [7:0] probe_b,
    output logic       busy_a,
    output logic       busy_b,
    output logic       done_a,
    output logic       done_b,
    output logic       triggered_a,
    output logic       triggered_b,
    output logic [2:0] trigger_index_a,
    output logic [2:0] trigger_index_b,
    output logic [7:0] rd_data_a,
    output logic [7:0] rd_data_b
);
    logic [7:0] counter_a;
    logic [7:0] counter_b;
    always_ff @(posedge clk_a or negedge rst_n) begin
        if (!rst_n) counter_a <= 8'h00;
        else counter_a <= counter_a + 8'h01;
    end

    always_ff @(posedge clk_b or negedge rst_n) begin
        if (!rst_n) counter_b <= 8'h80;
        else counter_b <= counter_b + 8'h03;
    end

    assign probe_a = counter_a;
    assign probe_b = counter_b;
    sf_cdc_control #(.WIDTH(8)) u_cdc_a (
        .ctrl_clk(ctrl_clk), .ctrl_rst_n(rst_n),
        .ctrl_req(ctrl_req_a), .ctrl_payload(ctrl_payload_a),
        .ctrl_busy(ctrl_busy_a), .ctrl_ack(ctrl_ack_a),
        .sample_clk(clk_a), .sample_rst_n(rst_n),
        .sample_pulse(sample_pulse_a), .sample_payload(sample_payload_a)
    );

    sf_cdc_control #(.WIDTH(8)) u_cdc_b (
        .ctrl_clk(ctrl_clk), .ctrl_rst_n(rst_n),
        .ctrl_req(ctrl_req_b), .ctrl_payload(ctrl_payload_b),
        .ctrl_busy(ctrl_busy_b), .ctrl_ack(ctrl_ack_b),
        .sample_clk(clk_b), .sample_rst_n(rst_n),
        .sample_pulse(sample_pulse_b), .sample_payload(sample_payload_b)
    );

    tracebridge_multiclock_domain #(.DEPTH(8), .WIDTH(8)) u_capture_a (
        .clk(clk_a), .rst_n(rst_n), .probe(probe_a), .arm(arm_a),
        .rd_addr(rd_addr_a), .busy(busy_a), .done(done_a),
        .triggered(triggered_a), .trigger_index(trigger_index_a), .rd_data(rd_data_a)
    );

    tracebridge_multiclock_domain #(.DEPTH(8), .WIDTH(8)) u_capture_b (
        .clk(clk_b), .rst_n(rst_n), .probe(probe_b), .arm(arm_b),
        .rd_addr(rd_addr_b), .busy(busy_b), .done(done_b),
        .triggered(triggered_b), .trigger_index(trigger_index_b), .rd_data(rd_data_b)
    );
endmodule
