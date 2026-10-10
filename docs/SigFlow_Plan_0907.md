# SigFlow 统一发展规划（双版本 + 跨平台）

> 版本 v1.0 综合规划 · 2026-09-07
>
> **本规划由两份内部文档融合而成：**
> - `SigFlow-Dual-Edition-Brief-0901.md`：双版本（教育版/工程版）与 Agent 化方向规划（v0.9 评审稿）
> - `SigFlow-Linux-Kylin-Portability-Research.md`：SigFlow 跑 Linux/麒麟系统的可行性调研
>
> 原两份文档作为详细技术附件保留；本文仅做工作目标、分工与时间线的统一表述。

---

## 0. 工作分工（核心变更）

本次规划把团队工作正式划分为**两条并行工作流**，共计 5 人。

| 工作流 | 人数 | 主要职责 | 主要产出 |
|---|---|---|---|
| **A. 双版本 + Agent 改造** | 3 人 | 把现有 SigFlow 拆出教育版/工程版两个产品形态；把 AI 定位为"理解、分析与辅助决策助手"，接入全硬件开发流程 | Design IR、Context Engine、FlowGraph/StageGate、错误模式库、分级提示引擎 |
| **B. Linux/麒麟适配** | 2 人 | 让 SigFlow IDE 摆脱 Windows 单一平台依赖，跑在主流 Linux 桌面与银河麒麟 V10 上 | CMake 构建、平台抽象层（进程/串口/管道）、Linux 工具链 Runtime、麒麟装机验证 |
| **共同底座** | 全员 P0 共建 | 两边的 P0 必须协同：CMake 化、Runtime 跨平台、Job 报告统一格式 | 见 §5 |

5 人具体角色建议（可在 P0 启动后微调）：

| 代号 | 归属 | 主要方向 | P0 期间交叉负责 |
|---|---|---|---|
| **A-Core** | A | Agent 骨架 + FlowGraph/StageGate 流程编排 | 与 B-Build 协同设计 Job 报告 schema |
| **A-IR** | A | Design IR + Context Engine + 工程 Agent 问答 | 与 B-Runtime 协同工具链产物接入 |
| **A-UI** | A | 层次化视图、四视图联动、错误模式与分级提示 UI | 与 B-Build 协同 IDE 跨平台界面适配 |
| **B-Build** | B | CMake 体系 + 平台抽象层 + 调试 CI 跨平台 | 担任 A 全部工作的"构建入口" |
| **B-Runtime** | B | Linux 工具链打包 + 麒麟真机验证 + 文档 | 担任 A 全部工作的"运行环境" |

---

## 1. 一句话总览

在"开源 EDA 工具链 + TraceBridge 硬件调试闭环"现有底座之上，把 SigFlow 推进为两条相互独立又共享基础设施的工作流：

- **A（产品方向）**：分化为**教育版（SigFlow Edu）**与**工程版（SigFlow Pro）**；AI 不再是"自动写代码的引擎"，而是贯穿全流程的**理解、分析与辅助决策助手**——工具执行交给既有的 Job 化框架，AI 只负责生成参数、解读结果、辅助用户决策。
- **B（部署方向）**：让整套 IDE 跑出 Windows，落地 **Ubuntu x86_64 → 麒麟 x86_64 → 麒麟 ARM64 → 龙芯 LoongArch** 的多架构支持序列。

两条工作流**共享 P0 底座**（CMake、Runtime、Job 报告、AI 骨架），并以"P0 共同完成 → P1–P3 双轨并行"为节奏。

---

## 2. 现状体检（精简）

### 2.1 项目已有的关键资产（A 复用、B 不重写）

| 资产 | 当前形态 | 在本规划中的角色 |
|---|---|---|
| **Yosys / nextpnr / 打包 / 下载 Job 化执行框架** | NextpnrJob 已 Job 化；FpgaYosysExecutor 等同款 | A 直接复用；B 改造其底层适配 Linux |
| **TraceBridge 硬件调试闭环** | 采集/双轨比较/根因/输入重放 P0–P4 全落地 | A 工程版的硬件信号源；B 改造其串口/管道部分 |
| **拖拽原理图与图码同步** | Canvas*/SFTree/VerilogStructuring 完整 | A 教育版的主入口 |
| **波形系统** | WavePanel + WaveformComparator + RootCauseGraph | A 两版共享，B 不动 |
| **插件/特性开关机制** | 已存在 | A 用作双版本分化机制（双构建目标） |
| **bundled Runtime** | `external/fpga-tools/runtime/` 下 Windows 版工具链 | B 改造为同时支持 Linux 版（同一目录布局） |

### 2.2 Windows 依赖清单（B 要解决的具体范围）

对 `main/` 静态扫描，**平台相关代码集中于 18 个文件**，分四类：

1. **进程执行**：`FpgaYosysExecutor` / `NextpnrExecutor` / `ProcessRunner` 等用 Win32 `CreateProcess` + Job Object；Linux 等价物：`fork/exec` + 进程组（`setpgid` + `killpg`），管道收日志同理。
2. **串口**：`SerialTransport` / `SerialPortEnumerator` 用 Win32 COM；Linux 等价物：`termios` + `/dev/ttyUSB*`。
3. **命名管道**：`PipeTransport`（无硬件 CI 用）；Linux 等价物：Unix FIFO 或 Unix domain socket。
4. **杂项**：WGL OpenGL 上下文、路径分隔符、文件 API；wxWidgets 自身已封装好跨平台接口，仅在调底层时需清理。

构建体系目前**只有 `SigFlow.sln`（MSVC）**，无 CMake/Makefile。

**关键有利因素**：Job 化框架已经把"调工具"隔离在 Executor/Runtime 层后面——B 的移植面是**小且有边界的**，而不是满屏 `#ifdef`。

### 2.3 底层工具链生态（B 的好基础）

