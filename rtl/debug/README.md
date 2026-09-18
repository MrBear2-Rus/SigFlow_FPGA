# rtl/debug — TraceBridge 采集核

## 文件

- `sf_micro_ila.sv`：精简采集核（P1a + P1a.2 增强触发）。单模块，环形采样 +
  触发位置冻结 + 顺序读出；预触发重排由宿主完成。
- `sf_micro_ila_tb.sv`：自动检查 testbench（需要 Verilator/iverilog 运行，
  当前仓库无仿真器时由 yosys 做综合检查、由行为等价 C++ 测试验证语义）。
- `sf_uart_link.sv`：8N1 UART 字节收发引擎（P1b），波特率由 `CLK_HZ/BAUD` 分频。
- `sf_debug_link.sv`：协议核（P1b），COBS + CRC-16/CCITT 帧编解码 + 协议状态机
  （PING/GET_INFO/CONFIG/ARM/STATUS/READ_CAPTURE/RESET），桥接 sf_micro_ila。
  当前可综合 MVP 每次 READ 返回 1 个采样，主机自动分块读取；这是为避免
  动态数组索引被映射为大规模 LUT 多路选择树的资源边界。
- `sf_debug_link_minimal.sv`：默认的专用 UART 低资源协议核。固定帧、无 COBS/CRC，
  提供 CONFIG（mask/value）、ARM、STATUS、单点 READ 和 RESET。
- `sf_debug_link_tb.sv`：协议核 testbench（待仿真器）。

## 接口要点

- `arm`：拉高一拍启动采集。
- `trigger_mask` / `trigger_value`：掩码相等触发；`mask=0` 立即触发。
- `trigger_mode`：0=掩码相等/第 N 次匹配（`trigger_count`），1=停滞（掩码子集
  连续 N 个采样周期不变），2=握手超时（`hs_valid && !hs_ready` 连续 N 拍）。
  新端口（mode/count/sample_en/hs_*）均带默认值，旧例化无需改动；
  mode=0、count=1、sample_en=1 时行为与 P1a 完全一致。
- `sample_en`：采样门控；为 0 时暂停采样（写指针/触发计数/prev 不更新），读端口不受影响。
- `trigger_index`：触发样本的环形地址；宿主按
  `rel_time_k = mem[(trigger_index + k) % DEPTH]` 重排。
- `DEPTH` / `WIDTH` 参数化（默认 1024x32），存储为 Gowin BSRAM
  （`ram_style="block"`，yosys synth_gowin 映射为 **DPX9B 真双端口**），
  写端口（wptr）与读端口（rd_addr）独立，`busy=1` 采集进行中即可并发读回
  （边采边传，`DebugLinkLoopbackSmoke` 已覆盖）。
- 资源实测（nextpnr PnR，32x1024，同 wrapper/CST 流程）：
  基线（仅掩码相等）LUT4 108 / DFF 130 / Fmax 179 MHz；
  增强触发（全模式动态驱动）LUT4 223 / DFF 154 / Fmax 98 MHz，
  均 2 块 BSRAM；27 MHz 采样时钟下增强版仍有 3.6x 裕量。
- 完整 TraceBridge Overlay（`sf_micro_ila + sf_uart_link + sf_debug_link`，
  32x1024，GW1NR-LV9QN88PC6/I5）实测 LUT4 1496 / 8640（17%）、
  DFF 526 / 6480（8%）、BSRAM 2 / 26（7%）、Fmax 62.64 MHz。
  相比旧的可变长帧缓冲实现（3714 LUT4 / 1115 DFF / 66.02 MHz），LUT4 减少
  60%、DFF 减少 53%。剩余主要来自 COBS/CRC 和协议状态机，并非 ILA 存储本体。
- 最小 TraceBridge（`sf_micro_ila + sf_uart_link + sf_debug_link_minimal`，同一
  32x1024、GW1NR-LV9QN88PC6/I5、921600 专用 UART）实测 **579 LUT4 / 319 DFF /
  2 BSRAM / Fmax 112.41 MHz**。该值包含最小用户 LED/UART 例程；不是 ILA 单体资源。

## UART 协议档位

调试契约使用 `transport.protocol` 选择协议；缺省为 `minimal`：

```json
"transport": {
  "kind": "uart",
  "protocol": "minimal",
  "tx_port": "dbg_tx",
  "rx_port": "dbg_rx"
}
```

- `minimal`（默认）：调试 UART 独占。固定帧，低资源；只支持 mask/value 的单次触发和
  `decimation=1`。板端没有 PING、GET_INFO、COBS/CRC 或在线指纹读回；宿主以构建会话的
  契约指纹标识产物。
