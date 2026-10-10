# SigFlow-Plan-0823
## Yosys / nextpnr 接入以来的工作总结与暑期收官任务布置

> 致 SigFlow 全体组员
> 日期：2026-08-23
> 主题：开源 FPGA 工具链接入复盘 + 暑期收官任务 + AI4EDA 与双版化方向动员

---

## 一、为什么写这份总结

从我们把 Yosys 和 nextpnr 接入 SigFlow 主工程的那一天起，到今天已经走完了一段相当完整的路：从"只有一行命令行调用"到"完整的 Job 化 + 状态机 + manifest + 报告解析 + 调试构建复用"，SigFlow 的 FPGA 侧已经从一个勉强能跑的脚本，长成了一个可以被 Agent 调度、可以被 TraceBridge 复用、可以支撑教育/竞赛场景的工具链底座。

这份文档做三件事：
1. **复盘**：把 Yosys/nextpnr 接入以来所有相关工作（含代码）做一次完整盘点，让每个人看清我们走到了哪里。
2. **布置任务**：暑期收官阶段的三项必做任务（AI4EDA 预研、双版化方案、批判性创新动员）。
3. **动员**：用一段话肯定大家这个夏天的产出，并邀请更野的想法。

---

## 二、Yosys / nextpnr 接入以来的工作全景

### 2.1 接入的起点与目标

接入之初我们定了三条原则，至今未变：
- **不重新发明轮子**：Yosys 做综合、nextpnr-himbaechel 做 P&R、Apicula/gowin_pack 做打包，SigFlow 只做编排与治理。
- **Job 化优先**：每个外部工具必须封装为带状态机、manifest、取消治理的 Job，不能裸调进程。
- **可被复用**：调试构建（TraceBridge）必须能复用同一套 Yosys/nextpnr 流程，而不是另起一套。

### 2.2 已完成的代码与文档清单

#### A. 综合层（Yosys）

| 文件 | 职责 | 状态 |
|---|---|---|
| [FpgaSynthesisJob.h](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/FpgaSynthesisJob.h) | 综合作业服务接口：manifest、状态迁移、取消、重试、产物与报告获取 | ✅ 完成，TraceBridge 复用基线 |
| [FpgaYosysLogParser.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaYosysLogParser.h) | Yosys 日志解析：警告/错误行提取、资源统计 | ✅ |
| [FpgaYosysReport.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaYosysReport.h) | 综合报告生成：LUT/DFF/BSRAM 统计、KPI Tile | ✅ |
| [FpgaSynthesisJobsPanel.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaSynthesisJobsPanel.h) | 综合任务列表面板：状态、进度、产物跳转 | ✅ |
| [CstValidator.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/CstValidator.h) | Gowin CST 约束文件校验：引脚冲突、非法语法 | ✅ |
| [FpgaConstraint.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaConstraint.h) | 约束模型与序列化 | ✅ |
| [FpgaPinBindingPanel.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaPinBindingPanel.h) | 引脚绑定 UI：图形化编辑 CST | ✅ |
| [FpgaPinData.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaPinData.h) | Tang Nano 9K 引脚数据库 | ✅ |
| [ArtifactValidator.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/ArtifactValidator.h) | 产物校验：Yosys JSON 合法性、SHA-256 | ✅ |

**MainFrame 侧调度入口**：
- [MainFrame.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/MainFrame.cpp) L202-L274：读取 `sigflow.project` 中的 `yosys_*` 配置，构建 Yosys 命令参数。
- [MainFrame.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/MainFrame.cpp) L396-L423：`BuildNextpnrReadme()` 生成 `yosys -> nextpnr -> gowin_pack` 流程示例。
- [MainFrame.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/MainFrame.cpp) L2734-L2756：TraceBridge 调试构建中启动 `YosysExecutor`，执行 `yosys -s <overlay script>`。

#### B. 布局布线层（nextpnr）

