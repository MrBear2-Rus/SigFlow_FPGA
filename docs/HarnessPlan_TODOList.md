# HarnessPlan TODO List（进度清单）

> 版本：v1.0 · 2026-09-21
> 基线仓库：`SigFlow_FPGA_Cmake`
> 依据：`docs/HarnessPlan.md`（§13 路线图 P0–P4、§14 验收、§2 遗留 R/H、附录 E）
> 说明：本清单把 HarnessPlan 的路线图**逐条拆成可勾选任务**，并标注**已做 / 在做 / 未做**。
> 状态图例：`[x]` 已完成并验证 · `[~]` 部分完成/进行中 · `[ ]` 未开始 · `[!]` 阻塞/需决策

---

## 0. 当前基线快照（2026-09-21）

- 构建：`cmake --build build-gcc --target sigflow` **成功**（`sigflow.exe`）。
- 测试：`ctest -R "eda_|sig_tree"` **19/19 Passed**；`-Wall -Wextra` 零警告。
- **工具链插件化主体（P1）已完成**（DoD 达标）；**主程序接入（P1-8 UI）未完成**，`main/` 仍走 legacy `main/jobs`、`FpgaYosysExecutor`、`NextpnrExecutor`、`SimulationEngine`。
- 已有插件（`InnerPlugin/`，9 个）：`eda-synth-yosys`、`eda-pnr-nextpnr`、`eda-pack-gowin`、`eda-program-openfpgaloader`、`eda-sim-verilator`、`eda-target-tangnano9k`、`eda-cst-gowin-cst`、`eda-wave-vcd`、`eda-lib-basic`。
- 底座：`include/eda/api/`（契约）、`core/src/eda-core`（服务）、`core/src/eda-platform`（进程/动态库/SHA/平台原语）、`cmake/EdaPlugin.cmake`、`main/Composer`（组合器骨架 + `CoreJobService`）。
- 过程文档：`HarnessP0-Gate-Review.md`、`HarnessP1-Gate-Review.md`、`HarnessP1-Progress.md`、`HarnessP2-Progress.md`、`HarnessP0-ApiCoverage.md`。
- **未提交**：本会话所有改动尚未 `git commit`。

---

## 1. P0 —— 地基（内核 + 宿主 + 进程/Job 抽象）

| # | 任务 | 状态 | 说明 / 证据 |
| --- | --- | --- | --- |
| P0-1 | 建立 `include/eda/api/`（三层接口 + Types + abi + jobs + build_info） | `[x]` | 最小子集 + 扩展（`services/events/schemas/process/toolchain/project/plugin_registry/target_profile/waveform`） |
| P0-2 | 实现 `eda-core`（Context/EventBus/Logger/Error/SchemaRegistry） | `[x]` | `EventBus/SchemaRegistry/Logger/Version` + `CoreSchemas` |
| P0-3 | 实现 `PluginHost`（6 层发现 + 指纹校验 + 状态机 + 诊断） | `[x]` | `PluginHost` + `DynamicLibrary`；`eda_plugin_host_smoke` 真加载 3 插件 |
| P0-4 | 实现 `IProcessHost` + 平台插件（复活 YosysExecutor + 迁入 ProcessRunner） | `[x]` | WIN 实测（真进程/超时/取消）；**POSIX 仅移植未编译** |
| P0-5 | 实现 `IJobService`（泛化 FpgaSynthesisJobService，接 IProcessHost，补 timeoutSec） | `[x]` | `CoreJobService`/`CoreJobContext`；`eda_job_service_smoke` |
| P0-6 | 实现 `IToolchain` + `IProject` | `[x]` | `DefaultToolchain`（`FindFpgaTool` 已接入）；`JsonProject`（核心） |
| P0-7 | CMake 重构（eda_core + eda_add_plugin + 注册表生成 + 源集合对齐 + wx/JSON 归一） | `[~]` | ✅ 显式源列表/`eda_add_plugin`/静态注册表；❌ wx 归一、JSON 归一 |
| P0-8 | `MainFrame` 拆出组合器骨架；旧路径仍能跑 | `[~]` | `main/Composer`（插件维度 + `CoreJobService`）；面板组合未拆 |
| P0-9 | 清理：删 `PluginStore`/`PluginEntity`/`DeepSeek_plugin.h` | `[x]` | `PluginStore` 已删；另两项本就不存在 |

