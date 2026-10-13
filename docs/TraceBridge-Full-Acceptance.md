# TraceBridge 完整验收示例与流程

本文使用 `examples/tracebridge_tangnano9k` 和 Tang Nano 9K，覆盖：

1. 软件回归；
2. Tang Nano 9K 17/18 UART 实物采集；
3. `capture.vcd` 波形呈现；
4. Capture-to-Scenario 输入重放；
5. 首差异、首因候选和 RTL/SFTree/画布定位。

## 1. 验收前准备

硬件连接和串口要求：

- 板卡通过 USB 连接电脑，设备管理器确认串口为 `COM7`；如果端口号不同，后续命令统一替换。
- Tang Nano 9K 板载 FTDI 的实际调试连接为 `dbg_tx=17`、`dbg_rx=18`。
- UART 采用交叉语义：FTDI TX → `dbg_rx`，FTDI RX → `dbg_tx`，双方 GND 共地。
- 关闭串口助手、串口监视器和其他占用 `COM7` 的程序。

工具检查：

```powershell
Get-Command verilator
Test-Path .\external\fpga-tools\runtime\openfpgaloader\bin\openFPGALoader.exe
```

`verilator` 是输入重放验收必需项；硬件采集本身不依赖它。

## 2. 软件回归验收

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_debug_ci.ps1
```

通过条件：

- 最后一行是 `DEBUG CI: ALL PASS`；
- `tracebridge_priority_smoke` 中出现 capture-to-scenario、候选图、映射 sidecar 和比较结果全部 `ok`；
- `debug_loopback_smoke`、`proto_smoke`、`acq_smoke` 均为 `ALL PASS`。

主程序编译应使用解决方案，而不是直接编译 `main\main.vcxproj`：

```powershell
& 'D:\Program_Files\VS2022\MSBuild\Current\Bin\MSBuild.exe' `
  '.\SigFlow.sln' /p:Configuration=Debug /p:Platform=x64 /m
```

通过条件：`已成功生成`、`0 个错误`。第三方库可能产生既有警告，不影响本验收。

## 3. 板卡快速验收

### 3.1 下载已验证镜像

先用已经验证过的 32 bit × 1024 镜像，排除调试构建变量：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_tracebridge_hardware_smoke.ps1 `
  -Port COM7 -Baud 921600 `
  -Bitstream .\out\full_17_18.fs -Program
```

### 3.2 先做最小通信检查

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_tracebridge_hardware_smoke.ps1 `
  -Port COM7 -Baud 921600 `
  -ExpectedDepth 1024 -ExpectedWidth 32 -ReadCount 4
```

预期输出至少包括：

```text
PING/PONG PASS
GET_INFO PASS (32x1024)
CONFIG PASS
ARM PASS
STATUS PASS
READ_CAPTURE PASS (4 samples)
```

确认最小通信通过后，再读完整深度：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_tracebridge_hardware_smoke.ps1 `
  -Port COM7 -Baud 921600 `
  -ExpectedDepth 1024 -ExpectedWidth 32 `
  -ReadCount 1024 -Decimation 1
```

预期：`READ_CAPTURE PASS (1024 samples)`，无 timeout、CRC 或 sequence 错误。

### 3.3 主机—FPGA—主机波形回环

对仓库示例执行带数据内容校验的回环测试：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_tracebridge_hardware_smoke.ps1 `
  -Port COM7 -Baud 921600 -ExpectedDepth 1024 -ExpectedWidth 32 `
  -ReadCount 16 -SyncCalibrate -VerifyDemoWaveform
