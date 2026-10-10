# SigFlow Plan 8.9

> 基于 [SigFlow-TraceBridge-Design.md](./SigFlow-TraceBridge-Design.md) 的精简规划稿
> 目标硬件：Sipeed Tang Nano 9K（GW1NR-LV9QN88PC6/I5）
> 设计原则：复用现有 SigFlow 能力，补齐"板上真实波形"闭环，不扩展为新综合器/仿真器/通用 ILA。

---

## 0. 背景知识：为什么需要 TraceBridge？

### 0.1 软件里跑"仿真"和芯片里跑"上板"，为什么经常结果不一样？

写数字电路（Verilog / 电路图）时，工程师通常先在电脑上用 **仿真器**（比如本工程用的 Verilator）跑一遍：把输入激励写进 testbench，看波形是否符合预期。这一步叫 **仿真验证**。

但仿真正确 ≠ 真正烧到 FPGA 芯片后就一定正确。常见差异来源：

| 问题 | 仿真里发生了什么 | 上板后真实情况 |
| --- | --- | --- |
| **复位时序** | testbench 里 `rst_n` 通常是完美的、与时钟对齐的 | 真实按钮/上电复位会抖，或者与时钟不同步，导致状态机跑飞 |
| **时钟/约束** | 仿真里假设所有寄存器都按时钟沿准时更新 | 布局布线后路径延迟可能太长，Fmax 跑不上去，出现亚稳态 |
| **FIFO / 握手** | 仿真里 testbench 按时发 valid，模拟出完美 ready | 真实外设可能不及时握手，导致死锁或丢数据 |
| **位宽/符号** | 仿真里通常不会超出位宽，负数自动处理 | 真实数据截断/符号位处理错误，结果直接偏离 |
| **综合优化** | 仿真看的是 RTL 行为，不做网表优化 | Yosys 可能把"看起来没用的信号"优化掉，实际调试时没东西看 |

结论：**仿真只能证明逻辑在理想情况下正确，上板才能暴露真实世界的问题。** 但上板后"芯片里面发生了什么"是看不见的——所以需要"硬件调试"。

---

### 0.2 硬件调试是什么？和软件断点调试有什么不同？

软件开发时，你可以打断点、单步、看变量值。硬件（FPGA / ASIC）里没有"暂停 CPU 看内存"这么方便：

- FPGA 内部是 **硬件逻辑门 + 触发器**，每个时钟沿所有寄存器同时更新，没法"一步一步跑"。
- 要看到内部信号状态，必须 **提前在硬件里塞一个"采样探头"**，把想观察的信号值写到片上存储器（RAM）里。
- 等异常发生或触发条件命中后，再通过某种通道（UART / JTAG / 以太网）把 RAM 里的历史数据 **读回电脑**，还原成波形看。

> 类比：硬件调试 = 在芯片里装一个"高速行车记录仪"，记录仪只在"出事"前后一段时间保存画面，之后你把存储卡拔下来在电脑上回放。

---

### 0.3 ILA 是什么？

**ILA = Integrated Logic Analyzer（集成逻辑分析仪）**，是 FPGA 厂商提供的"行车记录仪 IP"：

- Xilinx（AMD）叫 **ILA**（集成在 Vivado 里，通过 JTAG 读回）
- Intel（Altera）叫 **SignalTap II**
- Lattice 叫 **Reveal**
- Gowin（本工程用的国产高云）叫 **FS Inspector** 或自己接 JTAG

它们的共同点：
1. 你在综合前指定"想看哪些信号" → 工具自动在网表里插入探针。
2. 综合/布局布线后占用一定 LUT 和 RAM 资源。
3. 下载到板卡后，厂商配套软件通过 JTAG 回读采样数据，显示波形。

**但传统 ILA 有几个痛点：**