**P0 DoD**
- `[x]` 产出 `eda_core` + `SigFlow` 两个 target。
- `[ ]` 综合/仿真经 `IJobService` 提交跑通（**应用仍走 legacy `main/jobs`**）。
- `[~]` 取消真杀进程树（核心已实现并有测试；**未接到应用**）。
- `[x]` 加载失败的插件可见原因。
- `[~]` 回归：旧路径可用（构建通过；**未做 GUI 运行时回归**）。

---

## 2. P1 —— 工具链插件化

| # | 任务 | 状态 | 说明 |
| --- | --- | --- | --- |
| P1-1 | `eda-synth-yosys`（脚本生成器/ArtifactValidator 移入；输出走 jobDir） | `[x]` | 二者已移入去 wx；`eda_synth_yosys_smoke` |
| P1-2 | `eda-pnr-nextpnr`（补 NextpnrJob；LogParser/Report/CstValidator 移入） | `[x]` | LogParser/Report 已移入；CstValidator 归 P1-6 完成 |
| P1-3 | `eda-pack-gowin`（真实现 PackJob）/ `eda-program-openfpgaloader`（FlashJob + 确认门控） | `[x]` | 含 `FpgaPackService` 移入；`eda_pack_flash_smoke` |
| P1-4 | `eda-sim-verilator`（复用核心；去 vcvars；去死 DLL；补退出码） | `[x]` | sim.build+sim.run + 生成器/解析器移入 |
| P1-5 | `eda-target-tangnano9k`（解析 target-profiles JSON；删硬编码） | `[x]` | profile + 引脚库 JSON 化均接入 |
| P1-6 | `eda-cst-gowin-cst`（FpgaConstraint 移入） | `[x]` | `FpgaConstraint`+`CstValidator` 完整移入 |
| P1-7 | 声明式工具运行时（YAML → 自动生成 Job） | `[~]` | `DeclarativeTool` 已落；**用 JSON 非 YAML**；未接 MainFrame 工具发现 |
| P1-8 | UI：流程按钮接 `IJobService` + 能力下拉选择器 | `[~]` | 后端管道 + 四入口 `IJobService` 可选路径 + **能力选择 UI（Help → "Toolchain Backends" 子菜单，能力→后端单选，持久化到 `wxConfig Toolchain/DefaultBackend/<cap>`）**；**待做**：四入口新路径行为等价性 GUI 验证、默认值切换为 IJobService |
| P1-9 | 波形数据层拆 `IWaveformBackend` | `[~]` | 契约+`VcdWaveformBackend`+`TraceSource` 适配完成；**`3rd/vcd` 退役未做** |

**P1 DoD**
- `[x]` `sim/verilator ↔ icarus`（声明式）不重编译切换（`eda_declarative_switch_smoke`）。
- `[x]` 综合→布线→打包→烧录全部有 Job 记录（含 pack）。

---

## 3. P2 —— IR 与前端插件化（目标十一：算法不动）

> 硬约束（附录 E.4）：`UpdateTreeFromTS` 查询/匹配、`FormalizeExpression`、Arena 分配语义、`CompleteAutoWiring` **原样保留**。