Yosys / nextpnr-himbaechel / Apicula（gowin_pack）/ openFPGALoader / Verilator **全部是 Linux 原生开源项目**，与麒麟生态完全兼容：

- **YosysHQ 官方 oss-cad-suite**：linux-x64 一站式打包（Yosys + nextpnr + openFPGALoader + Verilator 版本互相匹配），与我们"捆绑 runtime"思路一致。
- **Debian/Ubuntu 软件源**：yosys、openfpgaloader 直接可用；apycula 已入 Debian 官方源（自动支持多架构）。
- **源码编译**：nextpnr-himbaechel 用 CMake `-DARCH=himbaechel -DHIMBAECHEL_UARCH=gowin` 一条命令开 Gowin 支持，依赖仅 Boost/Eigen/Python。
- **USB 烧录**：openFPGALoader 走 libusb/FTDI，配一条 udev 规则即可（比 Windows 驱动问题还少）。

**结论**：工具链层**零风险**，直接换 Linux 版本即可。Runtime 发现链（项目路径 → 捆绑 → 环境变量 → PATH）B 不需要改设计。

---

## 3. 工作流 A：双版本产品 + Agent 化

### 3.1 两个版本各自做什么

#### 3.1.1 教育版（SigFlow Edu）—— 数字电路教学

- **入口**：仍是现有拖拽式原理图设计 + 图码同步。
- **核心体验**：原理图、RTL 代码、仿真波形、综合结果**四个视图联动**——学生画一个门电路，能立刻看到对应代码、波形和上板后的资源占用。
- **Agent 角色**：**教学助手，不给答案**。学生设计过程中的典型错误（组合环、锁存器推断、位宽不匹配等）被自动识别后，Agent 通过 L1→L4 分级提示（方向性提问 → 逐步缩小范围 → 参考解法需二次解锁）引导学生自行修复。
- **远期**：教材/课程包适配（一生一芯、PA 课等），本期**只预留课程包接口**，不做实现。

#### 3.1.2 工程版（SigFlow Pro）—— FPGA/RTL 工程开发

- **设计视图**：**层次化**——顶层看系统架构，逐级展开模块直到 RTL；模块之间以**总线**连接，点击总线可查看位宽/协议/驱动端/负载端。
- **三方联动**：代码、设计图、波形之间任意跳转（点波形信号 → 定位到代码行 → 在设计图中高亮该模块）。
- **Agent 角色**：**设计理解与调试助手**。用户在设计图里点模块/信号/总线，或在波形上框选一段，直接向 Agent 提问；Agent 结合设计结构、源码、连接关系、波形数据分析作答。
- **AI "看懂"大规模设计的关键**：**Context Engine**——AI 不读整个工程源码，按用户当前关注点自动取局部结构化上下文（模块层次、连接关系、源码位置、波形切片），控制上下文规模，与可视化设计天然联动。

### 3.2 共同工作范式：AI 只关注三件事

对硬件开发全流程做"大致完善但容许选择"的编排：

```
RTL 编写 → 仿真 → 综合 → 布局布线 → 烧录调试(可选) → 结果比对 → 达标？
    ▲                                                      │
    └────────────── 未达标：修改后回环 ←────────────────────┘
```

**AI 三件事**（计划使用 MCP 与 Tools 实现工具调用）：

1. **输入**：结合用户意图生成各工具 Job 所需的参数；
2. **输出**：Job 运行后解读产物与结构化报告（如"综合有 3 个警告，其中 1 个疑似锁存器推断"）；
3. **用户干预**：每个阶段结束给出决策卡（继续 / 回退 / 改参数 / 中止），由**用户**决定下一步。

以 Yosys 为例：仿真通过、用户查看波形后决定进入综合 → AI 按提示生成综合参数 → Job 自动执行、产出网表与报告 → AI 总结异常点呈现给用户 → 用户决定继续布线还是回头改 RTL。

**边界**：AI **不直接执行工具**、**不直接改用户代码**、**不代替用户做阶段决策**，所有决策留有记录、可回溯。

### 3.3 现有系统怎么用（不推倒重来）

**界面原则：现有页面基本不动**。主窗口、编辑器、波形面板、TraceBridge 窗口保持现状；新能力一律以"新增面板/浮层"形式叠加（层次视图、Agent 对话面板、决策卡、提示卡均为新增组件），不重构现有页面。工程版默认不出现门级原理图相关界面，教育版不出现工程版专属面板——靠**特性开关**控制可见性。

**新增的只有四块**（均为新增模块，不是改造）：
- **Design IR**（设计中问表示）：支撑层次视图与三方联动；
- **Context Engine**（局部上下文引擎）：让 AI 不读全工程、点谁查谁；
- **FlowGraph + StageGate**（流程编排与决策门）：把"流程即数据"；
- **教育版错误模式库与分级提示引擎**：复用现有 5 类探测器（TreeSitterLinter、Design IR 静态分析、Yosys 警告码、Wave 分析、WaveformComparator）。

---

## 4. 工作流 B：Linux/麒麟适配

### 4.1 现状定位

调研结论：**可行，且比预想的乐观**。真正的瓶颈不在工具链（工具链 Linux 原生），而在 SigFlow IDE 自身约 18 个文件的 Windows API 依赖和 MSVC 构建体系——这部分清晰、有限、可逐步替换。

### 4.2 分架构评估

| 架构 | 工具链 | SigFlow IDE | 总体评估 |
|---|---|---|---|
| **x86_64**（Intel/AMD/兆芯/海光） | oss-cad-suite linux-x64 直接可用 | wxWidgets/GTK 齐备 | 🟢 **低风险，优先落地** |
| **ARM64**（飞腾/鲲鹏） | yosys 有发行版包；nextpnr/gowin_pack 需源码编译；apycula 纯 Python 无架构限制 | wxWidgets 支持 ARM64 Linux | 🟡 **可行，多一道"自编工具链"工序** |
| **LoongArch**（龙芯） | 社区有 yosys 先例（Drink-EDA），但 nextpnr-himbaechel + Gowin chipdb 无公开验证；有 x86 二进制翻译兜底（性能损失约 20%） | 理论可行 | 🔴 **不确定，需实机验证** |