| 文件 | 职责 | 状态 |
|---|---|---|
| [NextpnrJob.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/NextpnrJob.h) | nextpnr Job：manifest 序列化（project_path/top_module/target_profile/json_path/cst_path/device_name/family_name）、状态迁移 | ✅ Job 化完成 |
| [NextpnrExecutor.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/NextpnrExecutor.h) | nextpnr 进程执行器：命令拼装、超时、取消 | ✅ |
| [NextpnrJobsPanel.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/NextpnrJobsPanel.h) | PnR 任务列表面板：`RecoverStaleJobs/List`、状态刷新、无任务 UI 文案 | ✅ |
| [NextpnrReport.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/NextpnrReport.h) | PnR 报告诊断：packing/placement/routing 阶段失败原因与下一步建议 | ✅ |
| [NextpnrLogParser.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/NextpnrLogParser.h) | nextpnr 日志解析：阶段识别、fmax、关键路径、资源占用 | ✅ |

**注**：nextpnr 的 `--report` JSON（fmax + critical_path）已解析并写入 manifest，是后续门级时序仿真（DTA）SDF 输出的数据基础。

#### C. 打包层（Apicula / gowin_pack）

| 文件 | 职责 | 状态 |
|---|---|---|
| [FpgaPackService.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaPackService.h) | gowin_pack 服务：校验 nextpnr PnR JSON、生成 `.fs` bitstream、输入/输出 SHA-256 | ✅ |
| [FpgaToolWindow.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaToolWindow.h) | FPGA 工具主窗口：综合/PnR/打包三段式进度 | ✅ |
| [FpgaTheme.h](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/fpga/FpgaTheme.h) | FPGA 模块统一配色 | ✅ |

#### D. 调试构建层（复用 Yosys/nextpnr）

这是接入工作中**最有价值的一环**——TraceBridge 没有另起一套构建流程，而是完整复用了上面的 Yosys/nextpnr/gowin_pack Job 链。

| 文件 | 职责 | 状态 |
|---|---|---|
| [DebugOverlayBuilder.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/debug/DebugOverlayBuilder.h) | 影子构建：生成 wrapper（用户 RTL + sf_micro_ila + sf_debug_link），注入 `(* keep *)` 探针，生成 Yosys 脚本与 nextpnr 参数，**原 RTL 零修改 + SHA-256 校验** | ✅ |
| [DebugNetlistValidator.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/debug/DebugNetlistValidator.h) | Yosys JSON 探针校验：位宽、时钟域、信号存在性 | ✅ |
| [DebugContract.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/debug/DebugContract.h) | 不可变调试契约：probes/depth/trigger/clock | ✅ |
| [DebugSession.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/debug/DebugSession.h) | 会话状态机 + manifest 读写 + 目录清理 | ✅ |
| [DebugFingerprint.h/.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/debug/DebugFingerprint.h) | 构建指纹：Yosys/nextpnr/gowin_pack 版本 + 工具链 + `.fs` 哈希 | ✅ |

**MainFrame 侧调试构建调度**：
- [MainFrame.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/MainFrame.cpp) L2636-L2668：`RunTraceBridgeDebugBuild()` 校验 `yosys.exe` / `nextpnr-himbaechel.exe` / `gowin_pack.exe` 存在性，阻止重复任务。
- [MainFrame.cpp](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/main/MainFrame.cpp) L2734-L2756：调试构建中启动 `YosysExecutor`，完成回调里串联 `nextpnrExecutable` 与 `packExecutable`。

#### E. RTL 侧（经 Yosys/nextpnr 验证可综合）