| # | 任务 | 状态 | 说明 |
| --- | --- | --- | --- |
| P2-1 | `eda-ir`：去 `MainFrame*`、补 `DesignNodeRef(uid)`/`SignalTable`/序列化 | `[~]` | uid 字段 + `outMap` uid 键 + **去 `MainFrame*`（→`wxEvtHandler*`，解耦）已完成**；**画布 `DesignNodeRef{uid}` 未做**；物理拆包未做 |
| P2-2 | `eda-hdl-treesitter`（TSQuery + `UpdateTreeFromTS` 原样 + 真增量） | `[ ]` | 未开始 |
| P2-3 | `ICommandBus` 落地；编辑菜单真实现 | `[~]` | 契约 `command.hpp` + `CommandBus`（撤销/重做双栈）+ 单测完成；**写路径（AddChild/RemoveChild/AddWire/…）包命令 + 菜单接线未做** |
| P2-4 | `ITextDocument`（Scintilla 隔离）；画布改持 `DesignNodeRef` | `[ ]` | 未开始 |
| P2-5 | `eda-lib-basic`（`canvas_elements.json` 活路径；删 `ToolboxModel` 死路径） | `[~]` | 插件 + 解析 + 测试完成；**死路径 `ToolboxModel`/`tools.json` 已删**；`CanvasModel`/`ToolboxPanel` 接入 `IComponentLibrary` 未做 |
| P2-6 | `eda-hdl-slang` 决策：重建或先删死码 | `[x]` | **决策：删死码**。`main/` 代码级 slang + CMake 的 slang 构建/链接/cache 变量**全部移除**（`CMakeLists.txt` 仅剩说明注释） |
| P2-7 | 修 Arena UB（R11：`ClearNode` 改从 Arena 释放而非 `delete`） | `[x]` | 已复核满足（`ClearNode` 只断链 + `arena.reset()`） |
| P2-8 | 移除硬编码 `"DeepSeek_Assistant"`；旧 `ISigPlugin` 兼容层下线 | `[~]` | 宿主硬编码已消除（集中到 `Composer::LegacyAssistant`）；旧 `ISigPlugin` 兼容层下线依赖 P3 迁移 DeepSeek |

**P2 DoD**
- `[~]` 画布↔代码双向同步与现状**完全一致**（回归：全量样例往返一致）—— 已有 **`sig_tree_sync_smoke`**（parse→SFTree→`ToVerilog` 往返 + uid 稳定 + `outMap`/`NodeByUid`）；画布侧尚未覆盖。
- `[ ]` 画布删节点/删线回删源码接通（修 R12）。
- `[ ]` 编辑可撤销。
- `[ ]` `ir/snapshot` 可落盘。
- `[x]` Arena 无 UB。

**P2 前置缺口**
- `[x]` **双向同步回归样例**：`tests/ir/sig_tree_sync_smoke.cpp`（非 GUI，桩 `wxEvtHandler` 驱动 `SigFlowTree`）。

---

## 4. P3 —— 进程外 + AI/MCP

| # | 任务 | 状态 |
| --- | --- | --- |
| P3-1 | 进程外插件宿主：控制通道 JSON-RPC + 共享内存 + 降级 | `[ ]` |
| P3-2 | `eda-llm-deepseek`（网络/SSE/prompt/文件写下沉；`ISecretStore` 接管 Key） | `[ ]` |
| P3-3 | `eda-llm-mock`（离线规则引擎，教育版默认） | `[ ]` |
| P3-4 | `eda-mcp-client`（进程外；多 MCP server） | `[ ]` |
| P3-5 | 插件管理中心完整版（路径登记/健康诊断/能力分配/外部切换/构建指纹标红） | `[ ]` |
| P3-6 | 平台收口剩余项（串口/命名管道/crypto 统一） | `[ ]` |
| P3-7 | `IProject` 收口插件写方（写前审批回调；替代插件直接改 `sigflow.project`） | `[ ]` |

**P3 DoD**：`[ ]` 教育版离线可用（mock）；`[ ]` AI 审计证明"AI 从未直接拉起进程"；`[ ]` Key 不出进程、经 `ISecretStore`。

---

## 5. P4 —— 生态与分发

| # | 任务 | 状态 |
| --- | --- | --- |
| P4-1 | `sigflow plugin add/search/update`（本地/私有源） | `[ ]` |
| P4-2 | 插件一致性测试套件（`tests/plugin_conformance/`） | `[ ]` |
| P4-3 | Profile 打包发布（edu/pro/ci/minimal）+ `sigflow toolchain fetch` | `[ ]` |
| P4-4 | `.sln` 冻结下线；`main/` 目录清空 | `[ ]` |

---

## 6. §14.1 自动化验收门禁（每阶段 CI）