### 4.3 三步走路径

**第一步（验证性，1–2 周）**：在 Ubuntu x86_64 上跑通命令行级全流程——下载 oss-cad-suite，用 `examples/tracebridge_tangnano9k` 的同一套参数（Yosys 脚本、nextpnr 参数、gowin_pack 命令原样照搬）在 Linux 上综合 → 布线 → 打包 → 烧录 → UART 采集。**这一步不需要改任何 SigFlow 代码**，就能证明"底座"完全成立。

**第二步（IDE 移植，1–2 个迭代）**：
   1. 新建 CMake 构建体系（先 Linux，可反哺 Windows，摆脱单一依赖）；
   2. 抽薄薄一层平台抽象（进程/串口/管道约 4 个接口，POSIX 实现）；
   3. 路径分隔符与文件 API 清理；
   4. 调试 CI 从 PowerShell 改为跨平台脚本（bash）。

**第三步（麒麟落地）**：x86_64 麒麟装机验证（与 Ubuntu 无本质差异）；ARM64 麒麟需在目标机源码编译工具链并捆绑；龙芯视实机测试结果决定投入。

---

## 5. 双工作流的衔接与共同底座

两条工作流不是孤立推进，有**三个明确的衔接点**：

### 5.1 共同底座（P0 必须一起建）

| 底座项 | 主导 | 配合 | 说明 |
|---|---|---|---|
| **CMake 构建体系** | B-Build | A-Core | 落点：摆脱单一 MSVC，构建脚本既能编 Windows 也编 Linux |
| **平台抽象接口**（进程/串口/管道/路径） | B-Build | A-IR | A 的工具调用都走这层抽象；以后所有 Job 类只调接口不碰系统调用 |
| **Linux bundled Runtime**（`external/fpga-tools/runtime/linux-x64/`） | B-Runtime | A-IR | 与 Windows 版并列，Runtime 发现链自动适配 |
| **JobReport 统一 schema** | A-Core | B-Build | A 的 AI 解读报告与 B 的"工具跑通"都依赖它；先于 P0 锁定 |
| **Agent 骨架**（先接 Mock） | A-Core | 全部 | 调试 StageGate 闭环 |

### 5.2 衔接关键路径

```
                    ┌─────────────────────┐
                    │  P0：共同底座（全员） │
                    └──────────┬──────────┘
                               │
              ┌────────────────┼────────────────┐
              ▼                                 ▼
   ┌──────────────────────┐         ┌──────────────────────┐
   │  工作流 B：Linux 适配  │         │  工作流 A：双版本+Agent │
   └──────────────────────┘         └──────────────────────┘
              │                                 │
              ▼                                 ▼
   Ubuntu x86_64 跑通示例工程     Design IR + 层次视图 + Context
   （证明底座成立）              Engine + FlowGraph + 分级提示
              │                                 │
              └──────────────┬──────────────────┘
                             ▼
                 P3 / 麒麟：Linux 上跑工程版教学示例
                （双工作流首次"对撞"——同时上 A 端能力与 B 端部署）
```

**关键对撞点**：B 的 Ubuntu x86_64 跑通示例工程（§4.3 第一步）应**早于** A 的 P3 流程编排验收——它证明"Job 化框架 + 工具链"在 Linux 上是事实成立的，对外汇报极强；也是"Linux 上跑工程版教学示例"的物理基础。

### 5.3 共同验证矩阵

| 验证项 | 主导 | 形式 | 时间 |
|---|---|---|---|
| **命令行级全流程跑通**（综合→布线→烧录→采集） | B-Runtime | 在 Ubuntu 上跑示例工程 | P0 末 |
| **GUI 全流程跑通**（同 + IDE 联动） | B-Build | 在 Ubuntu 上启 IDE 走一遍 | P1 末 |
| **麒麟 x86_64 装机验收** | B-Runtime | 在麒麟机器上重复 GUI 验证 | P2 末 |
| **示例工程 AI 问答闭环** | A-IR | 在 IDE 中点总线/信号提问 | P3 末 |
| **教学示例分级提示闭环** | A-UI | 教学示例上"学生犯错→分级引导→自行修复" | P4 末 |

---

## 6. 统一路线图

把两条工作流合并为一张时间线（**P0 全员共建；P1–P3 双轨；P4–P5 各自延伸**）：

| 阶段 | 工作流 A（双版本 + Agent） | 工作流 B（Linux/麒麟） | 共同产出 / 里程碑 |
|---|---|---|---|
| **P0 底座统一**（全员，1–2 迭代） | 仿真/烧录 Job 化 + JobReport schema + Agent 骨架（先 Mock）+ 双版本特性开关 | CMake 体系 + 平台抽象层 + Linux bundled Runtime + Ubuntu 命令行级全流程跑通 | 全流程以 Job 形式跑通，报告可程序化读取；Ubuntu 示例工程零代码改动跑通 |
| **P1** | Design IR + 层次化视图（先两级展开可演示） + 总线面板 + 三方联动 | IDE 在 Linux 上 GUI 全流程跑通；CI 加 Linux job | 打开示例工程可从顶层逐级展开到 RTL，点总线看属性；同时在 Ubuntu 上跑 |
| **P2**（并行启动） | Context Engine + 工程 Agent 问答（点谁问谁、框选提问） | 麒麟 x86_64 装机验证；ARM64 工具链源码编译启动 | 示例工程上完成真实设计问答与报告解读；麒麟 x86_64 装机验收 |
| **P3** | FlowGraph + 决策门，Agent 贯穿全流程 | ARM64 麒麟落地；macOS 可选 | RTL→仿真→综合→布线→调试→回环全流程带决策卡走通（在 Linux 上） |
| **P4** | 教育版核心：四视图联动强化 + 错误模式库 + 分级提示 | 龙芯 LoongArch 实机验证（视资源决定是否做） | 教学示例完整演示"学生犯错 → 分级引导 → 自行修复" |
| **P5 远期** | 教材/课程包适配、多板卡支持 | 龙芯工具链迁移与二进制翻译兜底 | 按需立项 |

