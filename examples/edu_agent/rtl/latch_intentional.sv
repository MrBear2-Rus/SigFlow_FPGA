// 合法锁存器反例（必须不产生任何 finding）
//
// always_latch 明确表达了“这里就是要一个电平锁存器”的设计意图，
// 属于有意为之，不能当作“不小心推断出来的锁存器”报给学生。
// 等价写法：实例化工艺库里的锁存器原语，或用 always @(posedge clk) 的寄存器块。
module latch_intentional (
    input  logic       en,
    input  logic [3:0] d,
    output logic [3:0] q
);
    always_latch begin
        if (en) q = d;
    end
endmodule
