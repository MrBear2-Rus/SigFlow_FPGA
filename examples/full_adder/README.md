# 全加器测试工程

在 SigFlow 的“打开工程”中选择本目录。综合顶层为 `top`，仿真 testbench 为 `tb`。工程使用三位输入 a、b、cin，输出 sum 与 cout。接口满足 `{cout,sum}=a+b+cin`。

源码位于 `src/full_adder.v`，`src/top.v` 提供工程顶层。全加器使用异或、与、或门实现。测试中的参考结果独立使用整数加法。

## 运行测试

从仓库根目录执行以下命令。

```powershell
& .\sigflowAgent\.venv\Scripts\python.exe .\examples\full_adder\tests\run_tests.py
```

Windows 测试使用仓库现有 Verilator、MinGW 和 Git for Windows 的 shell/Perl。Python 仅使用标准库。Linux 使用 PATH 中的 Verilator、make 和 C++ 编译器。

`src/tb_full_adder.v` 提供可读的 0–80 ns 穷举刺激，用于工程波形联调。8 种输入各保持 10 ns。主界面仿真只在工程根目录的 `src` 下查找 testbench，因此该文件必须保留在此目录。独立测试入口直接驱动实际 Verilator 编译的 RTL，并生成完整转换覆盖。

## 主界面仿真

打开 `examples/full_adder` 工程根目录（包含 `sigflow.project`），不要打开 `src`、`tests` 或 `src/NewProject`。综合与仿真模型顶层为 `top`，testbench 模块为 `tb`。重新打开工程后，编译仿真模型并运行仿真；波形应覆盖 000 至 111 的全部输入组合。GUI 波形与独立覆盖率测试使用各自的输出目录。

2026-11-11 修复样例目录与 GUI testbench 查找规则不一致的问题：testbench 从 `tests` 移至 `src`，工程的 `source_files` 同步更新。无需修改或重建主程序。

修复后验证：工程列出的源码路径均存在，`src/tb_*.v` 可找到唯一 testbench，Verilator 对 `tb` 顶层的语法检查通过，独立 RTL 回归 200 次校验通过、插桩点 20/20 命中。GUI 点击运行与波形显示仍需在重新打开工程后确认。

2026-11-15 修复仿真波形的时间标注和同一时刻的重复采样。仿真器按 testbench 的 `timescale` 解释时序刻度，同一刻度的输入生效后再采样输出。生成的 VCD 使用 1 ns 刻度，输入组合在 0、10、…、70 ns 切换，0–80 ns 的五个端口值均与预期一致。仿真截止时间设为 70 ns 时，仍可看到在 70 ns 生效的最后一组输入及其输出。

## 覆盖目标

| 指标 | 验收要求 |
|---|---|
| 输入组合 | 8/8 |
| 输出组合 sum/cout | 4/4 |
| 有序输入对 | 64/64，含 56 个变化对与 8 个不变对 |
| 五个端口的双向翻转 | 10/10 |
| 保持输入后的稳定性检查 | 64 次 |
| 总计结果校验 | 200 次 |

上述功能覆盖面向二值组合逻辑。X/Z 传播、门级延迟、时序收敛和 FPGA 引脚板级行为单独验收。

Verilator 的 `--coverage` 输出保存为 `build/coverage.dat`。运行器读取实际插桩计数器并生成 `build/structural_coverage.json`，其百分比是插桩点命中率。功能分箱与插桩点命中率分别报告。

测试产物均在本工程 `build` 目录。

- `functional_coverage.json` 保存功能覆盖结果。
- `structural_coverage.json` 保存插桩点覆盖结果。
- `full_adder.vcd` 保存穷举及转换波形。
- `coverage.dat` 保存模拟器原始覆盖数据。

2026-11-07 实测，200 次校验通过，功能分箱全部命中，Verilator 插桩点 20/20 命中。零输出错误实现曾在输入 001 被算术参考检查拒绝。仓库自带的 Verilator 调试构建可能输出 COVERTOGGLEDECL 哈希诊断，运行结果和退出码用于判定测试成败。

在 SigFlow 中运行综合或仿真后，选取当前修订对应的终态 Job 报告作为 Agent 证据。上述独立覆盖报告与 SigFlow Job 报告分别保留。