P1/P2 中 A 与 B 双轨并行；A 的 P3 须在 B 已跑通 Linux 后才能完整验收；P4 不阻塞 P3，可独立推进。

---

## 7. 风险与对策（合并）

| 风险 | 类别 | 对策 |
|---|---|---|
| **LLM 分析硬件设计的效果不确定** | A | P2 先 Mock + 规则模板打通链路，效果验证后接真实模型；用户决策门保留 |
| **层次化视图 + Design IR 工作量大** | A | 严格分阶段，P1 先支持两层展开即可演示 |
| **AI 输出不可靠** | A | 强制用户确认后再执行，从"自动闭环"改为"用户干预、AI 执行" |
| **MSVC → CMake 迁移兼容性问题** | B | CMake 同时产出 Windows / Linux 两个目标；保留 .sln 一段时间作回归对照 |
| **wxWidgets 在麒麟的 GTK 主题兼容性** | B | P1 早期在 Ubuntu/麒麟真机做主题适配；不一致处靠 wxWidgets 主题切换规避 |
| **ARM64 工具链编译耗时长且不可重现** | B | oss-cad-suite 优先；若失败则固定 commit + 写死 Docker 镜像作为 Runtime 备份 |
| **LoongArch 无公开 nextpnr-himbaechel 验证** | B | 先申请一台实机做最小化验证（仅 yosys + 仿真）；不轻易承诺时间 |
| **双工作流 P0 资源争抢**（共同底座） | 共同 | B-Build 的 CMake 作为"基线"先跑通；A-Core 的 JobReport schema 与之并行；P0 期间每周 1 次 5 人同步 |
| **教育版与工程版数据模型冲突** | 共同 | 抽象出共享 IR（Design IR 不带"教育/工程"标签，靠特性开关切换可见字段） |

---

## 8. 待评审/决策清单（合并）

以下问题需在 P0 启动前明确，否则会阻塞后续推进：

### 8.1 来自双版本规划（Brief-0901）

1. **Agent 定位**：从"自动闭环修复"改为"用户决策干预 + AI 执行修复"（copilot 而非 autopilot）——是否接受？
2. **双产品 vs 单产品双模式**：教育/工程分化已深入到设计单元与数据模型层面，我们倾向在工程版。是否最终确认做"双产品（双构建目标）"？
3. **优先级**：先做工程版（P1–P3）还是教育版（P4）？当前排布默认工程版优先。
4. **范围控制**：P5 教材适配本期只留接口。

### 8.2 来自 Linux/麒麟调研

5. **目标机器架构**：装机单位是 x86_64 还是 ARM64 还是龙芯？这直接决定 B 的优先级与分支。建议**先问导师/装机单位**。
6. **CI 策略**：是否把 GitHub Actions Linux job 作为 P0 验收一部分？默认建议加。
7. **构建体系去重**：CMake 化是否反哺 Windows（即 CMake 替代 `.sln`），还是仅 Linux？默认建议"先并行一段时间，P1 末再做选择"。

### 8.3 新增（融合后冒出的）

9. **P0 期间 5 人工作排期**：B-Build 的 CMake 与 A-Core 的 JobReport schema 谁先谁后？建议 B-Build 先（构建入口决定一切），A-Core 在 CMake 框架内并行写 JobReport。
10. **教育版错误模式库是否复用现有检测器**：建议全部复用（5 类探测器都在 `main/` 现有代码中），避免新增采集链路。

---

## 9. 参考与原始文档

- `SigFlow-Dual-Edition-Brief-0901.md`：双版本规划 v0.9 评审稿（详细技术展开见其完整版 `SigFlow-Dual-Edition-Plan-0831.md`）
- `SigFlow-Linux-Kylin-Portability-Research.md`：Linux/麒麟可行性调研（详细技术依据）
- `docs/spec.md`：AI4EDA 双模 Agent 早期规格（被本规划在 copilot 化、RTL 生成降级、AI 经 Context Engine 结构化查询等处修订）
- `docs/SigFlow-FPGA-Progress-Since-July.md`：七月以来 SigFlow FPGA 侧进展（提供 A 工作流复用的资产盘点）
- `docs/TraceBridge-Overlay-Repair-Draft.md`：硬件调试层修复草案（A 工作流工程版硬件信号源的状态）
- `ChangeLog.md`：7 月以来提交记录（佐证当前能力边界）

---

## 10. 附录 A：双模式 Agent 前期工作 TODO List

> 适用范围：**工程版 Agent（设计理解/调试助手）与教育版 Agent（教学助手）共享内核的前期建设**，覆盖 P0 底座 → P2 两条最小闭环（示例工程问答闭环 / 分级提示闭环）。P3+ 仅列接口预留。
> 编制口径：以本规划对 `docs/spec.md` 的修订为准（copilot 而非 autopilot；AI 只碰"输入参数 / 输出解读 / 用户干预"三件事；AI 不直接读全工程源码，一律经 Context Engine）。
> 行文约定：每条标 `[阶段 · 责任角色]`；`[涉B]` 表示与 Linux/麒麟工作流 B 有交叉，需同步设计。角色代号见 §0。
> 排序原则：**数据结构先行 → 后端服务 → 前端消费**；同一行内若拆子任务，先做 schema 再做实现。

### 10.1 D0 决策与基线冻结（开工门槛，先于一切编码）