```

通过条件：同时出现 `READ_CAPTURE PASS (16 samples)` 和
`HARDWARE LOOPBACK WAVEFORM PASS (16 samples)`。后者验证样本经过 FPGA 采样
RAM 和 UART 回传，而不只是验证命令有响应。

如果已经遇到 `SYNC preamble sent ... response timeout`，本次现场验收先不要加 `-SyncCalibrate`；先按上面的普通命令确认 PING/GET_INFO。`-SyncCalibrate` 是额外校准测试，不是基础采集的必要参数。

## 4. GUI 基础采集验收

1. 启动 `bin\x64\Debug\main.exe`。
2. 打开 `examples\tracebridge_tangnano9k`。
3. 打开 `FPGA > TraceBridge Hardware Capture...`。
4. 加载 `debug-contract.json`。
5. 选择 `COM7`、`921600`、`full` 协议。
6. 如果使用刚下载的 `out\full_17_18.fs`，直接点击开始采集；如果使用 GUI 生成的调试位流，必须下载与当前契约匹配的 `.fs`。
7. 点击开始采集，等待状态从 `Armed` 变为 `Captured`。

基础采集通过条件：

- 自动打开 `capture.vcd`；
- WavePanel 显示 `led`、`state` 两条波形；
- 会话目录存在：
  `.sigflow\debug\<session_id>\artifacts\capture.raw`；
  `.sigflow\debug\<session_id>\artifacts\capture.vcd`；
- 用户 RTL 文件内容未被修改。

## 5. 完整 P4 输入重放验收

仓库中的原始 `debug-contract.json` 适合基础采集，只包含 `led/state`，不能证明输入重放。要完成完整 P4 验收，在 TraceBridge 的探针编辑框中改为以下四行：

```text
min_led_uart_top.clk 1
min_led_uart_top.rst_n 1
min_led_uart_top.state 4
min_led_uart_top.led 4
```

这里的 `clk` 和 `rst_n` 是顶层输入，`state` 和 `led` 是观察信号。探针位偏移由程序自动重新分配；不要手工填写旧的 `bit_offset`。

然后按以下顺序操作：

1. 点击调试构建，等待 overlay、综合、布局布线和打包完成。
2. 下载本次调试构建生成的 `.fs`，不要继续使用旧位流。
3. 回到 TraceBridge，重新开始一次采集。
4. 确认 `capture.vcd` 中可以看到 `clk` 和 `rst_n`。
5. 点击“生成重放 VCD”。
6. 等待生成 `replay_tb.cpp`、运行 Verilator，并自动打开 `replay.vcd`。
7. 点击比对；程序会优先使用硬件输入重放作为参考。

通过条件：

- 会话目录生成 `signal-map.json`；
- 会话目录生成 `dependency-graph.json`；
- `artifacts\replay_tb.cpp` 存在，并包含 DUT 时钟和输入端口驱动；
- `artifacts\replay-consistency.json` 中 `verified` 为 `true`、`score` 为 `1`；
- `artifacts\replay.vcd` 存在且 WavePanel 可以打开；
- `compare.json` 中 `reference_kind` 为 `hardware-input-replay`；
- 同一 RTL、同一硬件输入下没有非预期差异。

## 6. 首差异和首因定位验收

为了看到定位结果，需要让参考波形和硬件波形产生一个已知差异。推荐流程：

1. 先保存一次正确版本产生的 `replay.vcd`。
2. 在副本 RTL 中把计数逻辑从 `state + 1` 改成一个明显不同的行为，例如 `state + 2`。
3. 重新生成调试位流并下载，重新采集硬件波形。
4. 用旧版本仿真/重放 VCD 与新硬件采集执行比较。
5. 点击首差异事件。

通过条件：

- 首差异显示信号名、时间、期望值和实测值；
- 结果带有 `source_path`、`source_line`、`sftree_path`、`canvas_id`；
- 上游候选按图距离排序，直接上游优先；
- 双击结果后编辑器跳到 RTL 行，SFTree 选择对应节点，画布尝试选择对应元素；
- 不存在映射或行号为 0 时，界面显示位置不可用，不伪造定位。

无硬件 GUI 条件下，用以下命令验收同一功能：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_debug_ci.ps1
```

其中 `tracebridge_priority_smoke` 已覆盖已知首差异、依赖图排序和 RTL/SFTree/画布映射。

## 7. 必测负向用例

| 用例 | 操作 | 必须结果 |
| --- | --- | --- |
| 串口占用 | 用串口助手占用 `COM7` 后采集 | 明确提示打开失败，释放后可重试 |
| 错误几何 | 将 `-ExpectedWidth` 改为 `16` | GET_INFO 校验失败，不进入采集 |
| 错误位流 | 下载不匹配的调试位流 | 指纹不一致，阻断后续操作 |
| 无输入重放 | 探针只保留 `led/state` | 提示没有被采样的顶层 input，不输出确定性重放结论 |
| 缺少 Verilator | 从 PATH 移除 `verilator` | 采集仍可用，重放明确提示环境缺少 Verilator |
| CRC/断链 | 运行软件回归 | `debug_loopback_smoke` 和 `proto_smoke` 必须通过 |

## 8. 最终交付检查

一次完整 P4 会话至少应保留：

```text
.sigflow/debug/<session_id>/
├── manifest.json
├── signal-map.json
├── dependency-graph.json
└── artifacts/
    ├── capture.raw
    ├── capture.vcd
    ├── replay-consistency.json
    ├── replay.vcd
    ├── replay_tb.cpp
    └── compare.json
```

最终签字条件：软件回归通过、解决方案编译 0 错误、板卡完整读回通过、GUI 采集成功、重放一致性为真、比较结果可定位，且负向用例不会误报成功。
