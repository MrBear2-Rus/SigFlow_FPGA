// sf_multiclock_capture.sv -- one independent capture core per clock domain.
//
// Configuration and arm signals must already be local to each domain.  Use
// sf_cdc_control for host/control-domain requests; never fan a multi-bit
// control bus directly into this module across clock domains.
module sf_multiclock_capture #(
    parameter int DOMAIN_COUNT = 2,
    parameter int DEPTH = 1024,
    parameter int WIDTH = 32
) (
    input  logic [DOMAIN_COUNT-1:0] clk,
    input  logic [DOMAIN_COUNT-1:0] rst_n,
    input  logic [DOMAIN_COUNT*WIDTH-1:0] probe,
    input  logic [DOMAIN_COUNT-1:0] arm,
    input  logic [DOMAIN_COUNT*WIDTH-1:0] trigger_mask,
    input  logic [DOMAIN_COUNT*WIDTH-1:0] trigger_value,
    input  logic [DOMAIN_COUNT*3-1:0] trigger_mode,
    input  logic [DOMAIN_COUNT*16-1:0] trigger_count,
    input  logic [DOMAIN_COUNT*8-1:0] decimation,
    input  logic [DOMAIN_COUNT-1:0] sample_en,
    input  logic [DOMAIN_COUNT-1:0] hs_valid,
    input  logic [DOMAIN_COUNT-1:0] hs_ready,
    output logic [DOMAIN_COUNT-1:0] busy,
    output logic [DOMAIN_COUNT-1:0] done,
    output logic [DOMAIN_COUNT-1:0] triggered,
    input  logic [DOMAIN_COUNT*$clog2(DEPTH)-1:0] rd_addr,
    output logic [DOMAIN_COUNT*WIDTH-1:0] rd_data,
    output logic [DOMAIN_COUNT*$clog2(DEPTH)-1:0] trigger_index
);
    localparam int AW = $clog2(DEPTH);

    genvar domain;
    generate
        for (domain = 0; domain < DOMAIN_COUNT; domain = domain + 1) begin : g_capture
            sf_micro_ila #(.DEPTH(DEPTH), .WIDTH(WIDTH)) u_ila (
                .clk(clk[domain]),
                .rst_n(rst_n[domain]),
                .probe(probe[domain*WIDTH +: WIDTH]),
                .arm(arm[domain]),
                .trigger_mask(trigger_mask[domain*WIDTH +: WIDTH]),
                .trigger_value(trigger_value[domain*WIDTH +: WIDTH]),
                .trigger_mode(trigger_mode[domain*3 +: 3]),
                .trigger_count(trigger_count[domain*16 +: 16]),
                .decimation(decimation[domain*8 +: 8]),
                .sample_en(sample_en[domain]),
                .hs_valid(hs_valid[domain]),
                .hs_ready(hs_ready[domain]),
                .busy(busy[domain]),
                .done(done[domain]),
                .triggered(triggered[domain]),
                .rd_addr(rd_addr[domain*AW +: AW]),
                .rd_data(rd_data[domain*WIDTH +: WIDTH]),
                .trigger_index(trigger_index[domain*AW +: AW])
            );
        end
    endgenerate
endmodule
