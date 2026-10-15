# examples/edu_agent — 规则引擎可复现样例（U1 / T7）

这里的每个文件都是一段**最小可复现 RTL**，配一条期望结论，用来证明
`agent/src/sigflow_edu_agent/rules.py` 的四条确定性规则
「该报的报、不该报的不报」。样例同时被两处使用，只有一处真源：

- 单元测试：[`agent/tests/test_rules.py`](../../agent/tests/test_rules.py)
- 手工核验脚本：[`check_rules.py`](check_rules.py)

期望值记录在 [`expected_findings.json`](expected_findings.json)：
键是样例路径，`rules` 是 `analyse()` 必须返回的规则序列（空数组 = 一条都不能报），
`signals` 是与之一一对应的信号名。

## 1. 样例一览

| 文件 | 期望 | 教学含义 |
|---|---|---|
| `rtl/latch_incomplete.v` | `RTL_LATCH_INCOMPLETE_BRANCH(q)` | **锁存器正例**：组合块里只在 `en` 成立时给 `q` 赋值 → 推断锁存器 |
| `rtl/counter_reset_latch.v` | `RTL_LATCH_INCOMPLETE_BRANCH(count_next)` | 计数器的常见写法：`if (rst) … else if (en) …` 少了兜底分支 |
| `rtl/latch_intentional.sv` | 无 | **合法锁存器反例**：`always_latch` 明确表达设计意图，不能报成“推断出来的锁存器” |
| `rtl/latch_complete_else.v` | 无 | 锁存器反例：`if/else` 两条路径都赋值 |
| `rtl/sensitivity_incomplete.v` | `RTL_SENSITIVITY_INCOMPLETE(valid)` | 块内读了 `valid`，敏感列表只写了 `ready` |
| `rtl/sensitivity_complete.v` | 无 | 敏感列表反例：写全 / `@(*)` / 时钟块三种写法都不该报 |
| `rtl/width_mismatch.v` | `RTL_WIDTH_MISMATCH(y)` | 4 位信号被赋 8 位常量，高位被静默截断 |
| `rtl/width_match.v` | 无 | 位宽反例：常量位宽与目标一致 |
| `rtl/block_unbalanced.v` | `RTL_BLOCK_UNBALANCED` | 少了 `end`，`begin`/`end` 数量对不上 |
| `rtl/block_balanced.v` | 无 | 结构反例：`end else begin` 同行写法也要判成配对正确 |

「锁存器正例 + 合法锁存器反例」这一对（对应 D-01 的两份基础样例）是
`latch_incomplete.v` 与 `latch_intentional.sv`：两段代码的 `if (en) q = d;`
形状相同，唯一区别是后者用 `always_latch` 声明了“这里就是要锁存器”，
因此一个必须报、一个必须不报。

## 2. 自动核验

```sh
# 样例集（期望值来自 expected_findings.json）
python examples/edu_agent/check_rules.py

# 规则引擎单元测试（含正反例与样例回放）
python -m unittest discover -s agent/tests -t agent -p "test_rules*.py"
```

`check_rules.py` 的退出码：`0` 全部符合期望，`1` 有样例与期望不符，`2` 文件不存在。

## 3. 手工核验一个片段

想把任意一段 RTL（学生作业、真实工程文件）单独喂给规则引擎：

```sh
python examples/edu_agent/check_rules.py path/to/your_file.v
```

输出每条命中的规则、信号、行号与标题。例如对锁存器正例：

```text
examples/edu_agent/rtl/latch_incomplete.v: 1 finding(s) -> RTL_LATCH_INCOMPLETE_BRANCH(q)
  - RTL_LATCH_INCOMPLETE_BRANCH signal=q line_hint=10
    组合逻辑推断出锁存器：`q` 没有被所有分支赋值
```

脚本是纯只读的：只读取你给的文件，不写文件、不联网、不调用模型，
和 sidecar 运行期走的是同一个 `analyse()`。

## 4. 规则引擎的口径（与实现一一对应）

1. `RTL_LATCH_INCOMPLETE_BRANCH`：只检查**组合块**（`always @(*)`、`always_comb`、
   以及电平敏感的 `always @(a or b)`）。`always_ff`、`always @(posedge ...)` 是时序块，
   `always_latch` 是有意锁存器，都不参与这条规则。
   判定口径：信号在“部分控制路径”上被赋值（缺 `else`、缺 `default`、内层 `if` 不完整），
   而不是所有路径都赋值。
2. `RTL_SENSITIVITY_INCOMPLETE`：只检查**电平敏感**且写了显式列表的块；
   `@(*)`、`always_comb`、时钟块不适用。单字符信号名（`x`/`z`/`b`/`d`/`h`
   也是进制字符）证据不足，故意跳过。
3. `RTL_WIDTH_MISMATCH`：只比较**赋值右侧就是单个带位宽常量**（`y = 8'hff;`、
   `y <= 8'hff;`、`assign y = 8'hff;`）与目标信号位宽。`y = 255;` 这类不定宽十进制、
   以及 `state <= (byte == 8'd1) ? A : B;` 这种“常量只是比较操作数”的写法都不判。
   声明范围含参数（如 `[WIDTH-1:0]`）时按“未知”处理，跳过而不是猜。
4. `RTL_BLOCK_UNBALANCED`：统计**去掉注释后**的 `begin`/`end`
   （`endcase`/`endmodule`/`endfunction`/`endtask`/`endgenerate` 不算），数量不等即命中。

`analyse()` 按「锁存器 → 敏感列表 → 位宽 → 结构」的顺序返回，每条规则最多一条，
第一条命中会成为 UI 上的主卡片。函数是纯函数：无模型、无 IO、同一输入永远同一输出。

## 5. 已知限制

规则引擎是可解释的近似检查，不是完整的 HDL 前端（也不做综合）。
以下情况**不会**被判出来，属于已知口径而非缺陷：

- 跨模块 / 跨文件的信号关系、`generate`、`function`/`task` 内部的覆盖分析；
- 不带位宽的常量、表达式综合出的位宽、有符号 / 零扩展细节；
- `for`/`while` 循环体内的赋值覆盖；
- 注释掉的代码不影响结论（有意如此），但字符串字面量里的 `begin`/`end`
  仍会被计入结构计数。

需要更严格的结论时，交给真实综合/仿真工具的诊断，规则卡只负责“不靠模型也能解释的第一步”。
