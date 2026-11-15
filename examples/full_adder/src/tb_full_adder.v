`ifndef SYNTHESIS
`timescale 1ns/1ps
// Keep in src/: the native GUI discovers testbenches in this directory.
// Explicit timeline for SigFlow's controlled stimulus importer.
module tb;
    reg a;
    reg b;
    reg cin;
    wire sum;
    wire cout;
    top dut(.a(a), .b(b), .cin(cin), .sum(sum), .cout(cout));
    initial begin
        a = 0;
        b = 0;
        cin = 0;
        #10;
        cin = 1;
        #10;
        b = 1;
        cin = 0;
        #10;
        cin = 1;
        #10;
        a = 1;
        b = 0;
        cin = 0;
        #10;
        cin = 1;
        #10;
        b = 1;
        cin = 0;
        #10;
        cin = 1;
        #10;
        $finish;
    end
endmodule
`endif
