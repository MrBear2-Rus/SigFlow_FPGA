`timescale 1ns/1ps
module top(
    input wire a, b, cin,
    output wire sum, cout
);
    full_adder dut(.a(a), .b(b), .cin(cin), .sum(sum), .cout(cout));
endmodule