- [ ] **D-01 内核约束文档** `[P0 · A-Core]`——把 §3.2"AI 三件事"落成一份《Agent 内核约束》（约 2 页）：AI 不直接执行工具、不直接改用户代码、不代替用户做阶段决策、所有决策留痕。作为后续所有模块评审的 checklist。
- [ ] **D-02 工具调用形态选型** `[P0 · A-Core 主导，A-IR/A-UI 参与]`——在"进程内 Tool/Function 注册表（Job 即工具）"与"MCP 客户端嵌入"之间做评估矩阵并拍板（考虑：wxWidgets 桌面进程内线程模型、Job 系统已有取消/审计、远端 LLM 需要 tools schema、B 流跨平台约束）。**默认建议：第一版用进程内注册表，MCP 作为第二版适配层**。
- [ ] **D-03 LLM Provider 策略** `[P0 · A-Core]`——抽象 `LLMProvider`，首批实现：OpenAI 兼容 REST 端点 + `MockProvider`（规则引擎，离线可跑，见 X-01）。API Key 不入库、不入 `.sigflow/`，走应用配置引用（环境变量/系统凭据）。明确无 Key 时的降级体验（教学核心不依赖 LLM，工程问答降级提示）。
- [ ] **D-04 最小演示场景冻结** `[P0 · 全员]`——白纸黑字写死两个验收场景：①工程版"在 `examples/tracebridge_tangnano9k` 上点选一个总线提问 + 请求解读最近一次 Yosys 报告 → Agent 给出结构化回答与决策卡"；②教育版"构造一个含锁存器推断的迷你工程 → 走完 L1→L4 分级提示且不直接给答案"。后续一切排期以能否支撑这两幕为准。
- [ ] **D-05 模式差异表** `[P0 · A-Core]`——对照 spec §2/§5/§6 输出教育/工程两模式差异表：工具白名单、安全边界、默认交互粒度、可见面板集、系统提示词。本规划已收敛的部分（教育给引导不给答案、工程点击/框选即问、实板烧录门控）写死，其余开放项列 TODO。
- [ ] **D-06 教育提示分级标准** `[P0 · A-IR + A-UI]`——冻结 L1→L4 语义（方向性提问 → 缩小范围 → 具体线索 → 参考解法需二次确认）与"哪些错误类型允许到 L4"的门控规则，先于错误模式库编码。
- [ ] **D-07 目录与命名基线** `[P0 · A-Core]`——冻结 `.sigflow/agent/` 子树布局（见 U-03）与新增模块的目录归属（`main/agent/`、`main/ir/`、`main/context/`、`main/flow/` 是否新建，与现有 `main/debug`、`main/wave`、`main/fpga` 平级），避免代码散落。

### 10.2 数据结构与 Schema（统一化之根，最先动工）

- [ ] **DS-01 JobReport 统一 schema** `[P0 · A-Core 主导，涉B]`——一份 JSON Schema（v1）覆盖全部工具 Job：`jobType / id / 状态机阶段 / 起止耗时 / exitCode / 产物清单(路径+哈希) / 错误数组[]`；错误数组元素含 `code / 严重度 / 阶段 / IR坐标(可选) / 摘要 / 原始日志行号`。这是 AI 解读报告与错误探测器的共同输入，先于 B-05。
- [x] **DS-02 Job 参数 schema 规范** `[P0 · A-Core]`——为每个 Job 类配一个 input 声明（参数名/类型/必填/默认/约束/枚举说明）。双重用途：①AI 生成参数的唯一依据（避免幻觉参数）；②未来 UI 自动表单来源。*（已实现：`main/jobs/JobRegistry.cpp` 中每个描述符带 `inputSchema`，`JobServiceRegistry::ValidateParameters` 在提交前校验必填/类型/枚举）*
- [ ] **DS-03 Design IR 四件套数据模型** `[P1 · A-IR]`——①HIR：源码层（SFTree 快照 + 逐符号 source span）；②NIR：网表层（Yosys JSON 解析结构，含 cell/wire/conn 索引）；③Binding：模块↔网表↔波形↔源码 span 四向映射；④BusDecl：总线声明（位宽/协议/驱动端/负载端，命名推断 + 用户标注覆盖）。先定义 JSON 形态与 C++ 结构，再写构建管线（B-10）。
- [ ] **DS-04 Context Engine 查询协议** `[P1 · A-IR]`——冻结 8 个查询 API 的请求/响应结构（GetModuleCard / GetBusCard / GetFanoutCone / GetDriverLoads / GetSourceSpan / GetWaveSlice / GetJobDigest / GetDiffAnchors），每个响应带 `token 预算字段` 与 `数据新鲜度`。前端点选、Agent 工具、波形框选共用同一协议。
- [ ] **DS-05 FlowGraph / StageGate JSON** `[P2 · A-Core]`——流程即数据：节点（JobRef + 产物 + 决策门）、有向边、回退/跳过语义；`decisions.jsonl` 行格式（时间 / 节点 / 选项集 / 用户选择 / 依据 / 关联 JobId 链）。P2 前只做 schema + 校验器，不实现编排器（P3）。
- [ ] **DS-06 Agent 消息/会话协议** `[P0 · A-Core]`——会话文件与面板消息统一结构：`system/user/assistant/tool_call/tool_result/decision_card/hint_card` 等消息类型；卡片一律结构化数据（标题/正文/选项/依据/关联JobId/可渲染类型），**不传纯文本让前端硬排版**（呼应 U-06）。
- [ ] **DS-07 教育错误模型** `[P2 预研 · A-IR]`——规则 JSON：`模式名 / 探测器类型(五选一) / 严重度 / 触发上下文(IR坐标模板) / L1-L4 文案模板 / 参考解法(门控) / 课程包override槽位`。先出 5–8 条高频规则样例（组合环/锁存器推断/位宽不匹配/未复位寄存器/多驱动），随 P4 扩充到 15–20。
- [ ] **DS-08 记忆与画像数据** `[P2 · A-Core]`——短期会话 `jsonl`（Job 链+最近错误）、项目长期 profile（偏好/修复模式）、学习者画像（概念掌握/错误频次）字段集；只定义写接口与存储格式，算法留 P3+。
- [ ] **DS-09 概念词典 schema（预留）** `[P2 预留 · A-IR]`——教学概念卡数据格式（术语/定义/示意图引用/关联概念/难度），本期只落 schema + ≥5 条样例词条，不建服务。