- 📌 **不跨厂商通用**：Xilinx 的 ILA 不能在 Gowin 上用，换板卡就得换工具链。
- 📌 **厂商工具绑死**：必须打开 Vivado / Gowin IDE，没法在自己的 IDE（比如本工程 SigFlow）里直接操作。
- 📌 **只能看硬件波形**：给你一张硬件波形图，和仿真波形对不齐，得自己人肉比较哪个信号先错。
- 📌 **插桩不可逆**：选好探针后要重新综合+PnR，耗时长，且会改工程文件（Git 脏）。
- 📌 **没有"复现闭环"**：你看到硬件波形错了，想回到仿真里复现，得手动写 testbench，输入激励和硬件当时的输入未必一致。

---

### 0.4 SigFlow TraceBridge 的"软硬件联合调试"创新点在哪里？

TraceBridge 不是做一个新 ILA，而是把 **「仿真 → 构建 → 上板 → 回读 → 对齐比较 → 定位」全链路打通**，做成 SigFlow IDE 里的一体化调试会话。核心创新可以用一张对比表理解：

| 传统 ILA 调试流程（痛苦） | SigFlow TraceBridge（一体化） |
| --- | --- |
| 1. 打开厂商 IDE，手动选信号插探针 | 1. 在 SigFlow 里（SFTree / 画布 / 波形面板）点选信号，IDE 自动分配探针位 |
| 2. 厂商工具生成 ILA 网表，原工程文件变脏（Git 有 diff） | 2. 生成 **影子调试构建**，原 RTL / 约束完全不改动，Git 干净 |
| 3. 重新综合、PnR（可能等十几分钟），资源/时序恶化了也不知道 | 3. 复用现有 Yosys/nextpnr 链，**调试前就估算资源增量与时序影响**，超预算就提示 |
| 4. 打开厂商波形软件，JTAG 连板，手动布防 | 4. 在同一个 TraceBridge 面板里一键"构建下载 + 布防 + 读取" |
| 5. 拿到硬件波形，自己开另一个仿真软件看仿真波形 | 5. 同一 WavePanel 里 **仿真 vs 硬件双轨同屏，联动游标，统一时间轴** |
| 6. 人肉找"第一个不一样的周期"，眼都花 | 6. **三层锚点自动对齐 + 首差异高亮**，直接告诉你哪个信号、哪个时钟沿、期望值多少、实测多少 |
| 7. 波形错位，可能是"仿真激励和真实输入不一样"导致，难以区分 | 7. **输入重放差分**：从硬件采样中提取真实输入，回灌 Verilator 重新跑，得到与真实激励一致的参考基准，区分"设计 bug"还是"激励不同" |
| 8. 厂商私有 JTAG，协议不透明，回放困难 | 8. 纯 UART + COBS + CRC-16 公开协议，原始帧归档，可用 CI 无硬件闭环回归 |
| 9. 下次复现同样 bug：重做一遍 | 9. **调试契约 + 会话导出包**，一键导入他人完整故障现场（探针/触发/波形/日志全打包） |
| 10. 探针被优化掉了还不知道，采样到空波形 | 10. 综合后 **校验 JSON 网表**，探针丢失/位宽不对立即阻断，不下载"看似成功"的 bitstream |

一句话总结：**TraceBridge 把传统 ILA 的"单点采样工具"升级成"仿真 ↔ 硬件双向闭环的可复现调试平台"，特别适合教学、课程设计、小型 FPGA 原型等"验证资源不足但迭代快"的场景。**

---

## 1. 一句话定位

SigFlow TraceBridge 把 RTL、Verilator 仿真波形、FPGA 调试构建和板上采样统一为一个可复现的调试会话：用户选信号 + 配触发 → IDE 生成不污染原工程的调试镜像 → 下板后回读采样 → 与仿真 VCD 对齐并定位首个分歧。

主要解决课程设计/小型原型/教学中的高频问题：仿真正确但上板错误、约束/时钟错误、复位时序、FIFO/握手死锁、状态机跑飞、接口位宽/符号。

