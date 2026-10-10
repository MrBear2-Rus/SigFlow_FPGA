# SigFlow 双版本（教育版 / 工程版）开发规划

> **文档版本**：v1.0
> **日期**：2026-08-31
> **作者**：SigFlow 架构组
> **状态**：Draft，待评审
> **关联文档**：`spec.md`（双模 Agent 规格 v1.0）、`SigFlow-Plan-0823.md`、`SigFlow-Plan-8.9.md`、`SigFlow-TraceBridge-Design.md`、`SigFlow-当前工作总结.md`、`WavePanel-Upgrade-TODO.md`
> **定位**：本文是 Plan-0823 中布置的"双版化方案"的正式答卷，将 `spec.md` 的"双模 Agent（同一产品两种模式）"升级为"双产品形态（教育版 / 工程版）"，并对 Agent 定位做出重要修订。

---

## 0. 摘要

SigFlow 后续分化为两个产品方向：

- **教育版（SigFlow Edu）**：面向数字电路教学。以拖拽式原理图设计为主入口，保留并强化图码同步，实现**原理图、RTL、仿真波形、综合结果**四视图联动。Agent 是**教学助手**：结合学生的设计过程自动发现典型错误，以**分级提示**引导学生自行分析和修复，不直接给答案。远期预留教材适配能力（一生一芯、PA 课等）。
- **工程版（SigFlow Pro）**：面向 FPGA/RTL 工程开发。**不再以逻辑门为主要设计单元**，改用**层次化设计视图**——顶层展示系统架构，逐级展开模块直至 RTL 实现；模块之间以总线连接，点击总线可查看位宽、协议、驱动端、负载端；代码、设计图、波形三方联动跳转。Agent 是**设计理解与调试助手**而非代码生成器：用户在设计图中点击模块/信号/总线即可提问，Agent 结合设计结构、源码、连接关系与波形分析；同时负责解读 Job 化工具的产出与结构化报告，呈现给用户做决策。

两个版本共享同一套**内核底座**：Job 化工具链（Yosys / nextpnr / gowin_pack / openFPGALoader / Verilator）、TraceBridge 调试闭环、WavePanel 波形系统、SFTree 代码结构树，以及本次规划的两个新核心系统——**Design IR（设计中问表示）** 与 **Context Engine（局部上下文引擎）**。

Agent 工作范式的核心修订（相对 `spec.md`）：

> **AI 只关注三件事：输入（Job 参数生成）、输出（产物与结构化报告解读）、用户干预（决策门）。**
> AI 不直接执行任何工具、不直接读全工程源码、不代替用户做阶段决策。每个阶段完成后的"继续 / 回退 / 修改"由用户决定。

---

## 1. 与既有规划的关系及定位修订

### 1.1 文档继承关系

```
spec.md (双模 Agent v1.0, 2026-08-20)
   │  产品形态升级：模式 → 产品线
   │  Agent 定位修订：autopilot → copilot
   ▼
本文 (双版本规划 v1.0, 2026-08-31)
   ├─ 继承：Job 化清单、分层架构、教学/工程用户画像、验收风格
   ├─ 修订：§1.2 所列四点
   └─ 新增：Design IR、Context Engine、FlowGraph/StageGate、
            教学错误模式库与分级提示、层次化设计视图、总线模型
```

### 1.2 相对 spec.md 的四点修订

| # | spec.md v1.0 的提法 | 本规划的修订 | 理由 |
|---|---|---|---|
| R1 | 工程模式 = "意图驱动全量执行 + 闭环自动修复"，AI 自动改 RTL 重跑直到通过 | **流程由用户驾驶，AI 只生成 Job 参数、解读产物、给出决策建议**；回退到 RTL 修改由用户确认（可选"AI 提议修改"但不自动执行） | EDA 阶段决策（继续布线还是改 RTL）需要工程判断，自动闭环在真实工程里风险大于收益；且与"自动化但可审计"的 SigFlow 理念更契合 |
| R2 | AI 的核心能力是"意图到代码"（RTL 生成器，P0） | RTL 以**用户编写为主**，Agent 定位为**设计理解与调试助手**；RTL 生成降级为辅助能力（模板/补全/示例改写），不再是工程版主轴 | 与产品差异化定位一致：TraceBridge 双轨调试 + 结构化上下文是 SigFlow 的独特资产，代码生成是红海 |
| R3 | AI 直接读取报告/波形做根因分析（隐含全量访问） | AI **不直接读整个工程源码**，通过 **Context Engine** 以结构化查询（层次、连接、源码位置、波形切片）构建**局部上下文** | 控制上下文规模（大规模设计无法整工程塞进 LLM）；结构化上下文与可视化设计天然联动（点谁查谁）；查询可审计 |
| R4 | 学习模式聚焦"7 步调试流程教学" | 教育版聚焦**原理图→RTL→波形→综合结果的联动教学**，Agent 以"典型错误识别 + 分级提示"引导学生自主修复 | 面向数字电路课程（本科教学），入口是原理图而非调试流程；"不给答案、分级引导"是教学法要求 |

spec.md 中与本文不冲突的部分继续有效：Job 化清单（§8.2）、执行底座架构、记忆系统、工具白名单思想、风险对策。本文定稿后，spec.md 标记为 Superseded-by 本文档，其未实现内容并入本文路线图。

### 1.3 为什么是"双产品"而不是"双模式"

教育版与工程版的分化已超出 UI 皮肤层面，深入到**主设计单元**（门级原理图 vs 层次化模块视图）、**主数据结构**（电路图 + 错误模式库 vs Design IR + Context Engine）、**Agent 交互范式**（分级提示不给答案 vs 点击即问 + 决策卡）：

- 共享的是**底座**（工具链 Job、波形、调试、解析），这是投入最大的部分；
- 分化的是**领域模型与交互**，强行塞进一个可切换的壳里会造成两套领域模型互相污染（例如教育版的门级 CanvasModel 与工程版的层次视图无法共用渲染逻辑，只在菜单里切换反而增加维护负担）。

结论：**单代码库、双构建目标（build target）+ 双发布包**。领域模型在代码层即分离（`main/edu/` 与 `main/pro/`），底座全部共享。开发调试期保留运行时切换以便对比。

---

## 2. 现有架构资产盘点（规划的地基）

### 2.1 资产映射总表