| # | 检查 | 状态 |
| --- | --- | --- |
| A1 | 依赖方向：`core/` 与 `InnerPlugin/` 不得包含 `MainFrame.h` | `[~]` 事实满足，**无 CI 扫描** |
| A2 | 业务代码不得含 `<windows.h>`/`<unistd.h>`（平台插件除外） | `[~]` core/插件已清零，**无 CI 扫描** |
| A3 | schema 写盘前过 `ISchemaRegistry` | `[ ]` |
| A4 | 零静默：PluginHost 失败必有结构化诊断 | `[x]` 事实满足 |
| A5 | 无密钥：CI 扫描 `sk-`/`api_key` | `[ ]` |
| A6 | 回归：`tests/fpga/yosys/*` 插件化后结果一致 | `[ ]` |
| A7 | 源集合对齐（CMake vs vcxproj） | `[~]` 显式列表已对齐 CMake；vcxproj 未同步校验 |
| A8 | 构建指纹：动态加载插件与宿主 `build_info` 一致 | `[x]` 事实满足 |

---

## 7. §2 遗留 R/H 清单状态（对照当前 HEAD）

**已处理**：R3（Key 环境变量化）、R5（取消杀树，核心）、R6（NextpnrJob/FlashJob/PackJob）、R10（`fpga/` 小写）、R11（Arena UB）、R15（仿真异步，部分）、H2/H4/H19/H20（清理/归一，部分）。

**仍待处理**：
- `[ ]` R1/R2：插件加载静默 + 宿主按名硬编码（`PluginHost` 已具备，**未替换 `PluginManager` 调用点**）。
- `[ ]` R4：三套进程执行未统一（`FpgaYosysExecutor` 仍在）。
- `[ ]` R7：`target-profiles` 已被解析（P1-5 完成）；**引脚库硬编码已 JSON 化**（P1-5 完成）。
- `[ ]` R8：`external/fpga-tools/runtime` 路径硬编码 / `PATH` 分隔符（部分）。
- `[ ]` R9：新建 `.v` 未更新 `.project`（当前 `wxID_NEW` 建整工程，无独立新增流）。
- `[ ]` R12：画布删节点不回删源码。
- `[ ]` R13：跨 DLL ABI（`PluginHost` 指纹校验已具备；旧 `ISigPlugin` 未下线）。
- `[ ]` R14：`3rd/vcd` 定容静默丢弃（新后端已非定容；**旧后端未退役**）。
- `[ ]` R16/R17/R18/R19：源集合/配置双写/菜单占位/工具发现（部分）。
- `[ ]` H1/H3/H5/H6/H7/H8/H9/H10/H11/H12/H13/H14/H16/H17/H18/H21/H22：详见 HarnessPlan §2。

---

## 8. 建议的下一个可验证步骤（不含 GUI 运行时依赖）

1. `[ ]` **补双向同步回归样例**（P2 DoD 前置）——作为画布改造的验证手段。
2. `[ ]` P2-5 `eda-lib-basic`（`canvas_elements.json` 活路径；删 `ToolboxModel` 死路径）——中风险、可构建验证。
3. `[ ]` P2-8 去硬编码 `"DeepSeek_Assistant"`——低风险、可构建验证。
4. `[ ]` P2-1 画布 `DesignNodeRef{uid}` 与 IR 物理拆包——**需 GUI 运行时验证**。
5. `[ ]` P2-3 `ICommandBus` / P2-4 `ITextDocument`——大工程，建议与同步回归样例并行。

---

## 9. 风险、约束与"未完成项"如实记录

### 9.1 环境约束（本仓库当前验证边界）

- `[!]` **仅能构建 + 非 GUI 单测**：本环境为 Windows + MinGW，**无法运行 GUI**，也无法做画布/编辑器交互验证。
- `[!]` **双向同步回归仅覆盖 SFTree 侧**：`tests/ir/sig_tree_sync_smoke` 覆盖"解析→SFTree→`ToVerilog` + 编辑增删"，**不覆盖画布**（`CanvasPanel`/事件/画布↔代码往返）。
- `[!]` **所有 GUI 项均未运行验证**（P0-8 面板组合、P1-8 控件、P2 画布/编辑器）。
- `[!]` **POSIX 后端仅移植未编译**（`eda-platform/posix`，待 Linux 真机）。
- `[!]` **未提交**：本会话改动量大，`git` 尚未 commit。