### 10.3 后端（执行底座补齐 → Agent 内核 → 上下文服务）

**Job 化补齐（Agent 可调度的前提，全部复用 `FpgaSynthesisJob`/`NextpnrJob` 的状态机范式）**

- [x] **B-01 SimJob** `[P0 · A-Core]`——把 `main/Simulation/` 的 `VerilatorRunner` + `SimulationEngine` + `ProcessRunner` 包成 Job（含 manifest、取消、超时、VCD 产物登记）。教育"一键仿真"与工程回归共用。*（已实现：`main/jobs/ToolJobs.cpp` 的 `SimulationJob`，进程内 `runner` 钩子接 `SimulationEngine`，GUI 的编译/运行入口已改走 SimJob）*
- [x] **B-02 PackJob** `[P0 · A-Core]`——`main/fpga/FpgaPackService` 由"服务"升级为 Job（补状态机/manifest/哈希记账，沿用现有 SHA-256 记账逻辑）。*（已实现：`PackJob` 复用 `FpgaPackService::ValidateInput/Finalize`，GUI 打包入口已改走 PackJob）*
- [x] **B-03 FlashJob** `[P0 · A-Core]`——openFPGALoader 下载 Job 化，带 `requireConfirm` 门控标志（工程模式弹确认，教育模式默认禁用实板）。*（已实现：`FlashJob`，GUI 烧录入口已改走 FlashJob）*
- [x] **B-04 JobService 注册表与调度** `[P0 · A-Core]`——按 JobType 注册/发现/运行/取消/超时/并发上限；**AI 层只拿 JobType + 参数对象，不碰任何路径与命令行**（U-07 的前提）。*（已实现：五类 Job 全部注册（sim/synthesis/pnr/pack/flash），`JobServiceRegistry::Run` 是唯一执行入口，参数按 DS-02 schema 校验，并发上限/取消/超时生效）*

**Agent 内核与工具面**

- [ ] **B-05 报告解析统一** `[P0 · A-Core]`——现有 `FpgaYosysLogParser` / `NextpnrLogParser` 与后续 sim/pack/flash 解析器全部输出 DS-01 的 JobReport；新增"摘要提取器"把报告压成 AI 可读的结构化要点（资源、fmax、警告清单、错误清单）。
- [ ] **B-06 LLM 网关** `[P0 · A-Core]`——`LLMProvider` 接口 + OpenAI 兼容实现（流式 + 可中断 + 超时）+ `MockProvider`；统一 token 计数与预算切断（防跑飞）。
- [ ] **B-07 Agent 内核（copilot 口径）** `[P0 · A-Core]`——意图解析 → 参数补全（依 DS-02，禁止幻觉参数）→ Job 调度 → 结果解读 → 决策卡生成 → 审计写入。**明确不做自动修复循环**；"失败 → 建议下一步"由决策卡交回用户。会话状态机参照 spec §10.1 但去掉 autopilot 回环。
- [ ] **B-08 Tool/Function 注册表** `[P0 · A-Core 主导，A-IR 参与]`——把 Job（B-01..04）与 IR/波形查询（B-09/B-15）注册为 AI 可见工具，每工具带 `name/描述/input JSON schema(依DS-02/DS-04)`；D-02 若选 MCP 则此注册表即 MCP 工具清单来源。
- [ ] **B-09 Context Engine 服务** `[P1 · A-IR]`——实现 DS-04 的 8 个查询 API；索引缓存 + 失效策略（源码 mtime / Job 版本 / 工程重载）；每条查询返回的上下文包带 token 预算与裁剪策略（**AI 不读全工程源码的落点**）。
- [ ] **B-10 IR 构建管线** `[P1 · A-IR]`——HIR（加载 SFTree/`tree-sitter` 源文件索引）→ NIR（复用现有 Yosys JSON 产物/或按需触发综合）→ Binding（对齐模块/信号/span）→ BusDecl 推断；支持增量更新与"工程变更后失效重建"。

**上下文接入与探测**

- [ ] **B-11 错误探测器管线** `[P2 预研 · A-IR]`——五类探测器（TreeSitter 静态 / IR 结构 / Yosys 警告码 / WaveAnalysis / WaveformComparator）输出统一"发现事件"（含 IR 坐标 + 严重度），供工程解读与教育错误库（DS-07）共用，**避免为教育另建一套采集**。
- [ ] **B-12 决策审计与回放** `[P0 · A-Core]`——写入 `decisions.jsonl`（DS-05 行格式）并能反向还原某次 Agent 会话的完整动作链（哪个提问 → 取了什么上下文 → 调了哪个 Job → 用户选了哪项）。
- [ ] **B-13 TraceBridge 上下文接入** `[P1 预研 · A-IR]`——把调试会话产物结构化：采集元数据、双轨首差异点、`RootCauseGraph` 依赖链、波形切片，作为工程 Agent 问答的硬件信号源（先只做"可查询"，不做自动根因）。
- [ ] **B-14 SelectionService** `[P1 · A-IR 主导，A-UI 参与]`——统一"点选/框选 → 上下文包"：设计图点模块/信号/总线、波形框选、Job 报告点错误，都归一成同一个"选择上下文"结构（内部即 DS-04 查询的入参）。
- [ ] **B-15 波形切片 API** `[P1 · A-IR + A-UI]`——在 `main/wave/`（WavePanel/WaveAnalysis/WaveCompareHub）上提供"时间窗 + 信号集 → 波形切片 JSON"，供框选提问（F-07）、比较器与 TraceBridge 双轨复用。

### 10.4 前端（严格遵守"不改现有页面、一律新增面板/浮层"）