| 文件 | 职责 | 状态 |
|---|---|---|
| [sf_micro_ila.sv](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/rtl/debug/sf_micro_ila.sv) | 采样核：32×1024 BSRAM、4 触发模式、trigger_index 重排 | ✅ Yosys lint/synth 通过，nextpnr PnR 通过 |
| [sf_debug_link.sv](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/rtl/debug/sf_debug_link.sv) | 完整协议链路：COBS/CRC/PING/GET_INFO/fingerprint64 | ✅ Yosys/nextpnr/gowin_pack 通过，Tang Nano 9K bring-up |
| [sf_debug_link_minimal.sv](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/rtl/debug/sf_debug_link_minimal.sv) | 最小协议（579 LUT4） | ✅ |
| [sf_uart_link.sv](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/rtl/debug/sf_uart_link.sv) | UART 物理层：921600 波特率 | ✅ |
| [min_led_uart_top.v](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/rtl/debug/examples/min_led_uart_top.v) | 示例工程：4 位 LED 计数器 + UART 心跳 | ✅ |
| [min_led_uart_direct_debug_top.sv](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/rtl/debug/examples/min_led_uart_direct_debug_top.sv) | 调试 wrapper 示例 | ✅ |

#### F. 设计与规划文档

| 文档 | 内容 | 状态 |
|---|---|---|
| [SigFlow-Plan-8.9.md](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/docs/SigFlow-Plan-8.9.md) | 8.9 版本里程碑规划：Yosys/nextpnr Job 化、TraceBridge、WavePanel | ✅ 主体完成 |
| [SigFlow-TraceBridge-Design.md](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/docs/SigFlow-TraceBridge-Design.md) | TraceBridge 设计文档：影子构建复用 Yosys/nextpnr、manifest、指纹、会话目录 | ✅ |
| [SigFlow-当前工作总结.md](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/docs/SigFlow-当前工作总结.md) | 工作总结：含 decimation 待办、资源实测 | ✅ |
| [spec.md](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/docs/spec.md) | AI4EDA 双模 Agent 规划：学习版/工程版、工具链增删、DTA 三重时序验证 | 🟡 规划完成，待启动 |

### 2.3 里程碑时间线

| 阶段 | 内容 | 状态 |
|---|---|---|
| Yosys 接入 | 综合作业 Job 化、日志解析、CST 校验、引脚绑定 UI | ✅ |
| nextpnr 接入 | PnR Job 化、manifest、阶段诊断报告、fmax 解析 | ✅ |
| gowin_pack 接入 | bitstream 生成、SHA-256 校验 | ✅ |
| 调试构建复用 | DebugOverlayBuilder 影子构建，原 RTL 零修改 | ✅ |
| RTL 可综合性验证 | sf_micro_ila / sf_debug_link 经 Yosys/nextpnr 全链路通过 | ✅ |
| TraceBridge UI | 7 步 stepper、双轨波形、会话管理、AUI DockPanel | ✅ |
| 无硬件 Loopback CI | 11/11 PASS（PipeTransport） | ✅ |
| 实板闭环 | Tang Nano 9K 烧录 + 采集 + VCD | ⬜ 未完成（最大缺口） |

### 2.4 一个值得记住的教训

