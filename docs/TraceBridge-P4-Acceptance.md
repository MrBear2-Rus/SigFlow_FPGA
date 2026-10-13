# TraceBridge P4 验收方案

本方案针对三项最高优先级工作：Capture-to-Scenario、首因候选图、首差异到源码/画布映射。板上回环到 WavePanel 的验收已完成，不再作为本方案的阻塞项。

## 0. 当前交付物

- 采集会话创建时自动生成 `signal-map.json` 和 `dependency-graph.json`。
- TraceBridge 左侧新增“生成重放 VCD”，从已采集且被选中的顶层 `input` 探针生成 `artifacts/replay_tb.cpp`，调用 Verilator 输出 `artifacts/replay.vcd`。
- 比较结果统一保存为 snake_case 字段；重放比较额外保存 `replay-consistency.json`，并在 `compare.json` 中标记 `reference_kind`。
- 双轨波形的首差异事件沿用主窗口导航回调，可跳转 RTL 行、SFTree 节点和画布元素。

## 1. 软件回归

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_debug_ci.ps1
```

通过条件：最后输出 `DEBUG CI: ALL PASS`；新增 `tracebridge_priority_smoke` 必须包含以下结果：

- VCD 输入变化提取成功，时钟半周期推断正确。
- replay 输入覆盖率为 100%，一致性报告 `verified=true`。
- 生成的 testbench 同时驱动 DUT 输入和时钟。
- 直接上游信号排在更远上游信号之前。
- 候选保留 RTL 文件、行号、SFTree 路径和画布 ID。
- 首差异结果能挂载源码位置和上游候选。

## 2. Capture-to-Scenario

### GUI 傻瓜式路径

1. 在 Debug Contract 的探针中加入 `top.clk`、复位和至少一个顶层业务 `input`，重新构建并下载调试位流。
2. 采集成功后点击“生成重放 VCD”；程序自动生成映射、提取输入、生成 `replay_tb.cpp` 并运行 Verilator。
3. 成功后点击“比对”或直接查看自动比较结果；参考基准应显示为 `hardware-input-replay`。

若按钮提示“没有被采样的顶层 input 探针”，说明旧位流没有采集输入，必须重新选择探针、构建、下载和采集；不能用只含 `state/led` 的旧 capture 推导输入激励。

### Verilator 环境

确认以下命令可用：

```powershell
verilator --version
```

GUI 会在会话目录执行 `verilator --cc --exe --build --trace`，不需要手工复制生成的 testbench。

准备一份包含采样时钟和至少一个顶层输入探针的 `capture.vcd`，然后调用：

```cpp
ReplayStimulusExtractor::Extract(vcd, {"input_a", "input_b"}, "clk", scenario, error);
EvaluateReplayConsistency(scenario, 2);
ReplayTestbenchGenerator::Generate(
    scenario, "demo_top", "replay_tb.cpp", "replay.vcd", error);
```

通过条件：

- 输入探针全部存在时，`capturedInputs == requestedInputs`、`verified == true`。
- 生成的 `replay_tb.cpp` 包含 `Vdemo_top.h`、`top->clk` 和每个输入端口赋值。
- 使用安装好的 Verilator 编译并运行生成的 testbench，成功产出 `replay.vcd`。
- 修改一个输入探针名称后，报告变为未验证，不得输出确定性首因结论。

仓库已随附 Verilator，标准位置为 `tools/verilator/verilator-install`；脚本默认使用
`bin/verilator_bin_dbg.exe`，也可通过 `-Verilator` 指定其他安装。当前验收已通过单时钟与多时钟
真实 Verilator 编译、运行和 VCD 生成。

## 3. 首因候选图

在会话目录放置 `dependency-graph.json` 和 `signal-map.json`。最小图形如下：

```json
{
  "nodes": [{"signal":"state"},{"signal":"fifo_empty"},{"signal":"rd_en"}],
  "dependencies": [
    {"source":"fifo_empty","dependent":"state"},
    {"source":"rd_en","dependent":"fifo_empty"}
  ]
}
```

通过条件：

- 对 `state` 查询，上游顺序为 `fifo_empty`、`rd_en`。
- 限制深度为 1 时只返回 `fifo_empty`。
- 候选排序稳定，直接上游优先；没有映射时保留信号名但明确位置为空。

## 4. 首差异定位

在同一会话目录提供位置映射：

```json
{
  "signals": [{
    "signal":"state",
    "source_path":"E:/demo/rtl/demo.sv",
    "source_line":42,
    "sftree_path":"TOP.state",
    "canvas_id":"state",
    "clock_domain":"clk",
    "fanout":1
  }]
}
```

在 TraceBridge 中加载 HW/Sim VCD 并执行比较。通过条件：

- 结果页显示首差异信号、期望值、实测值、RTL 路径和行号。
- 结果页显示上游候选链。
- 波形首差异事件携带同一源码路径和行号。
- 双击首差异后，SFTree 选中对应节点，画布尝试选中对应元素，编辑器跳到对应行。
- 映射文件不存在或行号为 0 时，界面明确显示位置不可用，不伪造定位结果。

## 5. Tang Nano 9K 现场验收

现场使用已经验证的 `COM7`、`921600` 和 17/18 UART 位流完成一次采集。确认：

1. 板上回环采集成功，`capture.raw` 和 `capture.vcd` 自动落盘。
2. WavePanel 自动打开硬件波形。
3. 选择仿真 VCD 后，HW/Sim 双轨比较完成。
4. 若契约包含输入探针，生成 replay 场景并记录 `replay.vcd`。
5. 会话目录同时保留 `capture.raw`、`capture.vcd`、`replay.vcd`、`compare.json` 和映射 sidecar。

现场通过不等于 P4 完成：还必须通过第 2、3、4 节的软件和 Verilator 门禁。