| 域 | 现有资产（真实文件） | 状态 | 双版本去向 |
|---|---|---|---|
| 拖拽原理图 | `main/CanvasPanel.*`、`CanvasModel.*`、`CanvasElement.*`、`CanvasEventHandler.*`、`CanvasNoteBook.*`、`ToolboxPanel.*`、`ToolboxModel.*`、`Wire.*`、`SVGParser.*`、`canvas_elements.json` | ✅ 可用 | **教育版主入口**；工程版仅复用其画布渲染/交互基础设施 |
| 图码同步 | `main/SigTree.*`、`SigFlowTreePanel.*`、`VerilogStructuring.*`、`VerilogManager.*`、`TreeSitterLinter.*` | ✅ 可用 | 两版共享；教育版强化（原理图↔RTL 高亮同步），工程版作为 Design IR 的源码级输入 |
| 仿真 | `main/Simulation/`：`SimulationEngine.*`、`VerilatorRunner.*`、`SimMainGenerator.*`、`StimulusParser.*`、`TimelineGenerator.*`、`ProcessRunner.*` | ✅ 可运行 | 共享；**待 Job 化**（`SimJob`），是流程编排的前置条件 |
| 综合 Job | `main/FpgaSynthesisJob.*`、`FpgaYosysRuntime.*`、`FpgaYosysScriptGenerator.*`、`FpgaYosysExecutor.*`、`fpga/FpgaYosysLogParser.*`、`FpgaYosysReport.*` | ✅ 完整 Job 化 | 共享；工程版 Agent 报告解读的主要数据源之一 |
| P&R Job | `main/fpga/NextpnrJob.*`、`NextpnrExecutor.*`、`NextpnrLogParser.*`、`NextpnrReport.*`、`NextpnrJobsPanel.*` | ✅ Job 化完成 | 共享；`--report` JSON（fmax/关键路径）供 Design IR 时序标注 |
| 打包 | `main/fpga/FpgaPackService.*`（SHA-256 记账、manifest） | 🟡 服务化，非 Job | 升级为 `FpgaPackJob` |
| 下载 | openFPGALoader GUI 入口 | 🟡 入口级 | 升级为 `FpgaFlashJob`（带实板门控） |
| 约束 | `fpga/CstValidator.*`、`FpgaConstraint.*`、`FpgaPinBindingPanel.*`、`FpgaPinData.*` | ✅ | 共享；教育版只读 + 概念解释，工程版全功能 + 总线约束 |
| TraceBridge | `main/debug/` 全套：`DebugContract/Session/Fingerprint/OverlayBuilder/NetlistValidator/Acquisition/Protocol/CaptureDecoder/MappingBuilder`、`SerialTransport/PipeTransport/ITransport`、`WaveformAligner/Comparator`、`RootCauseGraph.*`、`ReplayScenario.*`、`TraceBridgeWindow.*` | ✅ P0–P4 落地 | 共享核心资产；工程版作为 Agent 调试问答的硬件信号源，教育版做 3 步简化教学版 |
| 波形 | `main/wave/`（`TraceViewPanel`、`WaveformGLCanvas/Renderer`、`WaveCompareHub`、`WavePatternSearch`、`WaveSession`、`WaveformMiniMap`…）、`main/trace/`（VCD 懒加载/索引/缓存） | ✅ | 共享；新增**框选提问**交互与信号↔Design IR 绑定 |
| Job 面板 | `fpga/FpgaSynthesisJobsPanel.*`、`NextpnrJobsPanel.*`、`FpgaToolWindow.*`、`TerminalController.*` | ✅ | 共享；合并为统一 Job 队列视图 |
| 插件 | `main/PluginManager.*`、`PluginStore.*` | ✅ | **版本分化机制**：教育/工程特性以插件/特性开关注册 |
| 解析研究 | `main/research/`（`LogicBridge.*`、`v_lexer.*`）、`research` 下 tree-sitter 集成 | ✅ | 并入 Design IR 源码级解析管线 |
| 工程数据 | `.sigflow/fpga/runs/<job-id>/`（inputs/scripts/logs/artifacts/reports/manifest）、`.sigflow/debug/<session-id>/` | ✅ | 共享；扩展 `.sigflow/ir/`、`.sigflow/agent/`、`.sigflow/flow/` |

### 2.2 缺口清单（本规划要新建的东西）

1. `SimJob` / `FpgaPackJob` / `FpgaFlashJob` / `DebugSessionJob`（采集任务化）——统一 Job 契约；
2. **统一结构化报告 schema**（`JobReport`）：现有 Yosys/nextpnr 报告各自为政，Agent 解读需要统一格式；
3. **Design IR** 及其构建管线、增量更新；
4. **Context Engine**（查询 API + 上下文打包 + 预算管理）；
5. **FlowGraph + StageGate**（流程编排与用户决策门）；
6. **Agent 运行时**（LLM 接入层、会话、审计，`main/agent/`）；
7. **层次化设计视图**（工程版主画布）与**总线模型**；
8. **教育版错误模式库 + 分级提示引擎 + 学习者画像**；
9. 教材/课程包格式（远期，仅预留 schema）。

---

## 3. 总体架构

### 3.1 分层架构

```
┌──────────────────────────────────────────────────────────────────────┐
│ 产品外壳层                                                            │
│  ┌────────────────────────────┐   ┌────────────────────────────────┐  │
│  │ 教育版外壳 (Edu Shell)      │   │ 工程版外壳 (Pro Shell)          │  │
│  │ · 原理图画布(门级为主单元)   │   │ · 层次化设计视图(模块为主单元)   │  │
│  │ · 图码同步面板               │   │ · 总线属性面板                  │  │
│  │ · 教学引导侧栏 + 提示卡      │   │ · 自然语言命令框 + 决策卡        │  │
│  │ · 概念卡/练习                │   │ · Job 队列 + 流程流水线         │  │
│  └─────────────┬──────────────┘   └───────────────┬────────────────┘  │
├────────────────┴──────────────────────────────────┴───────────────────┤
│ Agent 层（两版共享内核，外壳分化）                        main/agent/    │
│  意图解析 → 上下文组装(Context Engine 客户端) → Job 参数生成             │
│  → 产物/报告解读 → 决策建议(StageGate 卡片) → 记忆                     │
├───────────────────────────────────────────────────────────────────────┤
│ 流程编排层                                                main/flow/    │
│  FlowGraph(可配置流程 DAG+回边) · StageGate(决策门) · 决策记录          │
├───────────────────────────────────────────────────────────────────────┤
│ 执行底座层（现有+补齐）                                   main/fpga/    │
│  SimJob · YosysJob(✅) · PnrJob(✅) · PackJob · FlashJob · DebugJob     │
│  统一 Job 契约: 状态机 + manifest + 产物 + JobReport(结构化) + 取消     │
├───────────────────────────────────────────────────────────────────────┤
│ 数据底座层                                                                │
│  Design IR (main/ir/)  ·  Context Engine (main/context/)                │
│  SFTree/VerilogStructuring · Yosys JSON · VCD/trace · .sigflow/ 数据    │
└───────────────────────────────────────────────────────────────────────┘
```

### 3.2 版本分化机制（工程决策 D1）