## 2. MVP 规模边界

| 项目 | MVP 指标 | 后续 |
| --- | --- | --- |
| 板卡 | Tang Nano 9K | 由 Target Profile 扩展 |
| 采样时钟域 | 1 个 | 每域独立采样核 |
| 探针宽度 | 32 bit | 64/128 bit |
| 采样深度 | 1024 | 2048/4096 |
| 触发 | 掩码相等（mask=0 立即触发） | 边沿/组合/序列/计数 |
| 回读 | UART 921600（默认） | USB/JTAG/以太网 |
| 参考波形 | Verilator VCD + 输入重放 replay.vcd | 多仿真器 |

32 bit × 1024 样本 = 32 Kbit BSRAM，加控制逻辑在 9K 上约占 1% LUT、2% DFF、7% BSRAM（nextpnr 实测 Fmax 179 MHz）。

## 3. 五层架构

1. **调试契约层** — `debug-contract.json` 描述探针/时钟/触发/传输/指纹，是后续所有步骤的唯一输入。
2. **调试构建层** — 影子副本生成插桩 RTL/约束/脚本，复用现有 Yosys / nextpnr / openFPGALoader。
3. **板上采样层** — `sf_micro_ila` 单模块采集核，BSRAM 环形缓存 + 掩码触发。
4. **传输与归档层** — UART/COBS/CRC-16 回读，保存 `capture.raw` → 生成 `capture.vcd`。
5. **比较与呈现层** — 双轨 WavePanel + 三层锚点对齐 + 首差异定位 + AI 只读解释。

## 4. 核心模块

### 4.1 调试契约 `debug-contract.json`

- Schema 1.0；IDE 表单生成，用户不手写。
- 字段：`probes`（路径/宽度/bit_offset/时钟域）、`sample_clock`、`trigger`（含 `intent` 模板）、`capture`、`transport`（引脚/baud/同步校准）、三重指纹（源/工具/`.fs` 字节哈希）。
- 不可变，便于版本管理与回归。

### 4.2 会话目录 `.sigflow/debug/<session-id>/`

```
debug-contract.json   manifest.json
overlay/  scripts/  logs/  artifacts/  reports/
capture.raw  capture.vcd  replay.vcd  compare.json
```

清理策略：保留最近 20 个终态会话，活跃会话不清理，不触碰 `.sigflow/sim/`。

### 4.3 硬件 `sf_micro_ila.sv`

- 单模块精简采集核（约 60 行），合并原 6 个子模块。
- 状态机简化为 `IDLE → RUNNING → DONE`；DONE 仅冻结 RAM，用户设计 free-run。
- 触发：`(probe & mask) == value`，`mask=0` 立即触发。
- 存储：Gowin BSRAM（`ram_style="block"` → DPX9B），`DEPTH`/`WIDTH` 参数化默认 32×1024。
- 预触发重排由宿主 `CaptureDecoder` 按 `trigger_index` 完成：`mem[(trigger_index + k) % DEPTH]`。
- 关键网施加 `(* keep *)`，综合 JSON 产物校验探针存在与位宽。

### 4.4 UART 协议（921600 默认）

- 帧：`0x00 | COBS(payload) | 0x00`，payload = `version | type | sequence | length | data | crc16`。
- 帧类型：PING/PONG、GET_INFO/INFO、CONFIG/ACK、ARM/ACK、STATUS、READ_CAPTURE/SAMPLES、RESET。
- 上位机发 `0x55/0xAA` 同步序列做波特率校准。
- 1024×32-bit + 帧开销 ≈ 5 KB，921600 下回读 ≈ 0.6 s（≤1 s 验收目标）。
- 误码/缺失块有限重读，仍失败保留 `capture.raw` 并报具体缺失块，禁止生成误导性 VCD。

### 4.5 软件代码布局 `main/debug/`