### 9.2 P2 未完成项（如实，按失败/阻塞原因归类）

> 以下项**未完成**，且**不能在当前环境验证**；强行改动会拿项目旗舰能力（双向同步）冒险，故未改动。

| 项 | 具体未完成内容 | 阻塞原因 |
| --- | --- | --- |
| P2-1 | 画布 `DesignNodeRef{uid}`：`Pin::self/top_self`、`TopModuleBox::self`、`SecondElement::self`、`Wire::self` 与**全部消费点**（`CanvasPanel`/`CanvasEventHandler`/`SFNPropertyPanel`/`MainFrame`/`Wire.cpp`/`CanvasElement.cpp`…）由裸指针改 uid + `NodeByUid` 解析（修 C2/R12） | 行为等价性需 **GUI 运行时**验证；回归不覆盖画布 |
| P2-1 | `eda-ir` 物理拆包（`SigTree`/`VerilogManager` 抽离为独立插件 `InnerPlugin/eda-ir`） | 大重构 + 需画布回归 |
| P2-2 | `eda-hdl-treesitter`：抽离 `tree_sitter_verilog` + 3 段 TSQuery + `UpdateTreeFromTS`（算法原样）+ **真增量**（H5/H22） | 需"增量 vs 全量"等价回归（当前缺失） |
| P2-3 | 把编辑写路径（`AddChild`/`RemoveChild`/`AddSignal`/`AddWire`/`DeleteWire`…，附录 E）**包成命令**并接入画布与编辑菜单（R18） | GUI 交互 |
| P2-4 | `ITextDocument`（Scintilla 隔离，C1）；画布持 `DesignNodeRef`；`Block` 改文档 offset 区间 | GUI + 大重构 |
| P2-5 | `CanvasModel`/`ToolboxPanel` 接入 `IComponentLibrary`（形状保真） | GUI 渲染验证 |

### 9.3 P2 DoD 达成情况（如实）

- `[~]` 双向同步"与现状完全一致"：**仅 SFTree 侧有回归**（`sig_tree_sync_smoke`）；画布侧未验证。
- `[ ]` 画布删节点/删线回删源码（R12）：**未接通**。
- `[ ]` 编辑可撤销：**接口已就绪**（`ICommandBus`），**写路径未包命令**。
- `[ ]` `ir/snapshot` 可落盘：**未实现**。
- `[x]` Arena 无 UB。

### 9.4 结论（P2 状态）

- **P2 可构建验证的部分已完成**：P2-1 uid/outMap/去 `MainFrame*`、P2-3 命令总线、P2-5（插件+解析+删死路径）、P2-6、P2-7、P2-8 + 同步回归样例。
- **P2 未达 DoD**：画布侧改造、真增量、命令接线、`ir/snapshot` 均未完成，且**必须在有 GUI 运行时的环境、并先补齐画布回归**后方可推进/验收。

---

## 10. 原"建议下一步"（保留）

1. `[x]` 补双向同步回归样例（SFTree 侧）。
2. `[x]` P2-5 `eda-lib-basic`（插件+解析+删死路径）。
3. `[x]` P2-8 去硬编码 `"DeepSeek_Assistant"`。
4. `[ ]` P2-1 画布 `DesignNodeRef{uid}` 与 IR 物理拆包——**需 GUI 运行时验证**。
5. `[ ]` P2-3 `ICommandBus` 写路径接线 / P2-4 `ITextDocument`——**需 GUI**。

## 11. 原"风险与前置"（保留）

- `[!]` 画布 `DesignNodeRef` 与 IR 拆包直接触碰双向同步核心，**当前画布侧无自动化回归**。
- `[!]` 未提交：建议先落检查点再继续深水区。
- `[!]` POSIX 后端仅移植未编译。
- `[!]` slang 已删死码（P2-6 完成）。
- `[!]` GUI 项在本环境无法运行时验证。
