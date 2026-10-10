# TraceBridge Overlay 生成修复草案

## 1. 目标

让 TraceBridge 调试构建适用于普通单时钟工程和多时钟工程，并保证：

- 生成的调试位流继续使用用户工程的时钟、复位和普通 IO 管脚；
- UART 分频使用工程真实采样时钟，不再固定假设 27 MHz；
- 上位机协议、契约和 FPGA 链路模块三者一致；
- 多时钟探针不直接跨域采样；
- 生成完成后可以在下载前发现“必然无法通信”的配置错误。

P0 已按本草案实施；P1 多时钟 Overlay、协议闭环和统一复位策略仍在规划中。

## 2. 已确认的问题

### P0：用户 CST 没有并入调试 CST

旧版 `DebugOverlayBuilder::BuildCstPatch()` 只输出 `dbg_rx`、`dbg_tx` 和可选
`dbg_rst_n`。调试构建随后把 nextpnr 的 CST 设置为这个补丁文件，导致原工程的
`clk`、`rst_n`、LED 和其他 IO 约束丢失。

相关位置：

- `main/debug/DebugOverlayBuilder.cpp`：`BuildCstPatch()`；
- `main/MainFrame.cpp`：调试 nextpnr 使用 `overlay.cstPath`。

### P0：UART 时钟固定为 27 MHz

Overlay 生成器给 `sf_debug_link` 和 `sf_debug_link_minimal` 固定传入：

```text
CLK_HZ = 27000000
```

当用户工程的采样时钟不是 27 MHz 时，FPGA UART 实际波特率错误，表现为 PING
超时、CRC 错误或偶发通信。

### P1：多时钟契约没有落到 Overlay 接线

契约已经允许声明多个 `clock_domains`，但当前 wrapper 只生成一个
`sf_micro_ila`，所有探针都使用单一 `sample_clk`。其他时钟域的信号可能被直接
带入采样时钟域，既不符合 CDC 约束，也没有使用 `sf_multiclock_capture`。

### P1：协议选择缺少构建产物闭环检查

生成器根据契约选择 `full` 或 `minimal`，而硬件 smoke 脚本和 GUI 也分别选择协议。
如果契约、生成位流和采集界面不是同一档位，当前主要在运行阶段才表现为 timeout。

### P1：复位策略没有统一定义

没有 `rst_pin` 时，调试核复位被固定为高电平；用户 DUT 的复位仍可能是普通顶层
输入。这样会造成调试核和用户逻辑的启动时刻不一致，尤其影响刚下载后的首次采集。

## 3. 修复设计

### 3.1 CST 合并器

新增一个独立的约束合并函数，不直接覆盖用户 CST：

```text
MergeDebugConstraints(userCst, debugPins, wrapperPorts) -> mergedCst
```

处理规则：

1. 读取项目原始 CST；
2. 保留原有用户端口的 `IO_LOC`、`IO_PORT` 等约束；
3. 将用户顶层端口名称映射到 `sf_debug_top` 的同名端口；
4. 追加 `dbg_rx`、`dbg_tx`、`dbg_rst_n` 约束；
5. 检测重复端口、重复管脚和 UART 与用户端口冲突；
6. 输出会话目录中的 `merged_debug.cst`；
7. nextpnr 只使用合并后的文件。

如果用户 CST 不存在，构建必须明确提示“用户端口未约束”，不能只生成一个仅含
UART 的可下载位流。

验收最低要求：调试生成的 CST 同时包含原工程的 `clk`/IO 约束和 TraceBridge 的
UART 约束。

### 3.2 UART 时钟参数

在生成 wrapper 时使用：

```text
CLK_HZ = contract.sampleClock.frequencyHz
```

并增加范围检查：

- `frequencyHz` 必须在目标 FPGA 支持的合理范围内；
- 根据 `CLK_HZ` 和 `baud` 计算误差；
- UART 误差超过 2.5% 时阻断构建并给出建议；
- 构建产物 manifest 记录 `clk_hz`、`baud`、计算后的分频值和误差。

契约中的 `sample_clock.signal` 必须是 DUT 顶层端口。若时钟来自内部层级或 PLL，
本阶段不自动猜测，构建应提示用户显式导出一个采样时钟端口。

### 3.3 单时钟与多时钟分流

Overlay 生成前先按契约分流：

```text
一个有效时钟域       -> 单个 sf_micro_ila
多个有效时钟域       -> sf_multiclock_capture + 每域一个采集实例
```

单时钟模式：

- 所有探针必须属于默认采样域，或显式标记为 asynchronous；
- 异步探针必须在 manifest 中降低置信度并显示警告。

多时钟模式：

- 每个域生成一个本地域采样核；
- 控制请求通过 `sf_cdc_control` 跨域；
- 不允许把多位控制总线直接接入其他时钟域；
- 回读协议必须携带 `clock_domain` 和域内样本地址；
- 如果当前 UART 回读协议还不能表达多域数据，先阻断 GUI 采集并提示“多时钟
  构建已生成，跨域回读尚未启用”，不能伪装成单时钟采集。

### 3.4 协议闭环

在会话 manifest 中固定记录：

```json
{
  "protocol": "full",
  "clk_hz": 27000000,
  "baud": 921600,
  "tx_pin": 17,
  "rx_pin": 18,
  "bitstream_sha256": "..."
}
```

采集前执行三项检查：

1. GUI 当前协议必须等于契约协议；
2. 下载的 bitstream SHA-256 必须等于当前会话产物；
3. `GET_INFO` 的协议版本、宽度和深度必须匹配契约。

同步校准仍然只是 PING 前的可选前导，不能作为修复错误时钟、引脚或镜像的替代。