- **代码组织**：`main/core/`（共享底座，现有 `fpga/`、`debug/`、`wave/`、`trace/`、`Simulation/` 迁入或保持原位）、`main/edu/`（教育版特性）、`main/pro/`（工程版特性）、`main/agent/`（共享 Agent 内核）、`main/ir/`、`main/context/`、`main/flow/`。
- **构建**：同一 `SigFlow.sln`，两个 vcxproj target（`SigFlowEdu` / `SigFlowPro`）+ 一个 `SigFlowDev`（双壳并存，运行时可切换，仅内部使用）。通过 `PluginManager` 的特性注册决定面板/工具/Agent 提示词集合。
- **发布**：两个安装包；数据格式 100% 兼容（同一 `.sigflow/` 目录布局，教育版生成的工程可在工程版打开，反之亦然，只是特性可用性不同）。
- **命名**：对外 `SigFlow Edu` / `SigFlow Pro`；仓库内目录用 `edu/`、`pro/`，避免 `learn/engineering` 与 spec.md 旧术语混淆。

---

## 4. 工程版规划（SigFlow Pro）

### 4.1 产品形态：层次化设计视图

**设计单元从"逻辑门"迁移到"模块实例与总线"**。主画布是一个可逐级展开的层次设计图：

- **L0 系统视图**：顶层模块 + 板级外设（LED/UART/按键…，来自 `FpgaPinData` 引脚库）+ 模块间总线；
- **L1..N 模块视图**：双击模块进入其内部结构（子模块实例 + 内部总线/信号 + 关键寄存器），面包屑导航保留层级路径；
- **RTL 视图**：叶子模块（无子实例）直接展示其源码（`SigTextEditor`），可一键切换到"信号级"网表视图（来自 Yosys JSON，含位宽/LUT 映射）。

层次展开的粒度规则：默认展开到"有子实例或端口数 ≥ 8"的层级；`always` 块、`assign`、寄存器组在 RTL 视图查看，不在设计图上平铺（避免回到门级思维）。

### 4.2 Design IR（核心数据结构，工程决策 D4）

Design IR 是工程版一切能力（视图、联动、Agent 上下文）的单一事实来源，采用**双层 + 绑定**结构：

```
DesignIR
├─ HIR 源码级（来源: SigTree + VerilogStructuring + tree-sitter，实时、可增量）
│   ModuleDecl    { name, file, span, params, ports[] }
│   InstanceDecl  { module, instance_name, span, param_bindings, port_conns[] }
│   PortDecl      { name, dir, width, span }
│   NetDecl       { name, width, kind(wire/reg/logic), span, drivers[], loads[] }
│   AlwaysBlock   { span, sensitivity, kind(seq/comb) }
├─ NIR 网表级（来源: Yosys JSON，构建后生成、不可变、按 job-id 版本化）
│   CellNode      { type(LUT/DFF/BSRAM/CARRY), name, bel, attrs }
│   NetNode       { bits[], name, src(映射回 HIR span) }
│   时序摘要      { fmax, critical_path[](来自 NextpnrReport --report JSON) }
│   资源摘要      { lut/dff/bsram/io 占用(来自 FpgaYosysReport) }
├─ BINDING 绑定层（增量维护）
│   HIR.Instance  ↔  NIR.hierarchy path ↔ 视图节点 ID
│   HIR.Net/Port  ↔  VCD trace ID ↔ WaveSession 信号
│   HIR.Net       ↔  CanvasNode（教育版原理图节点）
│   probe(契约)   ↔  capture 通道 ↔ 波形泳道（TraceBridge 既有 DebugMappingBuilder 扩展）
└─ BUS 总线层（新模型，见 §4.3）
```

**构建管线**：

```
源码保存/工程加载
   → HIR 增量解析（tree-sitter，TreeSitterLinter 同源）
   → 层次拼装（top-down elaboration：参数传递、端口匹配检查）
Yosys Job 成功
   → NIR 导入（Yosys JSON，含 src 属性）
   → HIR↔NIR 绑定（按 module 名 + src span 对齐，冲突标记"源码已改，网表过期"）
VCD 加载
   → 信号名 ↔ HIR.Net 绑定（支持层次名 vcdpath 匹配 + 用户手工修正）
```

**一致性与增量**：Design IR 版本号 = hash(源文件 mtime+内容, 最新成功 Job id, VCD id)。任何一环变化只使受影响的子树失效；绑定关系持久化到 `.sigflow/ir/designir.json` + 侧车索引（复用 `main/trace/` 的懒加载/缓存模式）。

**约束（继承现有硬约束）**：IR 只读于工具产出，源码级 HIR 是唯一可写入口；NIR 与 Job manifest 的 SHA-256 强关联，过期网表必须标记。

### 4.3 总线模型与交互

总线是工程版一等公民，独立于"wire"存在：

```
BusDecl {
  bus_id, name,
  width,                      // 位宽（来自 HIR，多拍信号取数据通路宽度）
  protocol,                   // 协议标签: "simple" | "valid-ready" | "req-ack"
                              // | "handshake-4phase" | "axi4-lite" | "custom:<name>"
  segments[],                 // 物理跨度（可跨层级）
  driver:  InstanceRef + PortRef,     // 驱动端（多驱动 = 错误，直接标红）
  loads:   [InstanceRef + PortRef],   // 负载端列表
  signals: { data[], valid?, ready?, clk?, rst? },  // 协议成员信号映射
  spans:   [SourceSpan],              // 声明/连线处源码位置
  traces:  [VCD trace id],            // 波形绑定
}
```

- **协议识别**：三级策略——① 命名规则（`*_valid/*_ready` → valid-ready；`axi_*` → AXI 系列）；② 结构规则（一驱动多负载 + 握手信号对存在）；③ **用户标注优先**（总线属性面板手工指定，写入 BusDecl 并持久化，覆盖推断）。识别器是 Context Engine 的一个查询实现。
- **交互**：点击设计图中的总线 → 右侧属性面板显示完整 BusDecl（位宽/协议/驱动/负载/源码跳转/波形打开）；驱动或负载模块可点击跳转；协议成员信号可整组加入波形。
- **约束联动**：总线位宽/协议与 CST/时序例外（未来 SDC）关联，位宽不匹配、多驱动在层次 elaboration 时即报错。

### 4.4 代码–设计图–波形三方联动

联动以 **Selection Context（选中上下文）** 为总线事件实现（一个全局 `SelectionService`，发布/订阅）：

| 选中处 | 设计图 | 代码编辑器 | 波形面板 |
|---|---|---|---|
| 设计图模块 | 高亮 + 可进入 | 打开文件并滚动到 module 定义 | 折叠显示该模块边界内信号 |
| 设计图总线/信号 | 高亮 | 高亮声明 span + 所有连线处（复用 TreeSitterLinter 的 span 基础） | 自动定位并选中对应 trace（含 capture 泳道） |
| 代码标识符 | 定位所属模块/总线并高亮 | — | 同上 |
| 波形泳道 | 定位驱动源模块 | 打开驱动赋值处（`drivers[].span`） | — |
| 波形**框选** | 高亮框选时刻活跃的模块 | — | 进入"框选提问"模式（§4.7） |

工程版保留现有 SFTree 作为"文件/模块浏览器"角色，设计图承担"结构浏览"角色，二者共存。