- [ ] **F-01 模式切换器** `[P0 · A-UI]`——顶部"教学 | 工程"分段控件；切换只改面板可见集与 Agent 提示词，**不销毁工程状态**（spec §7.1）。
- [ ] **F-02 Agent 对话面板** `[P0 · A-UI]`——AUI DockPanel：消息流 + 结构化卡片渲染（markdown/代码块/报告摘要/决策卡）+ "思考中"指示 + 可中断。两模式共用同一组件，仅系统提示词与可用工具不同。
- [ ] **F-03 决策卡组件** `[P0 · A-UI]`——渲染 DS-06 decision_card：选项（继续/回退/改参数/中止）+ 依据摘要 + 关联 Job 链；用户点击即写 decisions.jsonl。
- [ ] **F-04 提示卡组件（教育）** `[P2 预研 · A-UI]`——L1→L4 渐进展开、L4 二次确认、不直接给答案；与错误探测器/规则库联动。
- [ ] **F-05 自然语言命令框** `[P1 · A-UI]`——顶部命令框（工程模式显示），口语指令入口；输入先进意图解析，命中"执行类"先出拟执行方案卡待确认。
- [ ] **F-06 设计图点选提问** `[P1 · A-UI + A-IR]`——在 Canvas 上把"点击模块/信号/总线"接到 B-14 SelectionService → 生成上下文包 → 送 Agent；总线点击同时弹 BusDecl 信息浮层（工程版）。
- [ ] **F-07 波形框选提问** `[P2 · A-UI]`——WavePanel 框选手势 → B-15 切片 → 提问/比对。
- [ ] **F-08 层次化视图骨架（工程版）** `[P1 · A-UI]`——顶层架构图 → 逐级下钻模块 → 直至 RTL；先支持两级展开（§7 风险对策已承诺）。
- [ ] **F-09 总线信息浮层** `[P1 · A-UI]`——点击总线展示位宽/协议/驱动端/负载端（数据来自 DS-03 BusDecl）。
- [ ] **F-10 Job 队列面板** `[P1 · A-UI]`——所有 Job（含新增 Sim/Pack/Flash）统一排队与状态可视化；教育版只读简化。
- [ ] **F-11 执行流水线可视化** `[P2 · A-UI]`——底部流程节点（RTL→仿真→综合→P&R→打包→调试）；工程版显示完整状态，教育版带讲解卡（数据来自 FlowGraph schema，编排器 P3 接）。
- [ ] **F-12 报告摘要呈现** `[P1 · A-UI]`——JobReport 摘要卡片 + 风险高亮（警告/错误行可点击跳 IR 坐标）。
- [ ] **F-13 异步线程模型** `[P0 · A-UI]`——Agent 全链路（LLM 流式/Job 轮询/IR 构建）走后台 worker + `wxThreadEvent` 回 UI（沿用 TraceBridge 现有约定），**UI 线程零阻塞**；新增面板全部遵循。
- [ ] **F-14 概念卡浮层（预留骨架）** `[P2 预留 · A-UI]`——悬停触发、数据来自概念词典（DS-09），本期只做组件骨架与 ≥5 词条演示。

### 10.5 工具链

- [x] **T-01 AI 可调工具面收编** `[P0 · A-Core]`——确认"AI 可调 = Job 化工具（B-01..04）+ 查询工具（B-09/B-15）+ 决策（B-12）"，其余一律不暴露；**防止 AI 绕过 Job 直接调外部命令**。*（已实现：五类 Job 经 `JobServiceRegistry::Run` 统一执行；残留：参数中的 `executable` 仍需由 Runtime 发现链在服务端填充，勿让 AI 直接指定）*
- [ ] **T-02 新工具引入评估** `[P0 决策/P1 试点 · A-Core]`——Verible（lint，教学即时提示 + 工程风格门控，做 LintJob）列为优先试点；Icarus/cocotb/SymbiYosys/功耗估算仅出评估结论，**本期不承诺接入**（与 §8.2/§6 范围控制一致）。
- [ ] **T-03 LLM 客户端跨平台** `[P0 · A-Core，涉B]`——HTTP 层不依赖 Win32（可用 wxWidgets 自带或可移植库）；与 B-Build 平台抽象层交叉确认，避免在 B 流再返工。
- [ ] **T-04 工具链路径抽象确认** `[P0 · A-Core，涉B]`——验证"Runtime 发现链已被 Job 封装，AI/新面板拿不到也不该拿到 exe 路径"成立；不成立则补一层。
- [ ] **T-05 MCP 适配层（视 D-02）** `[P2 待定 · A-Core]`——若选型引入 MCP：客户端嵌入（进程内线程模型评估）、工具清单映射、鉴权/超时治理；不引入则关闭本条。

### 10.6 统一化（跨工具、跨模式、跨平台）

- [ ] **U-01 Job 状态机对照表** `[P0 · A-Core]`——synth/pnr/pack/flash/sim 五类 Job 状态语义逐一对齐（现有 `FpgaSynthesisJob` 状态集合为基准），取消/超时/产物校验行为一致。
- [ ] **U-02 错误码体系** `[P0 · A-Core]`——`阶段前缀(SYN/PAR/PACK/FLASH/SIM/DBG/AGENT/EDU) + 编号 + 严重度`，统一进 DS-01 错误数组；跨工具可溯源、可被探测器/AI/UI 三方消费。
- [ ] **U-03 `.sigflow` 扩展规范** `[P0 · A-Core]`——`fpga/runs`、`debug/<session>` 保持不动；冻结新增：`agent/{memory,plans,repairs,concepts}`、`snapshots/`、`ci/`（沿用 spec §10.2 草案并裁剪到本期范围）。
- [ ] **U-04 配置中心** `[P0 · A-Core]`——应用级配置：当前模式、feature flags（§3.3 特性开关）、LLM endpoint/Key 引用；与现有工程配置（`.sigflow`/工程文件）分层，避免 Key 落盘工程。
- [ ] **U-05 日志与审计统一** `[P0 · A-Core]`——沿用 7 色日志约定；Agent 动作链单列审计（decision 流水线），与终端日志互补不混淆。
- [ ] **U-06 数据驱动渲染约定** `[P0 · A-UI]`——新面板只消费结构化消息/卡片（DS-06/DS-07），文案模板集中在数据侧，前端不做规则判断写死话术（为 L1-L4 多语言/多课程包留路）。
- [ ] **U-07 可追溯性闭环** `[P0 · A-Core]`——"AI 动作 ↔ JobId ↔ decisionId ↔ 产物哈希"四方关联（复用力链：产物哈希可查、决策可回放、Job 可重跑）。
- [ ] **U-08 双模式数据一致性** `[P0 · A-Core]`——模式切换不重建工程、共享同一 `.sigflow/`；教育快照不影响工程 Job 链；工程修复记录可选转教学案例（spec §11.10，本期只留数据标记）。

