# TraceBridge Tang Nano 9K 测试工程

这是一个最小可运行的 TraceBridge 测试工程：LED 计数器作为被测逻辑，调试 UART 使用 Tang Nano 9K 板载 FTDI 的 17/18 引脚。

完整验收（软件回归、实物采集、输入重放、首差异定位和负向用例）见 [`docs/TraceBridge-Full-Acceptance.md`](../../docs/TraceBridge-Full-Acceptance.md)。

## 工程文件

- `sigflow.project`：SigFlow 工程入口。
- `debug-contract.json`：由 FPGA 菜单中的 `debug_contract` 配置界面生成的采集契约。
- `constraints/min_led_uart_top.cst`：普通用户逻辑的 Tang Nano 9K 管脚约束。
- `rtl/min_led_uart_top.v`：被测 RTL。

## GUI 测试流程

打开工程不会静默下载位流；请在 TraceBridge 中点击“一键流程”并确认下载，程序会按
“构建调试位流 → 下载 FPGA → 开始采集”顺序执行。这样可以避免使用旧位流导致同步校准
阶段 `response timeout`。

1. 启动 `bin/x64/Debug/main.exe`。
2. 打开本目录作为 SigFlow 工程。
3. 执行 `FPGA > Synthesis`，等待 Yosys 完成（首次或 RTL 改动后需要）。
4. 打开 `FPGA > TraceBridge Hardware Capture...`。
5. 加载本目录的 `debug-contract.json`，选择 `COM7` 和 `921600`；之后在 GUI 中修改的有效配置会自动写回该文件。
6. 点击 `构建、下载并采集`，看到确认框后选择“是”。
7. 等待日志依次出现构建完成、FPGA 下载完成和采集完成；成功后自动打开
   `capture.vcd`，文件位于 `.sigflow/debug/<session_id>/artifacts/`。

需要分步排查时，仍可使用 `构建调试位流`，然后通过 `FPGA > Program Board` 下载，
最后点击 `开始采集`。

注意：磁盘中的基础契约只采集 `state/led`，适合先验收硬件链路。要验收“生成重放 VCD”，在 TraceBridge 探针编辑框中改为：

```text
min_led_uart_top.clk 1
min_led_uart_top.rst_n 1
min_led_uart_top.state 4
min_led_uart_top.led 4
```

修改探针后必须重新构建、下载匹配的调试位流，再重新采集；否则旧位流与新契约的指纹/探针布局不一致。

## 板卡快速验证

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_tracebridge_hardware_smoke.ps1 `
  -Port COM7 -Baud 921600 -ExpectedDepth 1024 -ExpectedWidth 32 `
  -ReadCount 1024 -Decimation 1
```

对本示例执行主机 → FPGA → 主机的波形回环强校验：

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_tracebridge_hardware_smoke.ps1 `
  -Port COM7 -Baud 921600 -ExpectedDepth 1024 -ExpectedWidth 32 `
  -ReadCount 16 -SyncCalibrate -VerifyDemoWaveform
```

预期同时出现 `HARDWARE LOOPBACK WAVEFORM PASS (16 samples)`；该检查会验证
读回样本的地址、`led/state` 打包关系和连续采样递增。

如果此前使用 `-SyncCalibrate` 后出现 `response timeout`，先去掉该开关完成基础 PING/GET_INFO 验收；同步前导是可选校准测试，不是基础采集必需项。

UART 连接约定：板载 FTDI TX 对应 `dbg_rx`，板载 FTDI RX 对应 `dbg_tx`，地线共地。

如果板子没有回应，可先下载仓库根目录的已验证镜像：

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_tracebridge_hardware_smoke.ps1 `
  -Port COM7 -Baud 921600 -Bitstream out/full_17_18.fs -Program
```