### 4.5 Context Engine（工程决策 D5）

**定位**：AI 与工程之间唯一的数据通道。Agent 不直接读源码文件、不直接读 VCD 原始数据，一切通过 Context Engine 的结构化查询获得。

```
main/context/
├─ ContextEngine.h/.cpp        // 查询入口 + 权限边界（Agent 只能调它）
├─ ContextQuery.h              // 查询定义（类型化，见下表）
├─ ContextPack.h/.cpp          // 打包结果（token 预算受控）
├─ ContextBudget.h             // 预算策略（按查询类型分配 token 上限）
└─ ContextCache.h/.cpp         // 查询缓存（键= query+IR 版本号）
```

**查询 API（首批）**：

| 查询 | 输入 | 返回内容 | 主要消费者 |
|---|---|---|---|
| `GetModuleCard` | module 名 / 实例路径 | 模块卡片：端口表、参数、子实例列表、资源占用、源码 span 摘要 | 点击模块提问 |
| `GetBusCard` | bus_id | 完整 BusDecl + 驱动/负载模块卡片摘要 | 点击总线提问 |
| `GetHierarchy` | 根路径 + 深度 | 层次子树（模块名/实例数/粗粒度资源） | "这个系统怎么组织的" |
| `GetFanoutCone` / `GetFaninCone` | 信号 + 深度 N | 上/下游 N 级驱动链（span + 模块路径） | 调试追因（与 RootCauseGraph 协同） |
| `GetSourceSnippet` | span + 上下文行数 | 源码片段（带行号，**唯一允许取源码的通道**，片段级） | 定位到具体代码时 |
| `GetWaveSlice` | trace ids + 时间窗 | 波形切片摘要（值序列/翻转次数/稳定区间/触发点，**不返回原始采样**） | 波形框选提问 |
| `GetJobDigest` | job-id / flow-id | 结构化报告摘要（错误列表、资源、时序、与上一版 diff） | 报告解读 |
| `GetCompareDigest` | 两个 capture/会话 | 双轨差异摘要（首个差异点 ± 窗口，来自 WaveformComparator） | 调试提问 |

**上下文打包（ContextPack）**：查询结果组装为紧凑 JSON（字段名缩写、仅必要信息），带 token 预算上限；超预算时按"离选中对象跳数"衰减裁剪。每个 ContextPack 记录审计日志（谁在何时问了什么、返回了什么），落 `.sigflow/agent/audit/`。

**失效策略**：IR 版本号变化 → 对应缓存全失效；文件级粒度失效用于 HIR 查询。

### 4.6 Job 化流程编排：FlowGraph + StageGate（工程决策 D2/D3）

#### 4.6.1 硬件流程闭环（产品主循环）

```
┌─────────┐    ┌────────┐    ┌────────┐    ┌─────────┐    ┌──────────────┐
│ ① RTL   │───▶│ ② 仿真 │───▶│ ③ 综合 │───▶│ ④ P&R   │───▶│ ⑤ 打包/烧录  │
│  编写    │    │ (Job)  │    │ (Job)  │    │  (Job)  │    │  (Job, 可选  │
│ (人为主) │    │        │    │        │    │         │    │   调试采集)   │
└────▲────┘    └───┬────┘    └───┬────┘    └───┬─────┘    └──────┬───────┘
     │             │             │             │                  │
     │        [Gate]仿真通过? [Gate]综合干净? [Gate]时序/资源OK? [Gate]烧录确认
     │             │             │             │                  │
     │             ▼             ▼             ▼                  ▼
     │         ┌──────────────────────────────────────────┐   ┌──────────┐
     │         │  ⑥ 数据比对：仿真 VCD vs 硬件 capture      │──▶│ ⑦ 达标?  │
     │         │  （WaveformAligner/Comparator，可选）      │   │ 用户描述  │
     │         └──────────────────────────────────────────┘   │ 或框选波形│
     │                                                        └────┬─────┘
     └─────────────── 否：回到 ①（用户决策，可携反馈）◀─────────────┘
```

这条主循环即为默认 **FlowGraph**。它是**数据不是代码**：JSON 描述的阶段 DAG + 回边 + 每阶段的 Gate 配置，容许：

- **跳过**：无板卡时 ⑤⑥ 关闭（纯仿真流程）；不调试时 ⑤ 只打包不烧录；
- **回退**：任何 Gate 处可回退到任意上游阶段（默认回①，参数级问题可只回③）；
- **分叉**：⑤ 可并列多个变体（如 debug overlay 构建与普通构建并行）；
- **自定义**：项目内 `.sigflow/flow/flow.json` 可覆盖默认图（如插入 Lint 阶段、等价性检查阶段）。

```
main/flow/
├─ FlowGraph.h/.cpp        // 阶段图模型（加载/校验/版本化）
├─ FlowRun.h/.cpp          // 一次流程执行实例（阶段状态、产物指针、回退历史）
├─ StageGate.h/.cpp        // 决策门：AI 决策卡生成 + 用户选择记录
├─ GateDecisionCard.h      // 决策卡数据结构（见 §4.6.3）
└─ schemas/                // flowgraph.schema.json / gatedecision.schema.json
```

#### 4.6.2 统一 Job 契约（AI 只碰三件事的落点）

每个工具 Job 必须实现统一契约（现有 `FpgaSynthesisJob`/`NextpnrJob` 已接近，补齐字段即可，其余 Job 按此改造）：

```
JobContract {
  job_type:        "sim.verilator" | "synth.yosys" | "pnr.nextpnr"
                 | "pack.gowin"  | "flash.ofl"    | "debug.tracebridge",
  // ① 输入 —— AI（或用户/UI）生成的参数，schema 校验后才允许提交
  params_schema:   { top, sources[], defines, constraints, strategy,
                     timeout, artifact_expectations... },
  // ② 输出 —— 机器可读产物清单 + 结构化报告
  artifacts:       [{ path, kind(netlist|bitstream|vcd|report|fs), sha256 }],
  report:          JobReport {
                     status, stage_summaries[],
                     errors[{ code, severity, file, line, tool_message,
                              ir_binding(span/module/net), hint_category }],
                     stats{ resource, timing, coverage... },
                     anomalies[]        // 报告解析器标记的"异常点"
                   },
  // ③ 用户干预 —— 决策门定义
  interventions:   [{ gate_id, when(after_stage|on_error|before_flash),
                     required_decisions[continue|jump_back|modify_params|abort] }],
  // 治理（既有能力，保留）
  state_machine:   Created→Validating→Queued→Running→ValidatingArtifact
                 →Succeeded/Failed/Cancelled/TimedOut,
  workspace:       .sigflow/<domain>/runs/<job-id>/{inputs,scripts,logs,artifacts,reports,manifest}
}
```