```
DebugContract          DebugSession          DebugOverlayBuilder
DebugBuildService      SerialTransport        DebugProtocol
CaptureDecoder         WaveformAligner        WaveformComparator
DebugPanel             (Replay* 四件套)
```

## 5. 闭环流程（IDE 交互）

1. 用户在 TraceBridge 面板选顶层模块、采样时钟、探针、触发条件（三入口：SFTree / 画布 / 波形信号树）。
2. IDE 校验信号、位宽、时钟域、UART 引脚、资源预算 → 生成契约。
3. 构建并下载调试镜像（影子工作区，复用现有 Yosys/nextpnr/打包/下载）。
4. 串口 `PING`/`GET_INFO` 验证指纹与参数（`build_id + fingerprint64 + .fs` 哈希）。
5. 布防 → 等触发 → 自动分块读取 → 生成 `capture.vcd`。
6. "与仿真比较" → 加载仿真 VCD / replay.vcd → 三层锚点对齐 → WavePanel 标记首个差异。
7. 异常信号可跳转 RTL 行 / SFTree / 画布；可导出会话包复现。

## 6. 波形对齐与比较

三层锚点（解决上电/复位/布防时刻不同导致绝对时间不可比）：

1. 复位释放边沿（无复位探针用用户同步标志）
2. 触发命中样本（与仿真同条件首命中）
3. 顶层输入事务变化序列（消除固定周期偏差）

比较器输出："首个不同采样点 / 信号 / 期望 / 实测 / 上游探针 / 置信度"。宽度/采样周期/对齐质量不达标时标"待人工确认"。

**输入重放差分（创新点 A）**：当输入探针完整覆盖顶层输入时，从硬件采样提取激励 → 生成 `replay_tb` → Verilator 重放 → `replay.vcd` 作为默认参考基准；原仿真 VCD 降为二级。激励一致性不足时禁止输出确定性首差异结论。

## 7. 创新点（差异化价值）

| 创新点 | 一句话 |
| --- | --- |
| 调试契约 | 单一 JSON 驱动仿真/构建/硬件/比较，可一键导入他人故障现场 |
| 影子调试构建 | 原 RTL 不动，插桩仅在 overlay，Git 工作区干净 |
| 构建指纹闭环 | 硬件回传设计/工具/探针哈希，防用旧 bitstream 误判新波形 |
| 语义双轨比较 | 复位/触发/输入三层对齐，输出"第一个差异"而非两张孤立波形 |
| 输入重放差分 | 把 Capture-to-Scenario 从复现工具升级为比较工具 |
| 无硬件闭环 CI | PipeTransport + Verilator 模型，协议栈/解码器/比较器全 CI 回归 |
| 资源感知探针建议 | 探针选择与 nextpnr 报告联动 |
| 调试意图触发器 | "握手超时/FIFO 空读"模板自动展开为位掩码 |
| 首因候选图 | 基于 SFTree 驱动关系向上游追溯，排序候选根因 |
| 调试版影响量化 | 普通 vs 调试构建资源/slack/扇出对比，高风险阻断 |
| 硬件行为摘要 | 确定性事件时间线 + AI 只读润色 |

## 8. 波形面板升级（参考 Bear2Wave）

分四层独立可验收，与 P3/P4 并行：

- **W1 数据层**：统一 trace 加载 API、VCD 懒加载、侧车索引 `.bwv`、内存预算 LRU。
- **W2 渲染层**：OpenGL 批量渲染 + 文字层纹理合成，wxDC 软件回退。
- **W3 交互层**：Compare 联动、Marker/A-B 测量、模式搜索、UART 协议 lane、会话保存恢复、事件时间线。
- **W4 集成层**：接入 `capture.vcd` / `compare.json`，差异高亮 + RTL/SFTree 双向跳转。

> 移植原则：能力移植 + 重写适配，**不整目录复制**。FST/LXT 等 GPL 读库 vendor 前需许可证评估。

## 9. 风险与缓解

