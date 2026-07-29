module top(
    input wire a,
    input wire b,
    output wire y
);
    and_gate gate_instance(.a(a), .b(b), .y(y));
endmodule