**报告 schema 统一**是本阶段最重的存量改造：`FpgaYosysLogParser`、`NextpnrLogParser`、Verilator 输出解析分别产出 `JobReport`；每个 error 尽量绑定 IR 坐标（file:line 已有，补充 module/net 绑定），这是 Agent"多少报错、异常在哪"的精度来源。

#### 4.6.3 以 Yosys 为例的完整时序（用户剧本）

```
用户在仿真通过后查看波形，决定进入综合。
 1. 用户（或 Agent 对话）发起："综合这个设计"
 2. Agent 通过 Context Engine 取 GetHierarchy/GetModuleCard，
    结合用户提示词，生成 Yosys Job 参数（top、strategy、约束路径）
 3. [Gate-params] 参数预览给用户：脚本要点 + 与上次 diff，用户确认
    （首次或参数变化时必经；重复执行可配置自动放行）
 4. YosysJob 运行：受控 .ys 生成 → 进程执行 → 日志流式回传 Terminal
 5. 产物：pre.json/netlist.json + JobReport（错误/警告/资源统计）
 6. Agent 读取 JobReport → 生成决策卡：
    "综合完成。0 错误 / 3 警告；LUT 1642 (-5% vs 上次)。
     异常点: ① netlist 中 state[2] 被 keep 保护（来自调试契约）；
             ② 警告 UNUSED_WIRE: uart_rx 在顶层未被驱动。
     建议: 继续 P&R（时序上轮有 32% 裕量）或先处理 ②。"
 7. [Gate-synth] 用户选择：继续 → P&R ｜ 跳回 ① 修改 RTL ｜ 查看报告详情
 8. 决策记录写入 .sigflow/flow/<flow-id>/decisions.jsonl（可审计、可回放）
```

#### 4.6.4 与现有代码的映射

| 新概念 | 现有对应 | 改造量 |
|---|---|---|
| `SimJob` | `Simulation/VerilatorRunner + ProcessRunner` | 中：包一层 Job 状态机 + JobReport |
| Yosys 阶段 | `FpgaSynthesisJob` 全套 | 小：补 `JobReport` schema 与 IR 绑定 |
| P&R 阶段 | `NextpnrJob` 全套 | 小：同上 |
| `FpgaPackJob` | `FpgaPackService` | 小：状态机化（服务逻辑已完整） |
| `FpgaFlashJob` | openFPGALoader 入口 | 中：Job 化 + 烧录确认门（强制） |
| Debug 阶段 | `DebugSession` + `DebugAcquisition` + `TraceBridgeWindow` | 中：暴露为 FlowGraph 阶段（7 步 stepper 保留为手动模式） |
| 比对阶段 | `WaveformAligner/Comparator` | 小：产物化 `CompareReport` |

### 4.7 工程版 Agent：设计理解与调试助手

#### 4.7.1 能力矩阵

| 能力 | 交互入口 | 数据来源（全部经 Context Engine） |
|---|---|---|
| **点击即问** | 设计图选中模块/总线/信号 → 对话框自动带上选中上下文 | GetModuleCard / GetBusCard / GetSourceSnippet |
| **波形框选提问** | 波形面板框选一段 → "这里为什么停住了？" | GetWaveSlice + GetFaninCone + GetJobDigest |
| **报告解读** | 每个 Job 完成后自动生成决策卡；用户可追问 | GetJobDigest（统一 JobReport） |
| **设计导航** | "我想看 UART 接收链路" | GetHierarchy + 总线遍历 |
| **调试根因** | TraceBridge 双轨差异 / 框选异常区间 | GetCompareDigest + GetFanoutCone，衔接 RootCauseGraph 的候选排序 |
| **辅助改码** | 用户明确要求时：提议 patch（diff 形式），**不自动应用** | GetSourceSnippet（片段级） |

#### 4.7.2 边界（写进系统提示词 + 工具层双重约束）

- Agent 无文件系统工具、无进程执行工具；唯一工具集 = Context Engine 查询 API + "Job 参数草稿生成"（产出 JSON，提交前必须过 Gate-params）；
- 源码访问仅 `GetSourceSnippet` 片段级，且记录审计；
- RTL 修改只能以 diff 建议呈现，应用动作由用户在编辑器确认；
- LLM 不可用时：报告解读降级为规则版摘要（JobReport → 模板渲染，无 AI 也能用），流程编排与决策门完全不依赖 LLM。

---

## 5. 教育版规划（SigFlow Edu）

### 5.1 产品形态与用户旅程

目标用户：数字电路课程学生（组合逻辑 → 时序逻辑 → 状态机 → 简单外设）。

一节典型实验课的旅程：

```
拖拽门电路搭一个半加器
  → 图码同步面板看到自动生成的 Verilog（可对照学习）
  → 一键仿真，波形面板展示输入/输出（四视图联动高亮当前门的信号）
  → Agent 检测到"用组合逻辑直接反馈构成锁存"→ 弹出 L1 提示卡：
     "你的电路里存在一条从输出绕回输入的组合路径，想想这会造成什么？"
  → 学生尝试修改；再次仿真仍错 → 学生点"再给点提示" → L2：
     "问题出在 OR 门 U3 的输出又被送回了 AND 门 U1 的输入。"
  → 仍不明白 → L3 概念卡：组合环/锁存器讲解 + 波形示意
  → 主动点"看参考解法" → L4（记录进学习者画像）
  → 综合视图展示"你的电路用了几个 LUT、延迟多少"（卡通化资源卡）
```

### 5.2 拖拽原理图与图码同步（存量强化）

- 保留现有 `Canvas*`/`Toolbox*`/`Wire` 门级设计能力，工具箱按课程进度分组（基础门 → 中规模器件 74138/74161 → 自定义封装子电路）；
- **图码同步**：现有 SFTree 基础上强化双向性——原理图选中 ↔ RTL 源码高亮；RTL 手工修改后原理图标记"待重新生成"并可视化 diff（而不是静默覆盖）；
- 子电路（用户把一部分电路封装成模块）与工程版的 Module 概念**共用 Design IR 的 HIR 结构**——这是两版数据底座统一的关键点：教育版的"子电路封装"就是工程版"模块化设计"的教学前置。

### 5.3 四视图联动（Selection Context 同一套机制）

教育版与工程版共用 §4.4 的 SelectionService，只是视图集合不同：

| 教育版视图 | 对应组件 | 联动要点 |
|---|---|---|
| 原理图 | `CanvasNoteBook` | 主视图；选中元件 → RTL 行、波形泳道、综合映射（元件 → LUT 数）联动 |
| RTL | `SigTextEditor` + SFTree | 只读为主（防止学生绕过原理图学习；教师模式可解锁编辑） |
| 波形 | `TraceViewPanel` | 仿真 VCD；AI 事件标注（复位/首个差异/毛刺嫌疑） |
| 综合结果 | 新增 `EduSynthView`（卡通资源卡） | Yosys JobReport 教学化渲染："你的设计用了 3 个 LUT，相当于约 12 个等效门" |