| 风险 | 缓解 |
| --- | --- |
| 内部网被优化 | `keep` 属性 + 综合 JSON 校验，丢失即构建失败 |
| 多时钟域采样 | MVP 强制单域，跨域明确标"异步观察" |
| 插桩影响 Fmax | 显示时序增量、限探针宽度、分组采样、slack<0 阻断 |
| 串口误码/断连 | COBS+CRC+序号+重读+原始帧归档 |
| UART 引脚冲突 | 引脚冲突检查 + 外接 USB-TTL 选项 |
| 仿真激励与真实输入不同 | 输入重放差分 + 激励一致性评分 |

## 10. 里程碑

| 阶段 | 周期 | 主要交付 | 状态 |
| --- | --- | --- | --- |
| P0 | 3 天 | 契约/Schema/状态机/交互稿/阈值/指纹 | ✅ 完成（ALL PASS） |
| P1a | 1 周 | 采集核 `sf_micro_ila`（RAM/触发/hub） | ✅ 完成（nextpnr PnR 通过，行为等价测试 PASS） |
| P1b | 1 周 | UART/COBS/CRC、CDC、板卡验证 | ⬜ 待开始 |
| P2 | 2 周 | 调试构建接入（overlay/综合作业/指纹校验/引脚检查） | ⬜ |
| P3 | 2 周 | 采集/回读/双轨呈现 + Loopback CI | ⬜ |
| P4 | 2 周 | 比较器/重放/会话导入导出 | ⬜ |
| W1–W4 | 并行 | 波形面板升级 | ⬜ |
| P5 | 1 周 | 打磨/演示/发布 | ⬜ |

## 11. 当前进展要点

- **P0 已落地**：`main/debug/` 新增 `DebugContract` / `DebugFingerprint` / `DebugSession` / `DebugThresholds` + 三个 JSON Schema；接入 main.vcxproj，0 错 0 警；`DebugP0Smoke.cpp` ALL PASS（契约/指纹/状态机/会话/清理/隔离）。
- **P1a 已落地**：`rtl/debug/sf_micro_ila.sv` + tb + rescheck + README；nextpnr 实测 1% LUT / 2% DFF / 7% BSRAM / Fmax 179 MHz；行为等价 C++ 测试 ALL PASS。
- **关键教训**：早期用分布式 RAM（寄存器数组）实现存储 → yosys 单元计数约 9600 LUT + 2048 DFF，nextpnr 布局不收敛；改用 BSRAM 后资源骤降至 1% LUT。该决策已固化在 README。
- **待办阻塞点**：仓库当前无 Verilator/iverilog，SV testbench 待接入仿真器后运行；行为等价 C++ 测试先行覆盖语义。

## 12. 下一步优先级

1. **P1b 启动**：实现 `sf_uart_link.sv`（UART/COBS/CRC/帧序号/超时）与 `sf_cdc_control.sv`，板卡验证 16bit×256 与 32bit×1024 两种配置。
2. **T-CI-01 并行**：抽象 `SerialTransport` 接口，实现 `PipeTransport`，为 P3 无硬件回归铺路。
3. **P2 前置准备**：`DebugOverlayBuilder` 设计稿（顶层 wrapper + 探针接线 + `keep` 注入 + CST 补丁）。

## 13. 验收红线（不可妥协）

- 用户 RTL 文件与项目配置在调试构建后内容不改变。
- 探针被优化 / UART 引脚冲突 / 板上指纹不一致 → 必须阻断后续操作。
- 帧 CRC 错误时可靠重读或明确失败，禁止生成误导性完整 VCD。
- 默认 921600 下 1024×32-bit 完整回读+校验+VCD ≤ 1 s。
- 调试构建 slack < 0 或探针丢失 → 采样结果不得标记为可信。
- 激励一致性不足时禁止输出确定性首差异结论。
- AI 插件只解释结构化结果，不得修改构建/约束/源代码。
