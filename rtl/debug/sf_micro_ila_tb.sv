// sf_micro_ila_tb.sv — 精简采集核自动检查 testbench（供 Verilator/CI 使用）。
// 覆盖：ARM 启动、掩码触发命中与位置冻结、写满 DEPTH 停止、
//       mask=0 立即触发、宿主端环形重排语义。
`timescale 1ns/1ps

module sf_micro_ila_tb;
    localparam int DEPTH = 16;
    localparam int WIDTH = 8;

    logic clk = 0;
    logic rst_n = 0;
    logic [WIDTH-1:0] probe = 0;
    logic arm = 0;
    logic [WIDTH-1:0] trigger_mask = 8'hFF;
    logic [WIDTH-1:0] trigger_value = 8'h0A;
    logic [2:0] trigger_mode = 3'd0;
    logic [15:0] trigger_count = 16'd1;
    logic sample_en = 1'b1;
    logic hs_valid = 1'b0;
    logic hs_ready = 1'b1;
    logic busy;
    logic done;
    logic triggered;
    logic [$clog2(DEPTH)-1:0] rd_addr = 0;
    logic [WIDTH-1:0] rd_data;
    logic [$clog2(DEPTH)-1:0] trigger_index;

    sf_micro_ila #(
        .DEPTH(DEPTH),
        .WIDTH(WIDTH)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),
        .probe(probe),
        .arm(arm),
        .trigger_mask(trigger_mask),
        .trigger_value(trigger_value),
        .trigger_mode(trigger_mode),
        .trigger_count(trigger_count),
        .sample_en(sample_en),
        .hs_valid(hs_valid),
        .hs_ready(hs_ready),
        .busy(busy),
        .done(done),
        .triggered(triggered),
        .rd_addr(rd_addr),
        .rd_data(rd_data),
        .trigger_index(trigger_index)
    );

    always #5 clk = ~clk;

    int errors = 0;
    int cycle = 0;

    task automatic check(input logic cond, input string msg);
        if (!cond) begin
            errors = errors + 1;
            $display("FAIL: %s", msg);
        end
    endtask

    task automatic arm_and_run();
        @(negedge clk);
        arm = 1;
        @(negedge clk);
        arm = 0;
    endtask

    initial begin
        $display("=== sf_micro_ila_tb start ===");
        repeat (2) @(posedge clk);
        rst_n = 1;

        // ── 场景 1：触发在第 10 个周期命中，写满 DEPTH 后停止 ──
        probe = 0;
        arm_and_run();
        check(busy, "busy after arm");
        for (cycle = 0; cycle < DEPTH + 4; cycle = cycle + 1) begin
            @(posedge clk);
            probe = probe + 1;
        end
        check(done, "done after DEPTH samples");
        check(!busy, "not busy when done");
        check(triggered, "triggered observed");
        check(trigger_index == 4'd10, "trigger_index == 10");

        // 宿主重排：相对触发时刻 k 的样本 = mem[(ti+k)%DEPTH]
        // 触发样本（rel 0）应为 10
        rd_addr = trigger_index;
        #1;
        check(rd_data == 8'd10, "reordered sample at rel 0 is the trigger sample");
        // 触发样本在写入序列中的值为 10；前后样本按序列校验：
        // 写入序列: cycle c 时 probe = c（arm 后第 c 个周期写入值 c）
        // 触发时刻 ti=10 → 地址 10 存 10
        // rel +1 → 地址 11 存 11
        rd_addr = trigger_index + 1;
        #1;
        check(rd_data == 8'd11, "reordered sample at rel +1");
        // rel -1（地址 9）存 9
        rd_addr = trigger_index - 1;
        #1;
        check(rd_data == 8'd9, "reordered sample at rel -1");

        // ── 场景 1.5：第 N 次匹配触发（N=3）──
        trigger_mask = 8'h01;
        trigger_value = 8'h01;
        trigger_mode = 2'd0;
        trigger_count = 16'd3;
        probe = 0;
        arm_and_run();
        for (cycle = 0; cycle < DEPTH + 2; cycle = cycle + 1) begin
            @(posedge clk);
            probe = probe + 1;
        end
        // probe 每周期递增：奇数 1,3,5 连续命中三次，第 3 次在 cycle 5。
        check(done && triggered, "nth-match done+triggered");
        check(trigger_index == 4'd5, "3rd match at cycle 5");
        rd_addr = trigger_index;
        #1;
        check(rd_data == 8'd5, "nth trigger sample value 5");

        // ── 场景 2：mask=0 立即触发 ──
        trigger_mode = 2'd0;
        trigger_count = 16'd1;
        trigger_mask = 0;
        trigger_value = 0;
        arm_and_run();
        @(posedge clk); // 首个采样周期触发
        check(triggered, "immediate trigger with mask=0");
        check(trigger_index == 0, "immediate trigger_index == 0");

        // ── 场景 3：复位回 IDLE ──
        rst_n = 0;
        #10;
        check(!busy && !done && !triggered, "reset clears state");
        rst_n = 1;

        if (errors == 0)
            $display("ALL PASS");
        else
            $display("%0d FAILURES", errors);
        $finish;
    end
endmodule