### 5.4 教学 Agent：典型错误识别 + 分级提示（工程决策 D6）

#### 5.4.1 错误模式库

```
main/edu/
├─ ErrorPattern.h/.cpp        // 模式定义与匹配结果
├─ ErrorPatternLibrary.h/.cpp // 规则库加载（JSON，可由教师/课程包扩展）
├─ TieredHint.h/.cpp          // 分级提示引擎
├─ LearnerProfile.h/.cpp      // 学习者画像
└─ schemas/errorpattern.schema.json / learnerprofile.schema.json
```

规则 schema：

```
ErrorPattern {
  pattern_id,           // "comb-loop" | "latch-inferred" | "width-mismatch"
                        // | "blocking-in-seq" | "missing-reset" | "cdc-hazard"
                        // | "off-by-one-counter" | ...
  severity,             // error | warning | advice
  detectors: [          // 一个模式可由多个探测器联合确认
    { kind: "lint",       source: "TreeSitterLinter" },
    { kind: "structure",  source: "DesignIR(HIR)" },        // 组合环=HIR 图环检测
    { kind: "synth",      source: "JobReport(yosys)" },     // 锁存器推断=yosys 警告码映射
    { kind: "wave",       source: "WaveAnalysis" },         // 波形特征（X 态、停滞、毛刺嫌疑）
    { kind: "compare",    source: "CompareReport" }         // 仿真 vs 期望/实测差异
  ],
  hints: [              // 分级提示，逐级增强
    { level: 1, text: 方向性提问，不给位置 },     // "存在一条输出绕回输入的组合路径…"
    { level: 2, text: 定位到元件/代码行 },
    { level: 3, concept_ids: [概念卡], 讲解成因 },
    { level: 4, reference_fix: 参考解法（diff/电路改法） }
  ],
  concepts: [关联概念 ID]，    // 链接到概念词典
  prerequisites: [概念 ID]     // 提示 L3 前检查前置知识
}
```

首批规则集（建议 15–20 条，覆盖本科数电 90% 高频错误）：组合环、锁存器推断、位宽不匹配、时序块用阻塞赋值、缺少复位、复位极性不一致、敏感列表不全、计数器 off-by-one、状态机死锁/不可达状态、时钟域交叉无同步、三态误用、未驱动的输入、悬空输出、竞争冒险嫌疑（波形）、仿真 X 态传播。

**探测器全部复用现有设施**：TreeSitterLinter（静态）、Design IR HIR（结构）、FpgaYosysLogParser 警告码（综合）、WaveAnalysis（波形）、WaveformComparator（差异）——教学 Agent 的增量主要是"规则库 + 提示文案"，不是新分析引擎。

#### 5.4.2 分级提示协议（不给答案的机制保障）

1. **UI 门控**：提示卡一次只显示当前级别；"下一级提示"需学生主动点击；L4（参考解法）需二次确认（"确定要看参考解法吗？这会记录到你的学习记录"）；
2. **系统提示词约束**：教学 Agent 的提示词明确禁止"在 L1–L3 泄露修复代码/具体改法"；LLM 只负责把规则库命中的模式**用学生能懂的语言重新表述**并回答追问，规则命中本身不依赖 LLM（离线可用）；
3. **提示历史**：每级提示的请求/学生反应（改了什么、改对没有）记入学习者画像；
4. **教师视图**：画像汇总报表（哪些模式最常错、卡在哪一级），用于课程调整。

#### 5.4.3 学习者画像

```
LearnerProfile {
  mastered_concepts: [concept_id + 证据(练习正确率/错误不再复发)],
  weak_patterns:     [pattern_id + 出现次数 + 平均解锁级别],
  hint_history:      [{ pattern_id, levels_used, resolved_by(自研|提示|参考) }],
  exercise_records:  [...]
}
```

持久化到 `.sigflow/agent/learner_profile.json`（与工程版记忆同目录、不同文件）。画像驱动：已掌握概念的 L1 提示更简短；高频弱点在课程开始时主动复习卡。

### 5.5 教材适配预留（远期，只定接口不实现）

一生一芯、PA 课（NJU PA）等教材适配的共性抽象为**课程包（Course Package）**：

```
CoursePackage {
  metadata:   { course, version, board_profile },
  units: [{ unit_id, title, 里程碑检查点[] }],
  experiments: [{ exp_id, starter_project, stimuli, 期望波形/真值表,
                  errorpattern_overrides, hint_overrides,    // 教材定制提示
                  concept_map }],
  rubric:     评分规则（检查点通过情况 + 提示使用情况的加权）
}
```

关键决策：**错误模式库与提示引擎从第一天就设计为可被课程包覆盖**（overrides 机制），这是本次规划中唯一为教材适配做的实质预留，其余（课程内容制作）推迟到教育版核心跑通之后。

---

## 6. Agent 公共架构（两版共享，`main/agent/`）

### 6.1 修订后的内核（五件套 → 四件套 + 门户）

```
┌─ 意图解析 Intent ──────────────────────────────────────┐
│  分类: 查询 / 提问 / 阶段发起 / 参数调整 / 教学求助        │
│  抽取: 选中对象(SelectionService) + 用户话语 + 流程状态    │
├─ 上下文组装 Context Assembly ──────────────────────────┤
│  调 Context Engine 查询 API → ContextPack(token 受控)    │
├─ 规划与参数生成 Planner ───────────────────────────────┤
│  输出: Job 参数草稿(过 schema) / 查询计划 / 修复建议 diff │
│  ——注意: 不输出"直接执行"，一切动作都要过 Gate           │
├─ 解读 Interpretation ─────────────────────────────────┤
│  JobReport → 决策卡 / 错误模式 → 分级提示(教育版)        │
│  CompareReport+RootCauseGraph → 根因叙述(工程版)         │
├─ 记忆 Memory ─────────────────────────────────────────┤
│  短期: 会话内 FlowRun 状态 + 最近决策                     │
│  长期-工程: 项目偏好/常用参数/历史决策模式                  │
│  长期-教学: LearnerProfile                              │
└────────────────────────────────────────────────────────┘
```

相对 spec.md 五件套的修订：**"执行(Executor)"从 Agent 内核中移除**——执行全部收敛到 FlowGraph/Job 系统，Agent 与执行系统之间只有"参数草稿"和"报告回传"两个方向的数据流。这是 R1 修订的架构落点。

### 6.2 LLM 接入层

```
main/agent/
├─ LlmProvider.h           // 抽象: 云 API / 本地推理(预留 llama.cpp/ollama) / Mock
├─ LlmRouter.h/.cpp        // 按任务选择 provider; 断网降级(规则模板)
├─ AgentSession.h/.cpp     // 会话管理
├─ AgentPrompts.h          // 版本化提示词(edu/pro 不同外壳)
└─ audit/ 逻辑             // ContextPack + 请求 + 响应落盘 .sigflow/agent/audit/
```