接入过程中踩过的最深的坑：**早期 sf_micro_ila 用分布式 RAM 方案，nextpnr PnR 后资源严重超预期**。后来切换到 BSRAM（2 blocks / 32768 bits 物理最小）才稳定。这个教训被写进了 [project_memory.md](file:///c:/Users/99228/.trae-cn/memory/projects/-e-EDA-Race-Cangku-new-SigFlow-FPGA--p2-05994a1c9034773e080b/project_memory.md) 的硬约束："BSRAM usage is fixed at 2 blocks as physical minimum for 32×1024 depth"。

它提醒我们：**开源 FPGA 工具的资源模型与商业工具有本质差异，必须以 nextpnr PnR 实测为准，不能只看 Yosys 综合后预估。**

---

## 三、暑期收官任务布置

### 任务一：AI4EDA 知识预研（全员必做）

**背景**：spec.md 已经规划了"学习模式 Agent + 工程模式 Agent"的双模架构，但当前调试架构（TraceBridge）尚未升级到能被 Agent 统一调度的程度。在调试架构升级完成、全量 Agent 启动之前，每个人需要先把 AI4EDA 的知识底座打好。

**要求**：
1. 自行了解 AI4EDA 的核心范式，至少覆盖以下关键词：
   - Agentic AI for EDA（自主式智能体，区别于"AI+EDA 辅助模式"）
   - design-to-vector（iEDA/AiEDA 提出的设计产物向量化）
   - RTL 生成（RTLLM 基准、MAGE 多智能体生成引擎）
   - 闭环修复（生成→验证→纠错→优化）
   - AI 自主等级（Synopsys L1-L5 分级）
2. 至少阅读一份以下参考资料，并写一份不超过 1 页的读后笔记：
   - iEDA arXiv 论文（2308.01857）
   - AiEDA-2.0（ISEDA'25）
   - 合见 UDA 2.0 报道（新华网 2026-03-19）
   - iChipAgent（OSCC-Project，VSCode 插件 + MCP 协议驱动 EDA）
3. 笔记里回答一个问题：**"SigFlow 的 TraceBridge 双轨首差异，能作为 Agent 反思的什么信号源？它比单纯依赖仿真报告强在哪里？"**

**交付**：8 月底前，笔记放入团队共享文档。

> 参考入口：[AI4EDA 开源项目汇总](https://github.com/jszheng/AI4EDA_readings/blob/main/opensource.md)、[iEDA 官网](https://ieda.oscc.cc/)、[spec.md 第 9.4 节根因信号源](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/docs/spec.md)

### 任务二：SigFlow 双版化（学习版 / 工程版）方案征集

**背景**：spec.md 提出了 SigFlow 拆分为"学习版"与"工程版"的构想，但具体怎么拆、界面怎么调、工具链怎么增删，需要大家贡献方案。

**要求**：
1. 每人提交一份"双版化方案建议"，至少包含：
   - 学习版与工程版的**用户画像**（分别服务谁）
   - 两个版本**工具链的差异**（哪些工具学习版该隐藏/简化，哪些工程版该补齐）
   - **界面调整建议**（学习版要不要教学引导侧栏？工程版要不要自然语言命令框？）
   - 至少一个**你自己原创的、spec.md 里没写到的**差异化点子
2. 方案不设格式，但要有论据，不要空谈。
3. 鼓励互相讨论、互相打脸——好的方案是吵出来的，不是和气出来的。

**交付**：9 月第一周前，方案汇总后统一评审。

> **TIPS** 🍉：偷偷翻了几个组员最近的工作总结，发现里面已经悄悄冒出了"学习版/工程版分流"的念头，方向感不错，看得我直拍大腿。这次干脆把窗户纸捅破——别藏着掖着了，把你脑子里那点野路子全倒出来。我特别想看到 spec.md 之外的、更新颖的玩法，比如"学习版要不要做成游戏化闯关""工程版要不要支持自然语言一句话出 bitstream"这种级别的脑洞。谁的点子最让我眼前一亮，开学会有惊喜。

### 任务三：批判性与创造性动员

这一条不是任务，是写给每个人的话。

这个夏天，我们把 SigFlow 的 FPGA 侧从"勉强能综合"推进到了"Job 化 + 调试构建复用 + RTL 全链路通过 + 无硬件 CI 11/11 PASS"。这不是小事——一年前这还是 PPT 里的愿景。

但离"好"还差得远。**实板闭环一次都没跑通**，decimation 三处不一致还没收口，输入重放差分四件套还没动，Agent 还只是 spec 里的字。

所以最后这一段，我想借三句话：

> **"The best way to predict the future is to invent it." —— Alan Kay**

我们不是在预测开源 FPGA 工具的未来，我们是在亲手造它。Yosys 和 nextpnr 是别人的轮子，但 SigFlow 把它们编排成一个能支撑教育、竞赛、调试、Agent 的平台——这件事没人替我们做。

> **"Stay hungry, stay foolish." —— Steve Jobs**

别因为 CI 全绿就觉得稳了。最大的缺口（实板闭环）还在那里，最难的差异化（TraceBridge 双轨首差异作为 Agent 反思信号）也还只是设计。要一直保持"这玩意儿真的能用吗"的饥饿感。

> **"批判是创造的前提。" —— 改自 Karl Popper**

我特别希望接下来看到的不是"好的，我去做"，而是"我觉得这个方案有问题，我有个更好的"。spec.md 里 OpenSTA 被否掉换成 DTA，就是因为有人问了"nextpnr 的时序是动态的吗"——这种问题越多越好。

**所以，请每个人在暑期收官前，至少提出一个对当前 SigFlow 架构的批判性意见，或者一个 spec.md 之外的创新点子。** 哪怕乍听很荒谬——荒谬往往是突破的起点。

---

## 四、附录：当前缺口状态（2026-08-24 更新）

### 实板闭环已跑通 ✅

2026-08-24 在 Tang Nano 9K 上完成实板采集闭环，证据存档于 [examples/tracebridge_tangnano9k/.sigflow/debug/](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/examples/tracebridge_tangnano9k/.sigflow/debug/tracebridge-tangnano9k-demo/)：

| 环节 | 状态 | 证据 |
|---|---|---|
| 烧录 + 串口枚举 | ✅ | manifest 状态机 Programming→Armed 流转 |
| 同步 + 采集 + VCD | ✅ | `capture.vcd` 含 `led[3:0]`/`state[3:0]` 计数器递增，trigger_index=0，1 秒内完成回读（≤1s 红线达成） |
| 指纹校验闭环 | 🟡 协议层通 | GET_INFO 通信成功，但 `build_fingerprint`/`bitstream_sha256` 未落盘到 manifest |

> 注：本次闭环使用**外部预烧录 bitstream**（manifest reason="Using user-programmed debug bitstream"），非 SigFlow 影子构建自动产出。

### 实板闭环暴露的两个待修问题

1. **影子构建 Yosys 路径拼接错误** 🟡 P1
   - [debug-yosys.combined.log](file:///e:/EDA_Race/Cangku/new/SigFlow_FPGA/examples/tracebridge_tangnano9k/.sigflow/debug/tracebridge-tangnano9k-demo/logs/debug-yosys.combined.log) 报 `File 'sf_micro_ila.sv' not found`
   - DebugOverlayBuilder 生成的 `run_yosys.ys` 把 `examples/` 与 `rtl/debug/` 路径拼错，需修路径解析

2. **manifest 指纹字段未落盘** 🟡 P1
   - `manifest.json` 中 `build_fingerprint`、`bitstream_sha256`、`tool_versions`、`netlist_json_path`、`pnr_json_path` 全为空
   - 说明 GET_INFO 返回的指纹未写入 session manifest，无法回溯"这份 capture 来自哪次构建"

### 当前缺口清单（更新后）

| 缺口 | 优先级 | 负责方向 |
|---|---|---|
| ~~Tang Nano 9K 实板采集闭环~~ | ✅ 已跑通 | 完成 |
| 影子构建 Yosys 路径拼接错误（sf_micro_ila.sv not found） | 🟡 P1 | 待认领 |
| manifest 指纹字段未落盘（build_fingerprint/bitstream_sha256 为空） | 🟡 P1 | 待认领 |
| 实板双轨对比（capture.vcd vs sim.vcd 首差异定位） | 🟡 P1 | 待认领 |
| decimation 字段三处不一致（契约/协议/RTL） | 🟡 P1 | 待认领 |
| 输入重放差分四件套未实现 | 🟡 P1 | 待认领 |
| nextpnr 已 Job 化但 Apicula/openFPGALoader 未独立 Job 化 | 🟢 P2 | 待认领 |
| 门级时序仿真（DTA）SDF 流程未打通 | 🟢 P2 | 待认领 |

**实板采集闭环已通，P0 验收红线解除。** 下一步重点收口两个实板暴露的 P1 问题（Yosys 路径 + 指纹落盘），让影子构建也能自动跑通实板。

---

## 五、结语

Yosys 和 nextpnr 是 SigFlow 的两条腿，这个夏天我们把它们接好了、能走了。下一步是跑起来——跑上真实板卡，跑进 Agent 时代。

各位辛苦了。期待你们更野的想法。

—— SigFlow 团队
2026-08-23