- `full`：COBS + CRC、PING、GET_INFO、在线指纹校验、扩展触发参数和抽取字段。适合需要
  更强链路完整性或扩展触发能力的场景，但资源明显更高。

Tang Nano 9K 的板载调试 UART 已实测固定为 `dbg_tx=17`、`dbg_rx=18`，默认调试复位
固定为高电平；正式 Overlay 未配置复位引脚时会在 RTL 内生成该固定值，避免悬空复位输入。

完整协议的触发模式还支持 `edge_rising` 和 `edge_falling`，分别编码为 3 和 4；
两者按选中掩码位累计第 N 次边沿，最小固定帧协议仍保持单次掩码触发限制。

`DebugAcquisition` 会按该字段选择主机协议。对 `minimal` 的扩展触发、停滞/握手意图或
抽取请求会明确失败，不会静默生成与请求不一致的采集结果。

## Overlay 与直接集成

以相同用户逻辑、同一 ILA、专用 UART 和 CST 引脚，完成了两次
`yosys -> nextpnr -> gowin_pack` 实测：自动 Overlay 与手写直接顶层均为
**579 LUT4 / 319 DFF / 2 BSRAM / Fmax 112.41 MHz**，且均生成 `.fs`。
完整可复现记录见 `docs/tracebridge_overlay_vs_direct_resource_comparison.md`，命令入口为
`tools/compare_debug_integration_resources.ps1`。这说明 Overlay 是不修改用户源的集成形式，
不是额外的调试资源来源。

## P1b 说明

- 单时钟域：UART 与采样共用 27 MHz 时钟，`sf_cdc_control` 取消（跨时钟留多时钟版本）。
- 27 MHz 下各波特率分频误差：115200 → 0.16%、460800 → -0.69%、921600 → 1.02%、
  1500000 → 0%、3000000 → 0%（均 < 2.5%）。
- `sf_debug_link.sv` 采用流式 COBS 解码、固定字段命令解析和流式响应编码，已完成
  Yosys 综合与 nextpnr P&R。它不保存可变长帧，因此 READ 固定为单样本；
  `DebugProtocol::ReadCapture()` 自动拆分范围读取。该取舍将完整 Overlay 从
  3714 LUT4 降至 1496 LUT4。

## 对齐与集成状态

- RTL 参考实现与 C++ 行为基准（`DebugLinkProtocol.h`）已对齐：GET_INFO 的 data[0]
  固定写 IP 版本；READ 的 start/count 字节偏移；CONFIG 扩展字节（mode/count）。
  Loopback 与模型测试对同一字节流语义验证。
- Overlay wrapper（`DebugOverlayBuilder`）已把 `trigger_mode` / `trigger_count` /
  `sample_en` 从 `sf_debug_link` 接到 `sf_micro_ila`；握手信号 `hs_valid` / `hs_ready`
  由契约 `trigger.hs_valid_path` / `hs_ready_path` 指定的用户信号接线（空则不接）。
- `decimation` 已由 `sf_micro_ila`、C++ 行为模型、协议链路和板测脚本共同消费：
  0/1 表示不抽取，N>1 表示每 N 个墙钟采样周期保留一个逻辑样本，触发计数和
  `trigger_index` 使用抽取后的逻辑采样周期。

## 多时钟与本轮门禁

- 多时钟契约要求同步探针声明时钟域，异步观察必须显式标记。
- sf_cdc_control 使用请求/应答 toggle 和稳定 payload，避免多位控制总线直接跨域采样。
- sf_multiclock_capture 为每个时钟域实例化独立采样核；当前统一 UART 回读仍只接一个域。
- tools/run_multiclock_rtl_check.ps1 会对两个多时钟 RTL 模块执行 Yosys 综合/结构检查。

## Loopback CI（无硬件闭环）

- `tests/debug/DebugLinkProtocol.h`：`sf_debug_link`/`sf_uart_link` 的 C++ 行为基准
  （COBS/CRC/协议状态机/采集模型），供模型 smoke 与闭环测试复用。
- `main/debug/PipeTransport.h/.cpp` + `ITransport.h`：命名管道字节流传输，
  与未来 `SerialTransport` 共用接口。
- `tests/debug/DebugLinkLoopbackSmoke.cpp`：主机协议栈 ↔ 模型闭环（PING/GET_INFO/
  CONFIG/ARM/STATUS/READ、指纹不匹配、CRC 错误注入、环形回绕、超时/断连），
  并产出 `capture.raw`/`capture.vcd` golden 对照。
- 统一入口：`tools/run_debug_ci.ps1`（全部 debug 冒烟 ALL PASS）。