降级策略（离线可用性，教育版尤其重要）：错误检测/分级提示/报告摘要模板渲染均不依赖 LLM；LLM 负责"表述、追问解答、上下文相关的解释"。教师可锁定为纯离线模式。

### 6.3 安全与审计

- 所有 ContextPack 与 LLM 交互落审计日志（本地，`.sigflow/agent/audit/`），课堂场景默认不出网（云 provider 需在设置显式开启）；
- 烧录实板永远是强制人工 Gate（继承 spec.md 门控 + TraceBridge 既有"用户主动点击确认"约定）；
- 决策链完整可回放：`decisions.jsonl` + Job manifest + 审计日志三者对齐，任何一次流程结果都能回答"当时为什么这么决定"。

---

## 7. 数据结构变动总表

| 新增结构 | 落盘位置 | schema | 消费者 |
|---|---|---|---|
| Design IR（HIR/NIR/Binding） | `.sigflow/ir/designir.json` + 侧车索引 | `ir.schema.json` | 层次视图、联动、Context Engine |
| BusDecl 总线模型 | 并入 Design IR | 同上 | 总线面板、协议识别、约束 |
| JobReport 统一报告 | 各 Job `reports/` | `jobreport.schema.json` | Agent 解读、决策卡、教育版资源卡 |
| FlowGraph / FlowRun | `.sigflow/flow/flow.json`、`<flow-id>/` | `flowgraph.schema.json` | 流程编排、回退 |
| GateDecisionCard / decisions.jsonl | `.sigflow/flow/<flow-id>/` | `gatedecision.schema.json` | 决策门、审计回放 |
| ContextPack 审计 | `.sigflow/agent/audit/` | — | 审计 |
| ErrorPatternLibrary | `main/edu/patterns/*.json`（内置）+ 课程包覆盖 | `errorpattern.schema.json` | 教学 Agent |
| LearnerProfile | `.sigflow/agent/learner_profile.json` | `learnerprofile.schema.json` | 教学 Agent、教师视图 |
| CoursePackage | `courses/<name>/` | `coursepackage.schema.json`（远期） | 教材适配 |

工程文件格式（`sigflow.project`/`.project`）增补 `edition_hint`、`flow`（自定义流程覆盖）、`bus_annotations`（用户总线标注）字段——向后兼容，缺省走默认值。

## 8. UI 变动总表

| 组件 | 版本 | 动作 | 要点 |
|---|---|---|---|
| 版本外壳选择器 | Dev | 新增 | 仅开发构建，运行时切换对比 |
| `HierarchyDesignView` | Pro | **新建** | 层次设计画布：模块框+总线连线+面包屑+逐级展开；复用 Canvas 的渲染/事件基础设施 |
| `BusPropertyPanel` | Pro | 新建 | 总线属性：位宽/协议/驱动/负载/跳转 |
| `AgentPanel`（对话） | 共享 | 新建 | edu 教学向 / pro 工程向两套外壳配置 |
| `DecisionCard`（浮层） | Pro | 新建 | Gate 决策卡：摘要+异常+建议+四按钮（继续/回退/改参/中止） |
| `FlowPipelineBar` | Pro | 新建 | 底部流程流水线：①—⑦ 阶段状态 + 回退历史 |
| `JobQueuePanel` | 共享 | 合并 | 统一 Sim/Synth/Pnr/Pack/Flash/Debug Job 队列（合并现有两个 JobsPanel） |
| `SelectionService` + 联动高亮 | 共享 | 新建 | §4.4 事件总线 |
| 波形框选提问 | 共享 | 增强 `WaveformView` | 框选→上下文菜单"问 Agent" |
| `EduGuideSidebar` | Edu | 新建 | 实验步骤引导 + 当前步骤讲解 |
| `HintCard`（分级提示卡） | Edu | 新建 | L1–L4 逐级解锁 UI |
| `EduSynthView` | Edu | 新建 | 卡通化综合结果卡 |
| 概念卡浮层 | Edu | 新建 | 悬停术语/信号弹出（词典 JSON 驱动） |
| 教师模式开关 | Edu | 新增 | 解锁 RTL 直接编辑、画像报表、L4 预览 |
| `TraceBridgeWindow` | 共享 | 保留 | 手动 7 步模式保留；工程版可被 FlowGraph 阶段化调用 |

---

## 9. 实施路线图

依赖关系：Phase 0 是一切的地基；Phase 1/2（工程版）与 Phase 3（教育版）在 Phase 0 后可双轨并行，Phase 4 汇合。

### Phase 0 — 底座统一（约 1 个迭代，最高优先级）

- [ ] `JobReport` 统一 schema 定稿 + Yosys/nextpnr 报告解析器改造接入
- [ ] `SimJob`（Verilator 仿真 Job 化，复用 `FpgaSynthesisJob` 状态机模式）
- [ ] `FpgaPackJob`（`FpgaPackService` 状态机化）
- [ ] `FpgaFlashJob` + 强制烧录确认门
- [ ] `SelectionService` 骨架（先打通 代码↔波形 两方联动）
- [ ] `main/agent/` 骨架：LlmProvider/Router（先 Mock + 规则模板）、审计落盘
- **验收**：同一工程里 Sim→Synth→PnR→Pack→Flash 全部以 Job 形式运行并在统一队列可见；任一 Job 的报告可被程序化读取为 `JobReport` JSON。

### Phase 1 — 工程版数据底座（Design IR + 层次视图）

- [ ] HIR 构建管线（SFTree/VerilogStructuring/tree-sitter 汇入，增量解析）
- [ ] HIR↔NIR 绑定（Yosys JSON src 属性对齐）+ 版本化与失效
- [ ] `HierarchyDesignView` 首版（L0/L1 展开、面包屑、模块框）
- [ ] 总线模型：命名/结构推断 + 用户标注 + `BusPropertyPanel`
- [ ] 三方联动全通（SelectionService 接入设计图/代码/波形）
- **验收**：打开 `examples/tracebridge_tangnano9k`，设计图可从顶层展开到 `min_led_uart_top` 内部；点击 `led[3:0]` 总线能看到位宽/驱动/负载并三方跳转。

### Phase 2 — Context Engine + 工程版 Agent

- [ ] Context Engine 查询 API 首批 8 个（§4.5 表）+ 预算 + 缓存 + 审计
- [ ] AgentPanel（工程向）+ 点击即问（选中对象自动组装 ContextPack）
- [ ] `GetJobDigest` + 报告解读（规则版优先，LLM 增强表述）
- [ ] 波形框选提问（`GetWaveSlice` + `GetFaninCone`）
- [ ] 调试根因叙述（CompareDigest + RootCauseGraph 衔接）
- **验收**：在设计图点某模块问"这个模块干什么/资源多少"，答案与 IR 一致；框选 capture 异常区间提问，回答能指出驱动链上的可疑信号；断网时报告解读仍可用（模板版）。

### Phase 3 — FlowGraph + StageGate（流程编排）