### 10.7 测试、验收与里程碑

- [ ] **X-01 Mock 先行闭环** `[P0 · A-Core]`——无 API Key、用 `MockProvider` 规则引擎跑通"提问 → 取上下文 → 结构化回答 → 决策卡 → 审计落盘"全链路（CI 可跑，离线可演示）。
- [ ] **X-02 单元/smoke 套件** `[P0 起随建 · 各责任角色]`——IR builder、Context Engine 8 查询、错误探测器、schema 校验（DS-01/DS-02 用 JSON Schema 校验器）、decisions 写读回放；沿用 `tests/` 现有 smoke 组织方式。
- [ ] **X-03 工程版验收场景（=D-04 ①）** `[P2 · A-IR + A-UI]`——在示例工程上：点选总线提问 + 请求解读最近 Yosys 报告 → 结构化回答 + 决策卡 → 选"继续"能触发下一 Job。
- [ ] **X-04 教育版验收场景（=D-04 ②）** `[P2 预研 · A-IR + A-UI]`——构造含锁存器推断的迷你工程 → 五类探测器命中 → L1→L4 分级引导，学生可自行修复，全程 AI 未直接给答案。
- [ ] **X-05 既有回归保持绿** `[P0 起持续 · 全员]`——`DebugOverlaySmoke` / `DebugP0Smoke` / `TraceBridge` 系列 smoke 不因新增模块回归（新代码默认不触碰既有文件，除非本清单明示）。
- [ ] **X-06 与 B 流联合验证** `[P2 · 全员，涉B]`——在 B 流 Ubuntu x86_64 环境上复跑 X-03 场景（0907 的对撞点验收），同时暴露 LLM 网关与工具链发现的跨平台问题。
- [ ] **X-07 阶段交付物** `[各阶段末 · 全员]`——P0：五类 Job + Mock 闭环可演示 + 全部 schema 冻结；P1：两级层次视图 + Context 8 查询可用；P2：X-03/X-04 两幕录屏 + 决策审计样例 + 内核约束文档评审通过。

### 10.8 P3+ 接口预留（只留扩展点，不动工）

| 预留项 | 扩展点位置 | 触发条件 |
|---|---|---|
| FlowGraph 编排器（StageGate 自动推进） | DS-05 schema + B-07 内核决策接口 | 工程版两幕验收通过后 |
| 自动闭环修复（受限） | B-07 中"失败建议"升级为"预算内自动重试"，仍需用户门控 | 明确接受 copilot→受控 autopilot 后 |
| 教学案例自动生成 | U-08 修复记录数据标记 → 转 DS-07 规则案例 | 教育版 P4 立项 |
| 多方案对比 / 设计空间探索 | JobService 并行调度（B-04）预留并发上限字段 | 工程版 P3 立项 |
| 概念词典服务 / 知识图谱 | DS-09 schema + F-14 骨架 | 教育版 P4 立项 |
| 龙芯/多板卡支持 | 与 B 流共用 Runtime 发现链（T-04） | B 流麒麟验证通过后 |

---

## 11. 实施记录（2026-09-09）

> 一句话：**后端 Job 化清单 B-01~B-04 已落地并接入 GUI**，五类 Job（仿真/综合/布线/打包/烧录）走同一套状态机与报告格式；详细留痕见 `docs/SigFlow-Job化-评审报告.md`。

**已完成**

- **B-01~B-04**：`SimulationJob`（外部进程 + 进程内 `SimulationEngine` 两种运行体）、`PackJob`（复用 `FpgaPackService` 记账）、`FlashJob`（`requireConfirm` 门控）、`SynthJob`/`PnRJob`（复用既有日志解析器与产物校验器）；`JobServiceRegistry::Run` 是唯一执行入口，含参数 schema 校验、并发上限、有效取消、超时。
- **GUI 接入**：打包 / 烧录 / 仿真编译 / 仿真运行 四处入口改走 Job，终端输出、进度条、取消、报告回显保持原样。
- **平台层**：新增 `PlatformProcess`（Win32 管道 + Job Object 杀进程树；POSIX fork/exec/killpg 实现待真机验证）与可移植 `Sha256`，Job 层不再依赖 `windows.h`/`BCrypt`。
- **配套**：默认超时 仿真/综合/布线 600s、打包/烧录 300s（可参数覆盖）；`.sigflow/jobs/<type>/<job-id>/` 布局冻结进 `spec.md` §10.2；`tests/jobs/` 新增假工具 + 冒烟，CI 全绿。

**未完成**

- B-05 报告解析统一（既有 Yosys/nextpnr 解析器改输出 DS-01 JobReport）、U-01 既有 Job 与统一 Job 的逐项对照/合并
- GUI 的综合/布线仍走既有异步流程（迁移是独立工程）
- POSIX `PlatformProcess` 真机编译验证、`executable` 改由 Runtime 发现链服务端填充、F-10 Job 队列面板

---

*本规划是工作目标、分工与时间线的统一表述；具体技术细节请回溯各原始文档。*