### 3.5 复位策略

优先级建议如下：

1. 若契约声明独立 `rst_pin`，调试核使用该复位输入；
2. 若用户 DUT 有顶层复位端口，调试核复位与该端口保持一致；
3. 若用户工程没有外部复位，生成一个明确的上电复位策略，并在 manifest 标记；
4. 禁止无提示地把调试核固定为高电平复位。

## 4. 建议改动文件

### P0 实施状态（本轮）

- `DebugOverlayBuilder` 读取用户 CST，保留原有约束并追加 `dbg_rx`/`dbg_tx`/`dbg_rst_n`；
- 支持 GUI `nextpnr_args` 中配置的显式 `cst=` 路径，缺失或不可读时阻断生成；
- Overlay/link stub 使用 `contract.sampleClock.frequencyHz`，UART 分频误差超过 2.5% 时阻断；
- Wrapper 已统一连接 3-bit `trigger_mode` 和 `decimation`，并在无独立调试复位管脚时明确使用常高复位；
- `DebugOverlaySmoke` 与最小板级工程 smoke 已覆盖用户 CST 保留和 50 MHz 非 27 MHz 时钟。

### P1 协议闭环实施状态（本轮）

- 会话 manifest 记录 `protocol`、`clk_hz`、`baud`、UART 管脚以及采样宽度/深度；
- full 协议采集校验 GET_INFO 的协议版本、宽度和深度；
- 采集前校验会话与当前契约的协议、时钟、波特率和深度，不匹配时阻断并将会话置为 `Failed`；
- bitstream SHA-256 的下载校验沿用现有 `DebugSessionService::ValidateBitstreamForProgramming`；
- 位流与 manifest 的协议档位尚未从 `.fs` 内部反向读取，仍需后续加入产物闭环。
- 多时钟契约在 Overlay 入口明确阻断，避免第二时钟域被静默接入单一采样时钟；分域采集和回读仍待后续实现。

### 第一阶段：P0

- `main/debug/DebugOverlayBuilder.cpp/.h`
  - 使用契约时钟频率；
  - 生成合并 CST；
  - 增加时钟/波特率误差检查；
  - 记录生成参数。
- `main/MainFrame.cpp`
  - nextpnr 使用合并 CST；
  - 生成失败时显示明确原因。
- `main/debug/DebugSessionService.*`
  - manifest 保存 merged CST、时钟、协议和 bitstream 哈希。
- `tests/debug/DebugOverlaySmoke.cpp`
  - 增加非 27 MHz 工程和用户 CST 保留检查。

### 第二阶段：P1

- `main/debug/DebugOverlayBuilder.*`
  - 多时钟 wrapper 生成和域映射；
- `main/debug/DebugAcquisition.*`、`DebugProtocol.*`
  - 多域回读协议或明确阻断；
- `tests/debug/DebugAcquisitionSmoke.cpp`
  - 增加协议不匹配、时钟不匹配和多时钟阻断用例；
- `docs/SigFlow-TraceBridge-Design.md`
  - 将“多时钟已实现”拆成“RTL 已实现”和“Overlay/回读已实现”。

## 5. 分阶段验收

### A. 单时钟 27 MHz 回归

使用现有 Tang Nano 9K 工程，检查：

- 生成 wrapper；
- 生成 merged CST；
- 综合、nextpnr、打包成功；
- 下载后 `PING/GET_INFO/READ_CAPTURE` 通过；
- LED、时钟和 UART 管脚与原工程一致。

### B. 单时钟非 27 MHz

制作一个 24 MHz 或 50 MHz 的最小工程，要求：

- 生成 wrapper 中 `CLK_HZ` 等于契约值；
- manifest 中记录正确分频和误差；
- 下载后仍能在契约 baud 下完成 PING 和 4 点读取。

### C. 用户 CST 保留

在最小工程中给 `clk`、复位、LED 分配固定管脚，调试构建后检查 merged CST：

- 原有每一项约束仍存在；
- `dbg_tx`/`dbg_rx` 追加成功；
- UART 不与用户管脚冲突；
- nextpnr 报告中端口位置与 merged CST 一致。

### D. 协议不匹配负向用例

- 用 `full` 位流按 `minimal` 采集；
- 用 `minimal` 位流按 `full` 采集；
- 修改 bitstream 或 manifest 哈希；

三种情况都必须在下载或采集前给出明确错误，不能只显示 `response timeout`。

### E. 多时钟负向/正向用例

- 两个独立时钟域各采集一组递增计数器；
- 验证 CDC 请求/应答和各域样本独立完成；
- 在多域回读未接通时，GUI 必须阻断并说明原因；
- 不得把第二个时钟域的探针静默接入第一个采样时钟。

## 6. 实施顺序

1. 先修复 CST 合并和真实 `CLK_HZ`，这是导致其他工程直接 timeout 的最高风险项；
2. 再补充协议/bitstream/manifest 一致性检查，消除模糊的 timeout；
3. 最后将多时钟契约真正接入 Overlay 和回读链路；
4. 每阶段都保留当前 Tang Nano 9K 17/18 实物回归，避免修复通用工程时破坏已验证链路。

## 7. 完成定义

修复完成后，任意满足以下条件的单时钟工程应可复用 TraceBridge：

- 顶层导出采样时钟；
- 采样时钟频率写入契约；
- 用户 CST 存在且无 UART 管脚冲突；
- 契约、位流和 GUI 使用相同协议档位；
- 下载匹配位流后，基础 PING、采集和 VCD 生成通过。

多时钟工程只有在每个域都有本地采样核、CDC 控制和明确回读格式后，才能从“规划中”
标记为“已实现”。