- [ ] FlowGraph 模型 + 默认硬件流程闭环（§4.6.1）+ `.sigflow/flow/` 持久化
- [ ] StageGate：参数确认门 / 阶段完成门 / 烧录门；决策卡 UI；decisions.jsonl
- [ ] TraceBridge 阶段化（Debug 阶段接入 FlowGraph，7 步 stepper 保留手动模式）
- [ ] 比对阶段产物化（`CompareReport`）
- [ ] 回退机制（任意 Gate 回任意上游阶段，产物与 IR 版本联动标记）
- **验收**：完整跑一遍主循环（含板卡调试），全程 4 次 Gate 均由用户决策；决策日志可回放；无板卡环境跑通 ①→④+仿真比对。

### Phase 4 — 教育版核心

- [ ] `ErrorPatternLibrary` 首批 15–20 条规则 + 5 类探测器接入
- [ ] `TieredHint` 分级提示引擎 + `HintCard` UI + L4 门控
- [ ] `EduGuideSidebar` + 概念卡浮层（概念词典 JSON ≥50 条）
- [ ] `LearnerProfile` + 教师视图（画像报表、教师模式）
- [ ] `EduSynthView` 卡通资源卡 + 四视图联动收口
- [ ] 教育版构建目标（`SigFlowEdu`）+ 工具白名单（隐藏烧录/高级触发，沿用 spec.md §8.5）
- **验收**：一名非数电背景用户在无提示词引导下，靠 L1–L2 提示独立修复"锁存器推断"错误；L4 查看率 < 30%；断网全流程可用。

### Phase 5 — 增强与远期

- [ ] 课程包 schema 定稿 + 首个教材适配试点（一生一芯 / PA 课，视合作进展）
- [ ] 设计空间探索（参数扫描 + 多方案对比，spec.md P3 内容择要并入）
- [ ] Verible lint / 等价性检查 / 覆盖率等扩展 Job（按需）
- [ ] 本地 LLM 推理 provider（ollama）
- [ ] 发布工程：双安装包、用户手册、演示课例

---

## 10. 总体验收标准

**共享底座**
- [ ] 所有工具（Sim/Synth/PnR/Pack/Flash/Debug）以统一 Job 契约运行，状态机一致、可取消、产物+manifest+JobReport 齐全；
- [ ] 任一次流程的完整决策链（decisions.jsonl + manifest + 审计日志）可离线回放。

**工程版**
- [ ] 层次设计视图对 ≥3 层、≥30 模块的真实工程（含 TraceBridge 示例）流畅展开（<1s/层）；
- [ ] 总线点击展示位宽/协议/驱动/负载，三方联动跳转准确率（抽样人工核对）≥95%；
- [ ] Agent 对任意模块/总线/波形框选的回答，其引用的结构化事实与 IR 逐项一致（可审计核查）；
- [ ] 单次提问 ContextPack 预算 ≤ 预设上限，大工程（100+ 文件）下不整读源码（审计日志验证）；
- [ ] 主循环含调试全流程在 Tang Nano 9K 上完整跑通一次；无板卡环境跑通到比对。

**教育版**
- [ ] 原理图→RTL→波形→综合结果四视图联动可用；
- [ ] 首批错误模式规则在对应场景 100% 命中（构造用例），误报率 <10%；
- [ ] 分级提示 L1–L3 不泄露修复代码（人工评审 + 提示词回归测试）；
- [ ] 离线模式下教学核心（检测/提示/概念卡）全部可用。

---

## 11. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| HIR↔NIR 绑定在生成代码/参数化设计上失配 | 联动跳转错位 | 绑定置信度分级，低置信标记"推测"；用户可手工修正并持久化；`DebugNetlistValidator` 的探针校验经验复用 |
| 总线协议自动识别误判 | Agent 上下文失真 | 用户标注永远覆盖推断；推断结果显式标注来源（inferred/annotated） |
| LLM 幻觉编造设计事实 | 工程决策被误导 | Agent 只允许引用 ContextPack 内事实；决策卡附"事实来源"清单；审计可核查；规则版摘要兜底 |
| JobReport 统一改造量大 | Phase 0 延期 | 解析器已存在（Yosys/nextpnr），改造是映射不是重写；Verilator 输出解析新建但格式简单 |
| 双版本并行开发人力不足 | 双线都做不深 | Phase 0/1 全共享是主线；Phase 2（工程）与 Phase 4（教育）可由不同小组分头推进；先保工程版（竞赛/演示刚需），教育版复用其全部底座 |
| 教学规则库内容工作量大 | 教育版空壳 | 首批只做 15–20 条高频模式；文案由 LLM 辅助起草 + 人工审校；课程包 overrides 让教师分担 |
| 烧录误操作 | 硬件风险 | 烧录永远强制人工 Gate（不可配置关闭） |
| 大工程 IR 增量更新性能 | UI 卡顿 | 借鉴 `main/trace/` 懒加载/侧车索引模式；IR 构建放后台线程 + 失效粒度到文件 |
| LLM 网络依赖 | 课堂不可用 | 教学检测/提示/摘要全部规则化兜底；云调用需显式开启 |

---

## 12. 附录：新模块与现有文件对照速查

| 新模块 | 目录 | 前置依赖（现有文件） |
|---|---|---|
| `SimJob` | `main/fpga/` 或 `main/Simulation/` | `SimulationEngine`、`VerilatorRunner`、`ProcessRunner`、`FpgaSynthesisJob`（状态机模板） |
| `FpgaPackJob` / `FpgaFlashJob` | `main/fpga/` | `FpgaPackService`、openFPGALoader 入口 |
| `JobReport` schema | `main/fpga/schemas/` | `FpgaYosysLogParser`、`NextpnrLogParser`、`FpgaYosysReport`、`NextpnrReport` |
| `DesignIR` | `main/ir/` | `SigTree`、`VerilogStructuring`、`TreeSitterLinter`、Yosys JSON、`DebugMappingBuilder` |
| `HierarchyDesignView` | `main/pro/` | `CanvasPanel` 渲染基础设施、`FpgaPinData`、Design IR |
| `BusPropertyPanel` | `main/pro/` | Design IR、`PropertyPanelBuilder` |
| `ContextEngine` | `main/context/` | Design IR、`main/trace/`、`WaveformComparator`、JobReport |
| `FlowGraph` / `StageGate` | `main/flow/` | 全部 Job、`DebugSession`、`WaveformAligner` |
| `AgentRuntime` | `main/agent/` | ContextEngine、FlowGraph、JobReport |
| `ErrorPatternLibrary` / `TieredHint` | `main/edu/` | `TreeSitterLinter`、Design IR(HIR)、JobReport、`WaveAnalysis`、`WaveformComparator` |
| `LearnerProfile` | `main/edu/` | TieredHint |
| `SelectionService` | `main/core/` | `CanvasPanel`、`SigTextEditor`、`TraceViewPanel`、HierarchyDesignView |

---

**文档结束。**
