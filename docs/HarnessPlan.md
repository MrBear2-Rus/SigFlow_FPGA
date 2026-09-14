# HarnessPlan（最终版）—— SigFlow「核心 + 插件」架构落地规划

> 版本：v2.0 FINAL（基于 2026-09 对仓库 `145f181` 的深度精读修订）
>
> 本文件回答：**现在是什么样、要改成什么样、怎么一步步改、改哪些文件、每个文件怎么改、有哪些坑。**
> 前身 `EDAHarness.md` 已弃用，以其为历史参考不再维护；**冲突处一律以本文件为准**。
>
> 本文所有文件:行号均为精读核对实据（本会话一手阅读 + 4 份只读子代理报告交叉验证），
> 不再是估算。阅读符号：`①`=代码→树→画布、`②`=画布→代码（双向同步方向）。

---

## 目录

- [0. 十二点目标 + Job 抽象 → 设计映射](#0-十二点目标--job-抽象--设计映射)
- [1. 现状架构全景（精读版）](#1-现状架构全景精读版)
- [2. 历史残留与 Bug 清单（先修什么）](#2-历史残留与-bug-清单先修什么)
- [3. 目标架构总览](#3-目标架构总览)
- [4. 三层插件接口（核心契约）](#4-三层插件接口核心契约)
- [5. 插件宿主与两类插件定位](#5-插件宿主与两类插件定位)
- [6. 统一通信：进程内 C ABI / 进程外 控制通道+共享内存](#6-统一通信进程内-c-abi--进程外-控制通道共享内存)
- [7. 数据交换三类 + 版本化 schema](#7-数据交换三类--版本化-schema)
- [8. Job 抽象（每个插件功能 = 一个具体 Job）](#8-job-抽象每个插件功能--一个具体-job)
- [9. 核心（UI 组合器）改造方案](#9-核心ui-组合器改造方案)
- [10. 模块级改造清单（逐个模块，含涉及文件与删除清单）](#10-模块级改造清单逐个模块含涉及文件与删除清单)
- [11. 构建与分发：CMake 重构 + Profile](#11-构建与分发cmake-重构--profile)
- [12. 目标目录结构](#12-目标目录结构)
- [13. 迁移路线 P0–P4（每步可编译可运行，含 DoD）](#13-迁移路线-p0p4每步可编译可运行含-dod)
- [14. 验收标准](#14-验收标准)
- [15. 风险与对策（含深挖新增隐患）](#15-风险与对策含深挖新增隐患)
- [附录 A：`InnerPlugin/` 官方插件骨架](#附录-a内插件骨架)
- [附录 B：冻结 schema 草案](#附录-b冻结-schema-草案)
- [附录 C：进程外插件控制通道协议（JSON-RPC 2.0）](#附录-c进程外插件控制通道协议json-rpc-20)
- [附录 D：插件管理中心 UI 设计](#附录-d插件管理中心-ui-设计)
- [附录 E：双向同步算法现状与最小改造（核心资产保护）](#附录-e双向同步算法现状与最小改造核心资产保护)

---

## 0. 十二点目标 + Job 抽象 → 设计映射

| 目标（原文要点） | 本规划落点 |
| --- | --- |
| 一、UI 不是插件，归核心 | §9：核心 = wxWidgets 壳 + 画布/编辑器/面板 + 组合器 + 插件管理中心；`MainFrame` 降级为组合器 |
| 二、能力全部插件化 | §3/§4：IR/解析/仿真/综合/布线/波形/AI/MCP/元件库 全部实现为 `InnerPlugin/` 下的插件 |
| 三、插件接口分三层 | §4：`IPluginInteraction` → 能力子类接口 → 具体插件 |
| 四、两类插件定位（内部集成 / 可外接） | §5：`location: inner \| external` 是**设计定位**；官方默认进程内，可外接可切进程外 |
| 五、官方插件源码统一放 `InnerPlugin/` | §5.4 / §12 / 附录 A |
| 六、插件通信统一设计 | §6：`invoke` 异步请求-响应 + `subscribe` 事件订阅 + 元信息查询；进程内 C ABI；进程外 共享内存 + 控制通道 |
| 七、数据交换分三类 | §7：控制元数据 / 日志进度 / 设计产物（大产物只传 path+schema+sha256） |
| 八、跨边界数据有版本化 schema | §7.4 / 附录 B：JobReport、DesignIR、TargetProfile、PluginManifest 先冻结 |
| 九、UI 提供插件选择 | §9.4 / 附录 D：能力下拉选择器 + 插件管理中心；失败标红不静默 |
| 十、现有工具不换，包到接口后面 | §10：Yosys/nextpnr/Verilator/gowin_pack/openFPGALoader/3rd-vcd/DeepSeek 全部保留 |
| 十一、双向同步算法不动 | §10.5 / 附录 E：`UpdateTreeFromTS` / `VerilogManager` 同步算法原样抽离，只去 UI 化 |
| 十二、构建与分发 | §11：一个核心 target + 每插件一个 target；平台差异封平台插件；Profile 组合发行版 |
| **其二、Job 抽象接口，插件功能 = 具体 Job，异步、改动小** | §8：`IJobService`（核心）+ `IJobProvider`（插件侧）+ `JobContext`；沿用 9 态状态机与 manifest 持久化，`MainFrame` 只改成提交 Job |

---

## 1. 现状架构全景（精读版）

### 1.1 一句话现状

**SigFlow 当前是"以 `MainFrame`（2937 行）为中心的星形依赖"**，几乎所有能力都挂在 `MainFrame` 或持 `MainFrame*`，
**没有中间契约层**。以下按能力域列出精读确认的事实。

### 1.2 按能力域的现状事实表（文件:行号 实证）

| 能力域 | 现状实现 | 关键证据 |
| --- | --- | --- |
| **UI 壳** | `MainFrame` 构造器直接 `new` 全部面板：CanvasNoteBook、ToolboxPanel、SigFlowTreePanel、SFNPropertyPanel、FpgaPinBindingPanel、SigTextEditor+VerilogManager、AsyncAnalysisCenter、ProjectTreePanel、TerminalCtrl、WavePanel | `MainFrame.cpp:434-560` |
| **插件加载** | `PluginManager` 扫描 `exeDir/plugins`，`LoadLibraryA`/`dlopen` + `GetProcAddress("CreateSigPlugin")`；**失败全静默**；`GetPlugin` 按 `GetName()==name` 线性查找 | `PluginManager.cpp:16-79`；`MainFrame.cpp:524-550` |
| **插件硬编码** | 四处 `GetPlugin("DeepSeek_Assistant")`；插件创建 `wxPanel` 被塞进 rightNotebook | `MainFrame.cpp:487/542/654` |
| **综合流程** | **已 Job 化**：`DoFpgaSynthesis` 走 `SynthesisJob` 状态机（Create→Validating→…→Running→ValidatingArtifact→Succeeded），异步执行 + 产物校验，双 manifest（job + runtime + artifact） | `MainFrame.cpp:2183-2380`；`FpgaSynthesisJob.*` |
| **布线流程** | **无 Job 记录**：`DoFpgaRoute` 纯 `wxExecute` 临时调用；有 nextpnr 日志解析（`NextpnrLogParser`）+ CST 自动校验（`CstValidator`）产出 `<top>.analysis.json` | `MainFrame.cpp:2382-2538` |
| **打包流程** | **从未实现**：`gowin_pack` 只存在于 README 文案，无任何 UI/代码 | `MainFrame.cpp:361` |
| **烧录流程** | **无 Job 记录、无确认门控**：`DoFpgaProgram` 弹窗选 .fs → `openFPGALoader -b <board> ${bitstream}` | `MainFrame.cpp:2540-2606` |
| **进程执行** | 三套并存：`FpgaToolProcess`（wxProcess + 75ms 定时器）、`ProcessRunner`（裸 Win32/POSIX）、`FpgaYosysExecutor`（死代码，JobObject/进程组，**最完善但从未接线**） | `MainFrame.cpp:95-176`；`Simulation/ProcessRunner.*`；`FpgaYosysExecutor.*` |
| **工具发现** | `FindFpgaTool`：配置→向上 6 层找 `external/fpga-tools/runtime/<tool>/bin`→env→PATH(`';'` 切分)；Verilator 另有 `FindVerilatorPath`/`FindSystemVerilator` 两条重复链 | `MainFrame.cpp:241-295`；`SimulationEngine.cpp:95-151`；`VerilatorRunner.cpp` |
| **目标板数据** | `tang-nano-9k.json` **全仓库 0 引用，从未被解析**；数据 100% 来自 `GetTangNano9kTargetProfile` 硬编码结构体 + `FpgaPinData::GetQFN88PinDatabase` 硬编码引脚库 | `FpgaYosysRuntime.cpp:201-227`；`FpgaPinData.cpp:182-196` |
| **IR / 双向同步** | `SigFlowTree`（Arena 分配 + DefinitionTable/InstanceTable + UpdateTreeFromTS）+ `VerilogManager`（Scintilla marker + 300ms 定时器）；详见附录 E | `SigTree.h:26-64`；`VerilogManager.h` |
| **仿真** | `SimulationEngine`：`wxExecute SYNC` 跑 verilator（阻塞 UI）→ `compile_dll.bat`（vcvars+cl）→ 生成 sim_runner.exe → 60s 超时跑 VCD | `SimulationEngine.cpp:65-93/380-435/468-980/982-1050` |
| **波形** | `WavePanel` 裸持 `vcd_t*`（无 RAII）；`3rd/vcd` 定容 32 信号×4096 跳变，超限**静默丢弃** | `WavePanel.h:42`；`vcd.h:8-11`；`vcd.cpp:196-199/310-312` |
| **AI** | `Plug_DeepSeek.cpp`（实际 1782 行）DLL：硬编码 Key + WinHTTP 流式 SSE + **直接写 src/lib 并改写 sigflow.project** + 自带整套 UI（CreatePanel） | `Plug_DeepSeek.cpp:41/570-853/947-1782/1535-1647` |
| **配置** | `sigflow.project` 读写者散落：`LoadProjectConfig`(2105-2181)、`LoadFpgaProjectOptions`(178-239)、DoFileNew(1207-1227)、AsyncAnalysisCenter(168-190)、ProjectTreePanel(33)、**插件（双写方）**；`DoFileSave` 空注释实现 | `MainFrame.cpp`；`Plug_DeepSeek.cpp:261-340` |
| **编辑/撤销** | `DoEditUndo` 空、编辑菜单 20+ 项全是 `wxMessageBox` 占位；archive 的 UndoStack 未编译 | `MainFrame.cpp:1854-1886` |
| **构建** | CMake `GLOB_RECURSE main/*.cpp` 排除 `/.sigflow/` 与 `/archive/`；**不构建任何插件**；CMake 比 vcxproj 多编 5 个文件（含死代码 FpgaYosysExecutor）；wxWidgets 双份（3rd 预编译 + FetchContent） | `CMakeLists.txt:150-221` |

### 1.3 关键耦合点（改造前必须承认的事实）

| # | 耦合点 | 证据 | 后果 |
| --- | --- | --- | --- |
| C1 | IR 反向依赖 UI | `SigFlowTree` 持 `MainFrame* m_parent` 并直接发 wx 事件（`SigTree.h:404`）；IR 头 `SigTree.h` 同时 include `wx/event.h` 与 `slang/ast/Compilation.h` | 无法脱离 UI 测试/复用 IR |
| C2 | 画布嵌入 IR 裸指针 | `SecondElement::self=SecondNode*`、`Pin::self=top_self`、`TopModuleBox::self=TopNode*`、`Wire::GetSelf` 全指向 Arena 内存 | 视图与模型无边界；Arena reset 后悬垂 |
| C3 | 前端引擎硬编码 | `tree_sitter_verilog()` 在 MainFrame/VerilogManager/SigTree/TreeSitterLinter 多处直接调用；slang 是死链路 | 换 HDL = 改多处 + 重编 |
| C4 | 工具执行三套并存且行为不一致 | `FpgaToolProcess`（无超时/取消/进程树）vs `ProcessRunner`（有 POSIX 但无超时）vs `YosysExecutor`（最完善但死） | 统一 IProcessHost 是 P0 必做 |
| C5 | 现有插件机制名不副实 + 有 ABI 缺陷 | `ISigPlugin` 5 虚函数含 `wxPanel*`；宿主按名硬编码；失败静默；**Debug|x64 下 `WXUSINGDLL`/`_ITERATOR_DEBUG_LEVEL` 与宿主不一致 → std::string/wxString 布局不匹配** | 无法承载"能力插件化"；现 ABI 本身脆弱 |
| C6 | Job 只有"形状"到"半成品" | 综合已 Job 化，但 PnR/Pack/Flash 无 Job；`Cancel/Retry/List` 零调用；`TimedOut/Cancelled` 从未触发；取消不杀进程 | 流程不可追踪、不可靠 |
| C7 | 构建无法表达插件 | `CMakeLists.txt` 无插件 target；Linux 无插件构建 → `LoadPlugins` 静默空跑 | 无法"一个插件一个 target" |
| C8 | 配置多写方 | 插件绕过宿主直接改 `sigflow.project`（`Plug_DeepSeek.cpp:335`） | 数据竞争、无审批、R9 崩溃风险 |

---

## 2. 历史残留与 Bug 清单（先修什么）

> 分类：**R（必须修，阻塞架构）** / **H（历史残留，随模块迁移顺手清）** / **S（小问题，迁移期处理）**。
> 每项绑定 §13 路线图阶段；带 🔴 的为本次深挖新确认的关键隐患。
>
> **复核标记（2026-09 对照当前 HEAD `5aacd1e`，本目录 CMake 副本）**：行首 `✅` 表示该项**已完成或已不再适用**；
> 未标记者为复核后仍待处理。⚠️ 注意本文件 §1/§2 的"现状"写于旧快照 `145f181`，仓库已大幅演进。
>
> **2026-09-13 更新（仿真收敛落地）**：R15 主项修复（wxEXEC_SYNC 全部移除、仿真三段执行统一走
> `jobs/PlatformProcess`、取消经 `JobService::Cancel` 真杀进程树、Simulate 菜单新增 Cancel Simulation、
> 新增 `tests/job_tests` 回归基座）；H4 的 `VerilatorRunner`/`ProcessRunner` 已删除。
> R15 残留：DLL 死产物是否删除/可选化待决策；`GetSoftwareDirectory` 打包后失效未动。
> 遗留：`YosysExecutor`/`NextpnrExecutor` 的手写 Win32 进程管理与 `PlatformProcess` 重复，待 Phase 2 收敛。

### R 类：阻塞架构的必须修复项

| # | 问题 | 证据 | 阶段 |
| --- | --- | --- | --- |
| R1 | 插件加载失败**静默** | `PluginManager.cpp:34-37/62-69` | P0 |
| R2 | 宿主**按名字硬编码**插件 | `MainFrame.cpp:487/542/654` | P0 |
| ✅ R3 | **硬编码 API Key** 明文入源码/Git（安全事件，需轮换） | `Plug_DeepSeek.cpp:41` | P3 |
| R4 | 三套进程执行并存 + 死代码 `YosysExecutor` 未接线 | `FpgaYosysExecutor.*`（最完善但零引用） | P0 |
| ✅ R5 | 取消只改状态不杀进程；`TimedOut` 无人触发 | `FpgaSynthesisJob.cpp:280-287`（注释"前提：调用方已终止实际进程"，实际无调用方）；`MainFrame.cpp` 无任何 kill 调用 | P0 |
| ✅ R6 | `NextpnrJob`/`FlashJob` 不存在；`gowin_pack` 打包从未实现 | `MainFrame.cpp:2382-2606` | P1 |
| R7 | `target-profiles/tang-nano-9k.json` 从未被解析，数据 100% 硬编码 | 全仓 grep "target-profiles" 0 命中；`FpgaYosysRuntime.cpp:201` | P1 |
| R8 | 硬编码依赖 `external/fpga-tools/runtime`（该目录不存在）；`PATH` 用 `';'` 切分；`.exe` 后缀硬编码 | `MainFrame.cpp:255-259/280/2283` | P0 |
| R9 | 新建 `.v` 不更新 `.project` → `SetFileNode(nullptr)` 崩溃 | `ChangeLog.md`；`MainFrame.cpp:1869-1876` | P1 |
| ✅ R10 | `main/fpga/` 与 `main/Fpga/` 大小写冲突目录 | 目录实况（Linux 同目录） | P0 |
| R11 🔴 | **Arena UB：`ClearNode` 用 `delete child` 释放 placement-new 在 `char[]` 块上的内存** = 未定义行为/堆破坏 | `SigTree.cpp:884`；`SigTree.h:26-64` | P2 |
| R12 🔴 | **双向同步单向缺失：画布删节点/删线不回删源码**（`OnSFNodeDeleted` 不调 verilogMgr；`SigFlowNodeDeleted` 空壳） | `MainFrame.cpp:2081`；`VerilogManager.cpp:592-594` | P2 |
| ✅ R13 🔴 | **wx/std::string 跨 DLL ABI 不匹配**：插件 Debug|x64 丢 `WXUSINGDLL`、`_ITERATOR_DEBUG_LEVEL` 空值 vs 宿主 =2 → 布局不一致 | `Plugin_DeepSeek.vcxproj:120` vs `main.vcxproj:231` | P0 |
| R14 🔴 | **vcd 定容静默丢弃**（信号/跳变超限丢弃、名字截断）+ `WavePanel` 裸 `vcd_t*` 无 RAII | `vcd.cpp:196-199/310-312`；`WavePanel.h:42` | P1 |
| ✅ R15 🔴 | 仿真链：`wxExecute SYNC` 阻塞 UI；DLL 是**死产物**（编译后从不加载只当缓存标记）；`sim_runner` 恒 `return 0`（错误靠 VCD 存在性兜底）；`GetSoftwareDirectory` 打包后失效 | `SimulationEngine.cpp:73/682-686/758-762/1069-1106` | P1 |
| R16 🔴 | CMake 与 vcxproj 源集合漂移：CMake 多编 5 个文件（含死代码）、Linux 无插件构建 | `CMakeLists.txt:150-161`；vcxproj 对比 | P0 |
| ✅ R17 🔴 | 配置双写方：插件绕过宿主改 `sigflow.project`（无审批/无原子性） | `Plug_DeepSeek.cpp:335/261-340` | P3 |
| R18 🔴 | 编辑/项目/仿真菜单 30+ 项是 `wxMessageBox` 占位（**功能实际缺失**，非仅撤销） | `MainFrame.cpp:1854-1886/1888-1898` | P2 |
| R19 🔴 | `FindVerilatorPath` 默认返回 `"verilator_bin"` → "找不到"分支不可达，找不到时静默执行失败 | `SimulationEngine.cpp:151` | P1 |

### H 类：随模块迁移顺手清理

| # | 问题 | 证据 | 处置 |
| --- | --- | --- | --- |
| H1 | `PluginStore.{h,cpp}` 空壳（.h 仅 `#pragma once`，.cpp 0 行） | 文件实况 | P0 删除或实现为清单缓存 |
| ✅ H2 | `DeepSeek_plugin.h/.cpp`（MyDsPlugin）孤儿、`#include ""` 非法、未构建 | `Plugin_DeepSeek/` | P0 删除 |
| H3 | slang 死链路：`UpdateTreeFromSlang` 空壳；`PushTask` 全仓无调用；`AsyncAnalysisCenter::Entry` 的 slang/LogicBridge 在 `/* */` 注释块 | `SigTree.cpp:818-820`；`AsyncAnalysisCenter.cpp:88-151` | P2 决策 |
| ✅ H4 | `VerilatorRunner` 死代码 + 三条重复工具发现链 | `Simulation/VerilatorRunner.*` | P1 删除，发现链并入 IToolchain |
| H5 | `TreeSitterLinter::Lint` 假增量 + `GetLineStatus→StructureTest` 每次全量解析 + `OnTimer` 每 300ms 都调 | `TreeSitterLinter.cpp:32-44`；`VerilogManager.cpp:597-613` | P2 |
| H6 | `GenerateFileContent()` 整段 Logisim XML 注释返回 `""`；`DoFileOpen` 大段注释 | `MainFrame.cpp:1302-1366` | P2 |
| H7 | `Command.h` 空；`DoEditUndo`/`OnUndoStackChanged` 空壳；archive 的 UndoStack 未编译且依赖旧 API | `MainFrame.cpp:1854-1856/1967-1981`；`archive/UndoStack.cpp:10-16` | P2 落地 ICommandBus |
| H8 | `3rd/vcd` 定容 + 无 LICENSE；`vcd.cpp` 静默丢弃 | `vcd.h/vcd.cpp` | P1 |
| H9 | `my_log.h`：Release 下 no-op（生产无日志） | `main/my_log.h:30` | P0 换结构化 Logger |
| H10 | 日志落 `sigflow.log` 于仓库根 | 文件实况 | P0 改 `~/.sigflow/logs/` |
| H11 | 硬编码 `"\\"` 路径分隔符 | `MainFrame.cpp:2191/2328/2436`、`FpgaSynthesisJob.cpp:214`、`SimulationEngine.cpp` | P1 换 `std::filesystem` |
| H12 | 全局 `g_elements` + `using json` 混用（CanvasPanel.cpp 用 nlohmann、MainFrame.h 等 10 文件用 jsoncpp、插件用 nlohmann） | `main/CanvasModel.cpp:9`；`CanvasPanel.cpp:14` | P2 归一 + 收进 IComponentLibrary |
| H13 | `PATH` 用 `';'` 切分 | `MainFrame.cpp:280` | P0 平台分隔符 |
| H14 | `.exe` 后缀硬编码 | `MainFrame.cpp:2283/2428/2562` | P0 平台插件提供 |
| ✅ H15 | 分支名 `FPGA_Enginerring` 拼写错误 | `git branch` | S |
| H16 | `TerminalController::submitLine` 只有 help/echo 桩 | `TerminalController.cpp:30-45` | S |
| H17 | 魔法事件 id（`wxID_HIGHEST+N` 遍布菜单）；自定义事件 `EVT_SIGFLOWNODE_*` 混用 ProcessEvent 同步 + wxPostEvent/QueueEvent 异步 | `MainMenuBar.cpp:8-73`；`SigTree.cpp:1259/1277/1219` | S 随命令总线收敛 |
| H18 🔴 | `ToolboxModel`/`tools.json` 整套死代码：`ToolboxPanel.cpp` **硬编码建树**（300-483），`GetMainElements` 恒空 `{}`；`canvas_elements.json` 才是活路径 | `ToolboxPanel.cpp:300-483`；`ToolboxModel.cpp` | P2 |
| ✅ H19 🔴 | `PluginEntity/` 空目录 | 目录实况 | P0 删除 |
| ✅ H20 🔴 | wxWidgets 双份（`3rd/wxWidgets-3.2.9` 预编译 + FetchContent 各一，均 v3.2.9） | 目录实况；`CMakeLists.txt:29-41` | P0 归一 |
| ✅ H21 | archive 旧版 = **纯内存门级事件仿真**（另一种范式，与外部 Verilator 工具链不可比），仅历史参考 | `archive/SimulationEngine.cpp:1-418` | S 归档保留，不迁 |
| H22 | 每次重解析 `old_tree=nullptr` **非增量**；`m_ts_tree` 全量结果从不被查询（纯浪费）；`Lint` 每次新建即删树 | `VerilogManager.cpp:95-96/143-144`；`TreeSitterLinter.cpp:32-44` | P2 改真增量 |

---

## 3. 目标架构总览

### 3.1 分层与职责

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 核心（一个可执行 + 若干静态库）—— UI 归这里，不是插件                         │
│  ┌────────────────────────────────────────────────────────────────────┐   │
│  │ UI 壳（wxWidgets）：MainFrame→组合器、画布、编辑器、面板、            │   │
│  │ 插件管理中心、能力下拉选择器                                            │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│  ┌────────────────────────────────────────────────────────────────────┐   │
│  │ 组合层：流程编排（仿真→综合→布线→打包→烧录）+ 能力选择 + Job 调度        │   │
│  │ 只认 §4 第二层抽象接口，不认任何具体插件                                │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│  ┌────────────────────────────────────────────────────────────────────┐   │
│  │ eda-core 内核：Context / 服务注册表 / 事件总线 / PluginHost          │   │
│  │ IJobService / IProcessHost / ISchemaRegistry / IToolchain /        │   │
│  │ IProject / ILogger / ISecretStore / ICommandBus / ITextDocument    │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│  ┌────────────────────────────────────────────────────────────────────┐   │
│  │ 平台插件：eda-platform-win32 / posix（唯一允许 #ifdef/windows.h/      │   │
│  │ unistd.h；进程/串口/路径/后缀/分隔符）                                │   │
│  └────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────────┘
                     ▲ 核心只依赖：抽象接口头文件（include/eda/api/）
                     │
┌────────────────────┴───────────────────────────────────────────────────────┐
│ InnerPlugin/ —— 官方插件源码（每个插件一个子目录，实现第二层接口）             │
│  eda-ir  eda-hdl-treesitter  eda-hdl-slang(重建)  eda-sim-verilator        │
│  eda-synth-yosys  eda-pnr-nextpnr  eda-pack-gowin  eda-program-openfpgaloader │
│  eda-wave-vcd  eda-target-tangnano9k  eda-cst-gowin-cst  eda-lib-basic     │
│  eda-llm-deepseek  eda-llm-mock  eda-mcp-client                             │
│  ← 官方版本：静态库链接进核心（进程内）；可外接插件允许用户外部实现（进程外）    │
└────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 三条铁律

1. **核心不认识任何具体工具**：核心代码里不出现 `yosys`/`verilator`/`nextpnr` 字符串（除配置值）；只出现 `eda/synth`、`sim/verilator` 这类能力 id。
2. **业务代码零 `#ifdef`**：平台差异全在 `eda-platform-*`（现状 `SimulationEngine.cpp` 用 `wxExecute` 编译通过但 Windows 路径硬编码、Linux 运行失效——正是要根治的形态）。
3. **UI 不依赖插件、插件不依赖 UI**：插件经 `subscribe`/`invoke` 通信；核心渲染插件输出。

---

## 4. 三层插件接口（核心契约）

> 头文件统一放 `include/eda/api/`。这是核心与插件之间**唯一**的编译期契约。
> 关键设计：**第二层接口是 `IPluginInteraction` 的类型化门面**——进程内插件可覆写类型化方法直调；
> 进程外插件只实现 `invoke()`（JSON-RPC 方法集），默认实现自动翻译。

### 4.1 第一层：`IPluginInteraction`（统一调用方式）

```cpp
// include/eda/api/IPluginInteraction.h
#pragma once
#include <eda/api/Types.h>   // Error, Json(nlohmann), Callback, PluginInfo ...

namespace eda {

struct MethodCall { std::string method; Json params; std::string requestId; };

struct PluginInfo {
    std::string id, version, displayName, vendor;
    std::string location;                 // "inner" | "external"（设计定位）
    std::string runtime;                  // "inprocess" | "process"（当前形态）
    std::vector<std::string> capabilities; // ["sim/verilator"]
    std::vector<std::string> methods, topics;
    std::vector<std::string> platforms;
    int abi = EDA_PLUGIN_ABI_VERSION;
    std::string buildInfo;                // 构建指纹（编译器/迭代级别），用于 R13 类问题探测
};

class IPluginInteraction {
public:
    virtual ~IPluginInteraction() = default;
    virtual void invoke(const MethodCall&, Callback<Error, Json> onReply) = 0; // 异步
    virtual std::uint64_t subscribe(const std::string& topic, EventHandler<const Json&>) = 0;
    virtual void unsubscribe(std::uint64_t) = 0;
    virtual PluginInfo info() const = 0;
    virtual void onLoad() {} virtual void onUnload() {}
};
} // namespace eda
```

**调用语义统一为异步**：超时/崩溃/取消在接口层只有一种表达（`onReply(Error{TimedOut/Crashed/Cancelled}, ...)`）。
**对抗 R13**：`PluginInfo.buildInfo` 记录编译器与迭代级别；`PluginHost` 加载时校验宿主与插件构建指纹一致，不一致拒绝加载并标红（替代现有"Debug 下跨 DLL 传 std::string 崩溃"的隐患）。

### 4.2 第二层：能力抽象接口（`IPluginInteraction` 的子类）

```cpp
// include/eda/api/capabilities.h
#define EDA_TYPED_METHOD(method, MethodParams, MethodResult)                    \
    virtual void method(const MethodParams& p, Callback<Error, MethodResult> cb) \
    { invoke({ .method = #method, .params = toJson(p) },                        \
             [cb](Error e, Json r) { cb(e, fromJson<MethodResult>(r)); }); }    \
    static constexpr const char* k##method = #method;

class IHDLFrontend      : public IPluginInteraction { /* parse/elaborate; languageId() */ };
class ISimulator        : public IPluginInteraction { /* sim.build/sim.run; simulatorId() */ };
class ISynthesizer      : public IPluginInteraction { /* synth.run; synthesizerId() */ };
class IPlaceAndRouter   : public IPluginInteraction { /* pnr.run; pnrId() */ };
class IPacker           : public IPluginInteraction { /* pack.run */ };
class IProgrammer       : public IPluginInteraction { /* flash.run; requiresConfirm() */ };
class IDesignIR         : public IPluginInteraction { /* ir.query/ir.snapshot */ };
class IWaveformBackend  : public IPluginInteraction { /* wave.open/wave.slice */ };
class ILlmProvider      : public IPluginInteraction { /* llm.chat; available() */ };
class IMcpClient        : public IPluginInteraction { /* mcp.tools/mcp.invoke */ };
class IComponentLibrary : public IPluginInteraction { /* lib.list/lib.layout */ };
// 完整宏展开见 EDAHarness v1 的 capabilities.h；此处从略（接口名与方法名冻结）
```

### 4.3 第三层：具体插件（略，见附录 A 骨架）

### 4.4 使用者视角（核心/UI 只认第二层）

```cpp
ISimulator* sim = pluginHost().select<ISimulator>("sim");   // 按能力选
sim->sim_build(p, [this](Error e, SimBuildResult r) {
    if (e) { statusBar().ShowError(e.message, e.hint); return; }
    jobQueue().enqueue(r.jobId);
});
```

---

## 5. 插件宿主与两类插件定位

### 5.1 两类插件定位（设计定位，非运行时属性）

| 类别 | 设计定位 | 官方默认 | 更换方式 |
| --- | --- | --- | --- |
| **内部集成插件** | 必须进程内深度集成（指针级共享），如 IR、HDL 解析、波形渲染 | 编译进核心（静态库） | 覆写 `InnerPlugin/<name>/` 重编 |
| **可外接插件** | 接口设计上允许外部实现，如 仿真/综合/布线/打包/烧录/AI/MCP | 也编译进核心（进程内） | UI 里选"外部"填路径，进程外运行；不动代码 |

### 5.2 插件宿主（`PluginHost`，重写 `PluginManager`）

```
发现（6 层）→ 校验（manifest/abi/构建指纹/平台/依赖）→ 注册 → 激活 → 运行
                                                      │
                                    ┌──────────────────┴──────────────┐
                                    ▼                               ▼
                    内部集成（静态链接注册表）            可外接·外部实现
                    EDA_REGISTER_PLUGIN(id,factory)      C ABI dlopen 或 进程外 JSON-RPC
                    编译期生成 plugin_registry.cpp
```

- **静态注册表**：官方插件 `EDA_REGISTER_PLUGIN` 收集进编译期生成的 `plugin_registry.cpp`，零动态加载、零 ABI 风险（**天然规避 R13**）。
- **动态加载（C ABI）**：仅第三方原生插件走冻结 `eda_plugin_query_v1` + **构建指纹校验**（对抗 R13）。
- **进程外**：清单 `runtime: process` 由宿主拉起，JSON-RPC（附录 C），宿主侧生成 `IXXX` 适配器。
- **状态机**：`Discovered→Validated→Resolved→Loaded→Ready / Error{reason}`，失败进管理中心标红（附录 D），绝不静默（修 R1）。
- **发现链**：`bundled(<exeDir>/plugins) → Profile 声明的 bundle → 用户目录(~/.sigflow/plugins) → 项目 .sigflow/plugins → SIGFLOW_PLUGIN_PATH → 手动添加路径`。

### 5.3 能力选择器（UI）

```cpp
std::vector<PluginInfo> pluginHost().providersOf("sim");
PluginInfo chosen = settings().defaultPlugin("sim");
// 选择写回 settings() + 项目 .sigflow/edaharness.yml
```

### 5.4 `InnerPlugin/` 目录约定（每插件一个子目录）

```
InnerPlugin/
├── eda-ir/  eda-hdl-treesitter/  eda-hdl-slang/(P2 重建)  eda-sim-verilator/
├── eda-synth-yosys/  eda-pnr-nextpnr/  eda-pack-gowin/  eda-program-openfpgaloader/
├── eda-wave-vcd/  eda-target-tangnano9k/  eda-cst-gowin-cst/  eda-lib-basic/
└── eda-llm-deepseek/  eda-llm-mock/(P3)  eda-mcp-client/(P3)
```

---

## 6. 统一通信：进程内 C ABI / 进程外 控制通道+共享内存

### 6.1 进程内：冻结 C ABI（只用于动态加载边界）

编译进核心的官方插件不需要 C ABI。C ABI 服务于动态加载原生插件：

```cpp
// include/eda/api/abi.h
#define EDA_PLUGIN_ABI_VERSION 1u
extern "C" EDA_API const eda_plugin_descriptor_v1* eda_plugin_query_v1(std::uint32_t hostAbi);
struct eda_plugin_descriptor_v1 {
    std::uint32_t struct_size, abi_version;
    const char* id, *version, *manifest_json;
    const char* build_info;                       // ← 编译器/迭代级别指纹（对抗 R13）
    void (*register_plugin)(eda_host_api_v1*, void** out_inst);
    void (*unregister_plugin)(void*);
};
struct eda_host_api_v1 { /* ctx/query_service/on_event/emit_event/log/free_string ... */ };
```

**规则**：跨动态库边界不传 STL/wx；字符串一律 `const char*`；内存归属明确；版本与构建指纹不匹配拒绝加载并标红。

### 6.2 进程外：控制通道 + 共享内存（两者配合）

```
┌────────────┐       控制通道（JSON-RPC 2.0，stdio/UDS/named pipe）   ┌────────────┐
│   核心      │ ◄──── 握手/心跳/invoke/subscribe/取消/配置/TargetProfile ──► │  插件进程   │
│  (宿主侧    │       密钥引用                                          │            │
│  适配器)    │         共享内存（零拷贝：波形/IR 快照/网表）              │            │
│            │ ◄──── sigflow-shm-<id>-<seq> + 控制通道通知"就绪" ────► │  写大产物   │
└────────────┘   + 分配失败降级 base64 或临时文件传 path                 └────────────┘
```

**核心原则（目标七）**：大产物不塞 RPC 通道。通道压力只与消息数量有关，与数据体量无关。

---

## 7. 数据交换三类 + 版本化 schema

### 7.1 三类数据

| 类别 | 特征 | 通道 | 保护 |
| --- | --- | --- | --- |
| 控制与元数据 | 小、实时：握手/Job/取消/心跳/配置/TargetProfile/密钥引用 | 控制通道 | 直接传输 |
| 日志与进度 | 中等、持续：流式 | 控制通道（notification） | **必须限流**（同源合并/时间窗聚合），超限截断落盘 |
| 设计产物 | 大：网表/位流/波形/IR 快照 | 落盘 或 共享内存 | 只传 `path+schema+sha256` |

### 7.2 产物登记（所有插件统一）

```cpp
struct Artifact { std::string id; std::filesystem::path path;
                  std::string schema; std::string sha256; std::string role; };
// JobContext::registerArtifact 统一登记 → JobReport.artifacts
```

### 7.3 冻结 schema（P0 先冻结，集中注册与校验）

| schema id | 内容 | 对应现状 |
| --- | --- | --- |
| `eda.plugin-manifest.v1` | 插件清单（§5.1 示例） | 无 |
| `eda.job.v1` | Job 记录 + 状态迁移 | `FpgaSynthesisJob.cpp` 的 manifest.json 升级 |
| `eda.jobreport.v1` | 执行报告 | 各工具手写 schema 收敛（含 `NextpnrRunRecord`） |
| `eda.target-profile.v1` | 目标板 | `tang-nano-9k.json` 升级（**终于被解析**，修 R7） |
| `eda.ir.*.v1` | IR 快照 | 无 |
| `eda.netlist.yosys-json.v1` | Yosys 网表 | 现有产物标注 schema |

---

## 8. Job 抽象（每个插件功能 = 一个具体 Job）

### 8.1 设计目标

1. 核心只认 `IJobService` + `IJobProvider`，不认识具体工具。
2. 每插件功能 = 具体 Job：`synth`/`pnr`/`pack`/`flash`/`sim.build`/`sim.run`（补齐缺失的 pnr/pack/flash）。
3. 异步、正常运转：提交即返回 jobId；进度/日志流式；取消真杀进程树；超时真生效。
4. **改动要小**：沿用现有 9 态状态机、沿用 `FpgaSynthesisJobService` 持久化；`MainFrame` 调用点只改"提交 Job"。

### 8.2 接口

```cpp
// include/eda/api/jobs.hpp
enum class JobState { Created, Validating, Queued, Running, ValidatingArtifact,
                      Succeeded, Failed, Cancelled, TimedOut };   // 与现有枚举一致

struct JobRequest { std::string jobType, pluginId, projectId;
                    Json params; bool requireConfirm = false; };
struct JobRecord { std::string id, retryOf; JobRequest request; JobState state;
                   std::vector<JobTransition> transitions;
                   std::string createdAt, updatedAt; int exitCode = 0;
                   std::string traceId; std::optional<int> timeoutSec; };

class IJobService : public Service {
public:
    virtual std::string submit(const JobRequest&) = 0;
    virtual bool cancel(const std::string& jobId, const std::string& reason) = 0;  // 真杀树
    virtual bool retry(const std::string& jobId, std::string& outNewId) = 0;
    virtual std::optional<JobRecord> get(const std::string& jobId) = 0;
    virtual std::vector<JobRecord> list(const std::string& projectId) = 0;
    virtual JobReport report(const std::string& jobId) = 0;
    virtual void setConcurrency(std::size_t n) = 0;
};

class IJobProvider {
public:
    virtual std::string jobType() const = 0;
    virtual Json paramsSchema() const = 0;   // AI/UI 表单唯一依据
    virtual Json resultSchema() const = 0;
    virtual void startJob(const JobRequest&, JobContext& ctx) = 0;  // worker 线程池执行
};

class JobContext {
public:
    virtual void log(const std::string& line, bool isError) = 0;      // 内部限流
    virtual void progress(int percent, const std::string& status) = 0;
    virtual bool cancelled() const = 0;                               // 协作式检查点
    virtual void registerArtifact(const Artifact&) = 0;               // 自动 sha256
    virtual void emitMetric(const Json&) = 0;
    virtual std::filesystem::path jobDir() const = 0;                 // runs/<id>/{inputs,scripts,logs,artifacts,reports}
    virtual IProcessHost& processHost() = 0;                          // 唯一拉起进程入口
};
```

### 8.3 统一状态机（沿用现有合法迁移表，修缺口）

```
Created → Validating → Queued → Running → ValidatingArtifact → Succeeded
   │           │          │        │  │            │
   └───────────┴──────────┴────────┴──┴────────────┴──→ Failed / Cancelled / TimedOut
```

- 迁移表 = `FpgaSynthesisJobService::IsLegalTransition`（`FpgaSynthesisJob.cpp:322-338`）升级为通用表。
- **修缺口**：现有表 `ValidatingArtifact→Succeeded|Failed`（无 TimedOut/Cancelled 重入）；终态无重入（`retry` 单独处理）。通用表补：`ValidatingArtifact→Cancelled|TimedOut`；`Running→TimedOut`（接超时）。
- **修 R5**：`cancel` = 状态置 Cancelled + `JobContext::processHost().cancel()` 杀进程树。
- **修 R5**：`timeoutSec` 由 `IJobService` 统一计时 → 超时 → `Running→TimedOut`。

### 8.4 现有代码 → Job 的改动量评估（尽量小）

| 现有代码 | 改动方式 |
| --- | --- |
| `main/FpgaSynthesisJob.{h,cpp}` | 枚举/记录/迁移表**保留**；`FpgaSynthesisJobService` 变为 `IJobService` 持久化后端（Load/List/Transition 几乎原样搬）；`Create` 泛化为 `submit` |
| `MainFrame::DoFpgaSynthesis`（2183-2380） | 流程代码移入 `eda-synth-yosys::startJob`；MainFrame 只留"收集参数→submit→订阅"（约 30 行） |
| `MainFrame::DoFpgaRoute`（2382-2538） | 移入 `eda-pnr-nextpnr`（**补 NextpnrJob**）：复用 NextpnrLogParser/CstValidator 逻辑 |
| `MainFrame::DoFpgaProgram`（2540-2606） | 移入 `eda-program-openfpgaloader`（**补 FlashJob** + requireConfirm 门控） |
| `gowin_pack`（从未实现） | **在 `eda-pack-gowin` 真实现 PackJob**（R6 修复） |
| `FpgaYosysScriptGenerator.*` | 原样移入 `eda-synth-yosys`，输出目录改 `JobContext::jobDir()` |
| `fpga/ArtifactValidator.*` | 原样移入 `eda-synth-yosys`，产物走 `registerArtifact` |
| `Fpga/NextpnrLogParser.*`/`NextpnrReport.*` | 原样移入 `eda-pnr-nextpnr` |
| UI 反馈 | 订阅 `job/state`、`job/output` → 现有 Job 队列面板/Terminal 渲染 |

### 8.5 各 Job 的 params / 产物 / schema 建议（可落地）

| Job | params | 产物 | schema |
| --- | --- | --- | --- |
| `synth` | `{source_files[], top_module, strategy, family}` | `<top>.json` + `<top>.manifest.json` + runtime-manifest.json | `eda.netlist.yosys-json.v1` |
| `pnr` | `{netlist, device, family, cst, extra_args[]}` | `<top>.pnr.json` + `<top>.analysis.json` | 复用 NextpnrRunRecord JSON |
| `pack` | `{pnr_json, device}` | `<top>.fs` | `eda.bitstream.apicula.v1` |
| `flash` | `{bitstream, board, extra_args[]}` | 无（设备烧写） | — |
| `sim.build` | `{top_module, verilog_files[], project_root, testbench(空=自动匹配), extra_flags, timeout_ms, stages}` | objDir/dll/exe/simMain/vcd/compileLog + verilatorVersion | — |
| `sim.run` | `{sim_exe, vcd_out, duration_ns}` | wave.vcd | — |

**仿真 Job 细化（R15 修复点）**：DLL 编译步骤建议**删除或可选化**（产出是死产物）；`sim_runner` 补真实退出码传递；VCD 路径参数化。

---

## 9. 核心（UI 组合器）改造方案

### 9.1 `MainFrame` 拆分（2937 行 → 三个文件）

| 现状 | 去向 |
| --- | --- |
| 构造器 `new` 全部面板（434-560） | `core/eda-ui/Composer.cpp`：按 UI 配置创建面板 |
| 菜单事件转发 `DoXxx` | `ICommandBus`（P2）；命令可 Job 化 |
| `DoFpgaSynthesis/DoFpgaRoute/DoFpgaProgram/DoSimCompile/DoSimRun` | 薄封装：参数→`submit`→订阅（§8.4） |
| `FindFpgaTool/LoadFpgaProjectOptions/LoadProjectConfig` | `IToolchain`/`IProject` 服务（P0） |
| `FpgaToolProcess`（95-176） | 删除，统一 `IProcessHost`（P0） |
| 插件装载段（520-556） | `PluginHost` 接管（P0） |

### 9.2 核心新增服务（P0 实现）

| 服务 | 职责 | 取代 |
| --- | --- | --- |
| `Context`/`ServiceRegistry` | DI + 服务查找 | 分散全局/单例 |
| `EventBus` | 主题订阅发布 | `wxCommandEvent` 硬编码事件 |
| `PluginHost` | 发现/校验/注册/激活/状态机 | 重写 `PluginManager` |
| `IJobService` | Job 调度 | 泛化 `FpgaSynthesisJobService` |
| `IProcessHost` | 统一进程：异步/杀树/超时/日志上限 | 三套合一；代码取自 `YosysExecutor` |
| `ISchemaRegistry` | schema 注册与校验 | 新建（nlohmann-json-schema） |
| `IToolchain` | 工具发现链三合一 | `FindFpgaTool` + 两条 Verilator 链 |
| `IProject` | 工程模型 + `.project` 原子更新 | `LoadProjectConfig` 等 + 插件双写方 |
| `ILogger` | 结构化日志 | `my_log.h` |
| `ISecretStore` | 密钥按引用存取 | 硬编码 Key |
| `ICommandBus` | 编辑命令 + 撤销重做（P2） | 空 `Command.h` |
| `ITextDocument` | 编辑器抽象（P2） | Scintilla 直依赖 |

### 9.3 事件主题（P0 冻结）

```
project/opened  project/changed  ir/dirty  ir/rebuilt
job/created  job/state  job/output  job/progress  job/finished
artifact/produced  plugin/status  toolchain/changed
selection/changed  agent/message  agent/decision
```

### 9.4 能力选择器 UI（目标九）

- 流程按钮旁下拉框：`仿真: [Verilator ▾]`、`综合: [Yosys ▾]`、`布线: [nextpnr ▾]`。
- 数据源 `pluginHost().providersOf(capability)`；含外部插件（选"外部…"填路径）。
- 切换写回 `settings().defaultPlugin` + 项目 `.sigflow/edaharness.yml`。
- 加载失败的插件：管理中心标红 + 原因（含构建指纹不匹配，R13 的可视化）。

### 9.5 插件管理中心（附录 D）

页签 `[插件] [工具链] [元件库]`；插件页=已发现列表 + 添加/移除/禁用/启用 + 健康诊断；工具链页=可执行路径（自动发现优先，浏览兜底，兼容 `sigflow.project` 的 `fpga.*_path`）。

---

## 10. 模块级改造清单（逐个模块，含涉及文件与删除清单）

> 格式：**模块 → 目标插件/服务**；保留/移动/删除/新增。

### 10.1 插件系统 → `PluginHost` + `InnerPlugin/`

| 文件 | 处置 |
| --- | --- |
| `plugin_sdk/ISigPlugin.h` | 由 `include/eda/api/IPluginInteraction.h` + `abi.h` 取代（P0 内保留兼容适配层） |
| `main/PluginManager.{h,cpp}` | 重写为 `PluginHost`（修 R1/R2/R13） |
| `main/PluginStore.{h,cpp}` | 删除（H1） |
| `PluginEntity/` | 删除空目录（H19） |
| `Plugin_DeepSeek/` | 拆解（见 10.11）：网络/SSE/prompt/文件写 → `eda-llm-deepseek`；UI/状态机 → 核心 `AgentPanel`；删孤儿 `DeepSeek_plugin.h`（H2）；删硬编码 Key（R3） |

### 10.2 工具发现 → `IToolchain`（修 R8/R19/H13/H14）

- 三合一：`FindFpgaTool`(MainFrame.cpp:241) + `FindVerilatorPath`(SimulationEngine.cpp:95-151，**含 R19 逻辑缺陷**) + `FindSystemVerilator`(VerilatorRunner)。
- 发现链：`配置 → Profile 工具链 bundle → bundled runtime → SIGFLOW_* env → PATH(平台分隔符)`。
- 可执行名后缀、PATH 分隔符由平台插件提供。
- **修 R19**：IToolchain 的"未找到"必须返回明确的 NotFound 原因（不能有"默认返回 `verilator_bin`"这类掩盖分支）。

### 10.3 进程执行 → `IProcessHost`（P0，修 R4/R5/R15）

| 实现 | 处置 |
| --- | --- |
| `FpgaToolProcess`（MainFrame.cpp:95-176） | **删除**（无超时/取消/进程树，回调在 UI 线程） |
| `Simulation/ProcessRunner.{h,cpp}` | **整体迁入** `eda-platform`（其 POSIX fork/exec+poll 分支已实现，仅补参数化超时与 argv 转义） |
| `FpgaYosysExecutor.{h,cpp}`（死代码） | **复活为主实现**：JobObject/KILL_ON_JOB_CLOSE/TerminateJobObject（Win32）与 fork/setpgid/SIGTERM→SIGKILL（POSIX）分支直接成为 `IProcessHost` 实现；补 Linux 测试、UTF-8 容错、UI 取消按钮 |

### 10.4 Job → `IJobService` + 插件 Job（§8，修 R5/R6）

见 §8.4。核心资产保留：9 态枚举、迁移表、manifest 持久化、`FpgaYosysScriptGenerator`、`ArtifactValidator`、`NextpnrLogParser/Report`、`CstValidator`。

### 10.5 IR 与解析 → `eda-ir` + `eda-hdl-treesitter`（P2，目标十一：算法不动）

| 文件 | 处置 |
| --- | --- |
| `main/SigTree.{h,cpp}` | 拆两份：① `eda-ir`：树/Arena/节点/ToVerilog/符号表，`MainFrame*`→`Context&`，**同步算法原样**；② 补 `DesignNodeRef`(uid)/SignalTable/序列化（附录 E） |
| `main/VerilogManager.{h,cpp}` | 拆两份：同步算法原样进 `eda-hdl-treesitter`；Scintilla 依赖经 `ITextDocument` 隔离（附录 E） |
| `main/TreeSitterLinter.{h,cpp}` | 进 `eda-hdl-treesitter`，修假增量（H5/H22：复用旧 TSTree） |
| `main/AsyncAnalysisCenter.*`/`research/LogicBridge.*` | P2 决策：重建为 `eda-hdl-slang`（dead 现状，H3） |
| `main/CanvasElement.h`/`CanvasPanel` | 画布持 `DesignNodeRef` 替代裸指针；编辑走 `ICommandBus`；算法不动 |
| `main/Command.h` | 落地 `ICommand`/`ICommandBus`（P2，修 H7/R18） |

### 10.6 仿真 → `eda-sim-verilator`（P1，修 R15）

- 复用：`SimulationEngine` 的 Verilator 编译参数、`SimMainGenerator`、`StimulusParser`、`TimelineGenerator`、`sc_time_stub.cpp`、**ProcessRunner**（全部保留）。
- 改动点：`wxExecute SYNC`→`IProcessHost` 异步（消除 UI 阻塞）；vcvars/cl 硬编码→编译器发现走 `IToolchain`；**DLL 编译步骤删除或可选化**；`sim_runner` 补退出码；VCD 体积控制（每时间步 dump 策略）。
- 删死码 `VerilatorRunner`（H4）。**archive 旧版（内存门级仿真）保留归档不迁**（H21）。

### 10.7 综合/布线/打包/烧录 → 四个插件（P1）

| 现有 | 去向 |
| --- | --- |
| `FpgaYosysScriptGenerator.*` | `eda-synth-yosys` |
| `fpga/ArtifactValidator.*` | `eda-synth-yosys`（产物校验 + sha256） |
| `Fpga/NextpnrLogParser.*`/`NextpnrReport.*` | `eda-pnr-nextpnr`（补 NextpnrJob，R6） |
| `fpga/FpgaConstraint.*`/`Fpga/CstValidator.*` | `eda-cst-gowin-cst` |
| `fpga/FpgaPinData.*` + `fpga/target-profiles/*.json` | `eda-target-tangnano9k`（**终于解析**，R7；删硬编码） |
| `FpgaYosysRuntime.*` | 其 ValidateYosysRuntime → `IToolchain` 运行时校验 |
| gowin_pack / openFPGALoader | `eda-pack-gowin`（**真实现 PackJob**）/ `eda-program-openfpgaloader`（FlashJob + requireConfirm） |

### 10.8 波形 → `eda-wave-vcd` + `IWaveformBackend`（P1-P2，修 R14）

- 保留：`WavePanel` 绘制（UI 归核心）。
- 数据层：`3rd/vcd` 读取 → `eda-wave-vcd`。**修 R14**：定容改为动态解析（或先包装：容量不足显式报错不静默丢弃），补 RAII（现 WavePanel 裸持 `vcd_t*` 从不 `vcd_free`），补 LICENSE。
- `WaveSlice` 接口供"框选提问"与跨进程共享内存。

### 10.9 元件库 → `eda-lib-basic`（P2，修 H12/H18）

- **活路径**：`canvas_elements.json` + `CanvasModel::LoadSecondElements` + `g_elements` → `eda-lib-basic` 的 `lib.list/lib.layout`（Shape variant 8 型 JSON schema 已匹配）。
- **死路径**：`ToolboxModel`/`tools.json` + `ToolboxPanel.cpp` 硬编码建树（300-483）→ 删除或改造为从 `IComponentLibrary` 取数。

### 10.10 平台层 → `eda-platform-win32/posix`（P0-P2）

- `IProcessHost`（含 ProcessRunner + YosysExecutor 逻辑）、`ISerial`、`IFileSystem`（路径分隔符/后缀/临时目录）、crypto（bcrypt→OpenSSL 统一）、日志。业务代码清除全部 `windows.h`/`unistd.h`（现状 `SimulationEngine.cpp` 是反例：无 `#ifdef` 却 Windows 路径硬编码）。

### 10.11 配置与工程 → `IProject` + `ISecretStore`（P0-P3）

- `sigflow.project` 读写收归 `IProject` 原子更新（修 R9/R17：**插件双写方必须改为经 IProject/审批回调**）；`DoFileSave` 空实现补全。
- **DeepSeek 拆解切分点（P3）**：
  - 下沉 `eda-llm-deepseek`（无 wx UI 依赖）：`CallDeepSeekAPI`(570-853)、SSE 解析(665-736)、prompt 构造 STRICT_VERILOG_CONSTRAINTS + `/autogen//topdown//scan`(367-547)、ParseDSResponse/GatherProjectFiles/FindSolutionRoot、文件写入+正则校验+UpdateProjectFileList(1394-1647)。线程改 `std::thread` + 宿主回调，不碰 wxPanel。
  - 留核心 `AgentPanel`：CreatePanel 的 UI 装配+会话状态机(947-1782)；宿主注入 `IHostServices{GetProjectRoot、写前审批回调（替代 previewDlg 模态）、文件流 sink（替代 EVT_AI_RESPONSE）}`；API Key 改 `ISecretStore` 注入。

### 10.12 死代码与孤儿清理清单（P0-P4 顺带）

`PluginStore`(H1)、`DeepSeek_plugin.h`(H2)、`VerilatorRunner`(H4)、`ToolboxModel`+`tools.json`(H18)、`PluginEntity/`(H19)、`archive/`(保留归档不迁，H21)、`FpgaYosysExecutor`（**先复活为 IProcessHost 再保留**，非删）、`UpdateTreeFromSlang`/`AsyncAnalysisCenter` slang 死链路（H3，P2 决策）。

---

## 11. 构建与分发：CMake 重构 + Profile

### 11.1 CMake 重构（目标十二）

```
根 CMakeLists.txt          # project + FetchContent + add_subdirectory(core InnerPlugin)
core/CMakeLists.txt        # eda_core 静态库 + SigFlow 可执行（UI+组合器）
cmake/EdaPlugin.cmake      # eda_add_plugin()：MODULE/STATIC 目标 + install 到 <prefix>/plugins
InnerPlugin/<name>/CMakeLists.txt   # 每个插件一个 target
```

```cmake
# cmake/EdaPlugin.cmake（草案）
function(eda_add_plugin NAME)
  cmake_parse_arguments(P "" "LOCATION;RUNTIME;KIND" "SOURCES;LIBS" ${ARGN})
  if(P_KIND STREQUAL "module")                       # 动态库（可外接/第三方）
    add_library(${NAME} MODULE ${P_SOURCES})
    target_compile_definitions(${NAME} PRIVATE EDA_BUILD_AS_PLUGIN=1)
    install(TARGETS ${NAME} LIBRARY DESTINATION plugins)
    return()
  endif()
  add_library(${NAME} STATIC ${P_SOURCES})           # 内部集成：静态库进核心
  target_link_libraries(${NAME} PUBLIC eda_core)
  eda_emit_plugin_registry(${NAME})                  # → plugin_registry.cpp
endfunction()
```

- **移除** `file(GLOB_RECURSE main/*.cpp)`，改显式源列表；`main/` 逐步清空，P4 仅剩 launcher。
- **修 R16**：CMake 与 vcxproj 源集合对齐（明确 CMake 不再多编死代码）；Linux 插件构建开起来。
- **修 H20**：wxWidgets 归一（`3rd/wxWidgets-3.2.9` 或 FetchContent 二选一，建议统一 FetchContent）。
- **修 H12**：JSON 归一（建议统一 jsoncpp——MainFrame 系 10 文件用、产物校验也用；nlohmann 转译层仅在接口层）。
- **对抗 R13**：`eda_add_plugin` 显式统一 `WXUSINGDLL`/`_ITERATOR_DEBUG_LEVEL`（宿主与插件同一组宏），并记录 build_info。
- slang：保留 FetchContent 但**移入 `eda-hdl-slang` 插件 target**（不再无谓全局编译），或 P2 决策后删除。

### 11.2 Profile 组合发行版

```
profiles/
├── minimal/     bundles=[base, platform-<tag>]
├── edu/         bundles=[base, platform, ui, hdl, sim, wave, lib, llm-mock, edu-hints]
├── pro/         bundles=[base, platform, ui, hdl, sim, synth, pnr, pack, program,
│                         target, cst, wave, lib, llm-deepseek, mcp, agent]
├── ci/          bundles=[base, platform, sim, synth, pnr, pack]   (无 UI)
└── sdk/         bundles=[base, platform, ui, ..., jsonrpc-server]
```

---

## 12. 目标目录结构

```
SigFlow/
├── CMakeLists.txt
├── cmake/EdaPlugin.cmake  EdaApiVersion.cmake
├── include/eda/api/            # ★ 唯一公共契约（核心与插件编译期边界）
│   ├── IPluginInteraction.h  capabilities.h  jobs.hpp  Types.h  abi.h
│   ├── services.hpp  events.hpp  schemas.hpp  text_document.hpp
│   └── ...
├── core/                       # 核心（UI 归这里）
│   ├── src/eda-core/           # Context/EventBus/PluginHost/IJobService/IProcessHost/
│   │                           # ISchemaRegistry/IToolchain/IProject/ILogger/ICommandBus
│   ├── src/eda-platform/       # abstraction + win32/ + posix/
│   └── src/eda-ui/             # Composer/画布/编辑器/面板/AgentPanel/插件管理中心/能力选择器
├── InnerPlugin/                # ★ 官方插件源码（每个一个子目录）
│   ├── eda-ir/  eda-hdl-treesitter/  eda-hdl-slang/
│   ├── eda-sim-verilator/  eda-synth-yosys/  eda-pnr-nextpnr/
│   ├── eda-pack-gowin/  eda-program-openfpgaloader/
│   ├── eda-wave-vcd/  eda-target-tangnano9k/  eda-cst-gowin-cst/
│   ├── eda-lib-basic/  eda-llm-deepseek/  eda-llm-mock/  eda-mcp-client/
│   └── (每个: edaplugin.yml + src/ + CMakeLists.txt + schema/)
├── bundles/  profiles/         # Bundle 补丁层 / 出厂 Profile
├── main/                       # 【迁移期】逐步清空，P4 后仅 launcher
├── 3rd/  external/             # 第三方（保留；wx 归一、vcd 补 LICENSE）
├── tests/                      # unit/ contract/ plugin_conformance/ fpga/
├── tools/  docs/
└── EDAHarness.md               # 已弃用（保留作历史参考，不再维护）
```

---

## 13. 迁移路线 P0–P4（每步可编译可运行，含 DoD）

> 原则：**绞杀者模式**——每阶段旧路径仍可用、可回退；每阶段有可演示验收（§14）。

### P0 —— 地基：内核 + 宿主 + 进程/Job 抽象

| # | 任务 | 关键文件 | 修复 |
| --- | --- | --- | --- |
| P0-1 | 建立 `include/eda/api/`（三层接口 + Types + abi + jobs + build_info） | `include/eda/api/*` | — |
| P0-2 | 实现 `eda-core`（Context/EventBus/Logger/Error/SchemaRegistry） | `core/src/eda-core/*` | H9/H10 |
| P0-3 | 实现 `PluginHost`（6 层发现 + manifest/构建指纹校验 + 状态机 + 诊断） | 重写 `main/PluginManager.*` | R1/R2/R13 |
| P0-4 | 实现 `IProcessHost` + 平台插件（复活 `YosysExecutor` + 迁入 `ProcessRunner`） | `core/src/eda-platform/{win32,posix}/*` | R4/H13/H14 |
| P0-5 | 实现 `IJobService`（泛化 `FpgaSynthesisJobService`，接 IProcessHost，补 timeoutSec） | `core/src/eda-core/JobService.*` | R5 |
| P0-6 | 实现 `IToolchain` + `IProject`（发现链三合一；.project 原子写） | `core/src/eda-core/Toolchain.*` `Project.*` | R8/R9/R19 |
| P0-7 | CMake 重构（eda_core target + eda_add_plugin + 注册表生成 + 源集合对齐 + wx/JSON 归一） | `CMakeLists.txt` `cmake/EdaPlugin.cmake` | C7/R16/H20/H12 |
| P0-8 | `MainFrame` 拆出组合器骨架；旧路径仍能跑（双路径） | `core/src/eda-ui/Composer.*` | — |
| P0-9 | 清理：删 `PluginStore`/`PluginEntity`/`DeepSeek_plugin.h` | — | H1/H2/H19 |

**DoD**：`build/` 出 `eda_core` + `SigFlow` 两个 target；综合/仿真经 `IJobService` 提交能跑通且**取消真杀进程树**；加载失败的插件可见原因；**回归：旧路径仍可用**。

### P1 —— 工具链插件化（改动最小化）

| # | 任务 | 关键文件 | 修复 |
| --- | --- | --- | --- |
| P1-1 | `eda-synth-yosys`（脚本生成器/ArtifactValidator 移入；输出走 jobDir） | `InnerPlugin/eda-synth-yosys/` | — |
| P1-2 | `eda-pnr-nextpnr`（**补 NextpnrJob**；LogParser/Report/CstValidator 移入） | `InnerPlugin/eda-pnr-nextpnr/` | R6 |
| P1-3 | `eda-pack-gowin`（**真实现 PackJob**）/ `eda-program-openfpgaloader`（FlashJob + 确认门控） | 同名目录 | R6 |
| P1-4 | `eda-sim-verilator`（复用 SimulationEngine 核心；去 vcvars 硬编码；去死 DLL；补退出码） | `InnerPlugin/eda-sim-verilator/` | R15/H4/H11 |
| P1-5 | `eda-target-tangnano9k`（解析 target-profiles JSON；删硬编码副本与 FpgaPinData 硬编码） | `InnerPlugin/eda-target-tangnano9k/` | R7 |
| P1-6 | `eda-cst-gowin-cst`（FpgaConstraint 移入） | `InnerPlugin/eda-cst-gowin-cst/` | — |
| P1-7 | 声明式工具运行时（YAML 描述 CLI → 自动生成 Job）："自定义布线=交 YAML" | `core/src/eda-core/DeclarativeTool.*` | — |
| P1-8 | UI：流程按钮接 `IJobService` + 能力下拉选择器（第一版） | `core/src/eda-ui/` | — |
| P1-9 | 波形数据层拆 `IWaveformBackend`（先包装 3rd/vcd；**不静默丢弃**；补 RAII/LICENSE） | `InnerPlugin/eda-wave-vcd/` | R14/H8 |

**DoD**：`sim/verilator ↔ icarus`（声明式）**不重编译**切换；综合→布线→打包→烧录全部有 Job 记录（含 pack）。

### P2 —— IR 与前端插件化（目标十一：算法不动）

| # | 任务 | 关键文件 | 修复 |
| --- | --- | --- | --- |
| P2-1 | `eda-ir`：去 `MainFrame*`、补 `DesignNodeRef(uid)`/`SignalTable`/序列化（附录 E） | `InnerPlugin/eda-ir/` | C1/H6 |
| P2-2 | `eda-hdl-treesitter`：抽离 tree_sitter_verilog + 3 段 TSQuery + UpdateTreeFromTS（算法原样）+ 真增量 | `InnerPlugin/eda-hdl-treesitter/` | C3/H5/H22 |
| P2-3 | `ICommandBus` 落地（覆盖写路径清单，附录 E）；编辑菜单真实现 | `core/src/eda-core/CommandBus.*` | H7/R18 |
| P2-4 | `ITextDocument`（Scintilla 隔离）；画布改持 DesignNodeRef、编辑走命令总线 | `core/src/eda-ui/` | C2/R12 |
| P2-5 | `eda-lib-basic`（活路径 canvas_elements.json）；Toolbox 从 IComponentLibrary 取数（删死路径 ToolboxModel） | `InnerPlugin/eda-lib-basic/` | H12/H18 |
| P2-6 | `eda-hdl-slang` 决策：重建或先删死码 | `InnerPlugin/eda-hdl-slang/` | H3 |
| P2-7 | 修 Arena UB（R11：`ClearNode` 改从 Arena 释放而非 `delete`） | `InnerPlugin/eda-ir/` | R11 |
| P2-8 | 移除硬编码 "DeepSeek_Assistant"；旧 ISigPlugin 兼容层下线 | `core/src/eda-ui/` `main/` | R2 |

**DoD**：画布↔代码双向同步在插件化后与现状**完全一致**（回归：全量样例往返一致）；**画布删节点/删线回删源码接通（修 R12）**；编辑可撤销；`ir/snapshot` 可落盘；Arena 无 UB。

### P3 —— 进程外 + AI/MCP（2 迭代）

| # | 任务 | 关键文件 | 修复 |
| --- | --- | --- | --- |
| P3-1 | 进程外插件宿主：控制通道 JSON-RPC + 共享内存 + 降级 | `core/src/eda-core/ProcessPlugin.*` | — |
| P3-2 | `eda-llm-deepseek`（网络/SSE/prompt/文件写下沉；`ISecretStore` 接管 Key） | `InnerPlugin/eda-llm-deepseek/` | R3 |
| P3-3 | `eda-llm-mock`（离线规则引擎，教育版默认） | `InnerPlugin/eda-llm-mock/` | — |
| P3-4 | `eda-mcp-client`（进程外；多 MCP server） | `InnerPlugin/eda-mcp-client/` | — |
| P3-5 | 插件管理中心完整版（路径登记/健康诊断/能力分配/外部切换/构建指纹标红） | `core/src/eda-ui/PluginCenter.*` | — |
| P3-6 | 平台收口剩余项（串口/命名管道/crypto 统一） | `core/src/eda-platform/` | Agents.md 18 文件 |
| P3-7 | `IProject` 收口插件写方（写前审批回调；替代插件直接改 sigflow.project） | `core/src/eda-core/Project.*` `eda-ui/AgentPanel` | R17 |

**DoD**：教育版离线可用（mock）；AI 审计证明"AI 从未直接拉起进程"；Key 不出进程、经 ISecretStore。

### P4 —— 生态与分发

| # | 任务 |
| --- | --- |
| P4-1 | `sigflow plugin add/search/update`（本地/私有源） |
| P4-2 | 插件一致性测试套件（`tests/plugin_conformance/`：清单合法/ABI 匹配/构建指纹一致/依赖可用/无 UI 线程违规） |
| P4-3 | Profile 打包发布（edu/pro/ci/minimal + 各平台 bundle）；`sigflow toolchain fetch`（oss-cad-suite 等） |
| P4-4 | `.sln` 冻结下线；`main/` 目录清空 |

---

## 14. 验收标准

### 14.1 自动化验收（每阶段 CI）

| 项 | 检查 |
| --- | --- |
| 依赖方向 | 扫描 include：`core/` 与 `InnerPlugin/` 不得包含 `MainFrame.h`；业务代码不得含 `<windows.h>`/`<unistd.h>`（平台插件除外） |
| schema 校验 | 所有 JobReport/PluginManifest/TargetProfile 写盘前过 `ISchemaRegistry` |
| 零静默 | `PluginHost` 失败必有结构化诊断记录 |
| 无密钥 | CI 扫描 `sk-`/`api_key` 模式 |
| 回归 | `tests/fpga/yosys/*` 全量样例插件化后结果一致 |
| 源集合对齐 | CMake 与 vcxproj 源文件集合 diff 为空（或 vcxproj 已冻结） |
| 构建指纹 | 动态加载插件与宿主 build_info 一致 |

### 14.2 端到端验收场景

- **A（流程）**：pro Profile，`RTL→仿真→综合→布线→打包→烧录(门控)`，每步是 Job、有 JobReport、队列面板实时显示；**综合中途取消立即杀 yosys 进程树**。
- **B（可替换）**：下拉框 `sim/verilator → sim/icarus`（声明式），重跑仿真**不重编译**。
- **C（自定义布线）**：第三方交含 `edatool.yml` 的目录，`sigflow plugin add` 后出现在布线下拉框并跑通。
- **D（教育版离线）**：edu Profile 无 Key 时仿真/画布/波形/分级提示全部可用（`eda-llm-mock`）。
- **E（外部 AI）**：UI 选"外部 LLM"填端点，不动代码切换。
- **F（AI 边界审计）**：审计日志证明 AI 从未直接 spawn 进程，所有执行有 jobId；AI 写文件经审批回调（修 R17）。
- **G（同步回归）**：插件化前后，同一工程画布↔代码双向编辑逐字节一致；**画布删节点回删源码**。
- **H（打包闭环）**：`eda-pack-gowin` 产出 `.fs`，`eda-program-openfpgaloader` 可烧录（修 R6）。
- **I（波形完整）**：>32 信号/超 4096 跳变的 VCD 完整加载不静默截断（修 R14）。

---

## 15. 风险与对策（含深挖新增隐患）

| # | 风险 | 等级 | 对策 |
| --- | --- | --- | --- |
| 1 | 改造期间功能回归（双向同步/综合是核心资产） | 高 | 绞杀者；P0-P1 不碰 `SigTree` 算法；`tests/fpga` 全量回归；双路径可切换 |
| 2 | 大文件搬迁（`SigTree.h` 582、`SimulationEngine.cpp` 1106、`MainFrame.cpp` 2937）合并冲突 | 中 | 先 `git mv` 再改内容，分文件提交 |
| 3 | **C++ ABI 脆弱（R13：wx/std::string 跨 DLL 布局不匹配）** | 高 | 官方插件编译进核心（零 ABI 风险）；动态加载仅限冻结 C ABI + `build_info` 构建指纹校验 + 统一迭代宏 |
| 4 | **Arena UB（R11：`delete` 释放 placement-new）** | 高 | P2-7 必修：`ClearNode` 改 Arena 语义释放（或不删、标记待 reset） |
| 5 | 进程外插件性能（IR 快照序列化） | 中 | 官方 IR 走进程内；进程外仅可外接场景，快照带预算 |
| 6 | 线程模型违规（FpgaToolProcess 回调在 UI 线程做产物校验会卡 UI） | 中 | `IJobService` worker 线程与 UI 严格分离；事件总线自动投 UI 线程；产物校验移 worker |
| 7 | 取消/超时语义遗漏（现状完全缺失） | 中 | `JobContext::cancelled()` 协作检查点 + `IProcessHost::cancel` 杀树双保险；一致性测试注入慢工具 |
| 8 | **vcd 定容静默截断（R14）** | 中 | 动态解析或显式报错；补 RAII/LICENSE |
| 9 | 双构建漂移（CMake vs .sln，R16） | 中 | CMake 为主；源集合对齐检查；.sln 回归至 P2 冻结 |
| 10 | **wxWidgets 双份（H20）** | 中 | P0 归一，避免 ABI 分叉 |
| 11 | 菜单占位功能缺失（R18）被误当作"已完成" | 中 | 每 P 阶段验收含菜单实测清单 |
| 12 | 打包流程从未实现（R6）被误以为存在 | 中 | P1-3 显式补 PackJob，场景 H 验收 |
| 13 | 声明式插件能力边界 | 中 | 声明式先行，可升级原生插件，接口不变 |
| 14 | 仿真链死 DLL + 阻塞 UI（R15）改动范围大 | 中 | 分步：先异步化 + 删死 DLL，再迁插件 |
| 15 | 双 JSON 库（H12）迁移面大 | 中 | 接口层统一 + 逐文件替换，CI 扫描 `#include <json/json.h>` vs `nlohmann` |

---

## 附录 A：`InnerPlugin/` 官方插件骨架

以 `eda-sim-verilator` 为例（每个插件同构）：

```
InnerPlugin/eda-sim-verilator/
├── edaplugin.yml                    # 清单（§5.1）
├── CMakeLists.txt                   # eda_add_plugin(eda-sim-verilator LOCATION external ...)
├── include/VerilatorSimulator.h
├── src/VerilatorSimulator.cpp       # ISimulator + IJobProvider 实现
├── src/VerilatorToolAdapter.cpp     # 复用 SimulationEngine 逻辑（StimulusParser/TimelineGenerator/SimMainGenerator/sc_time_stub），走 IProcessHost
├── schema/config.schema.json
└── tests/
```

```cpp
class VerilatorSimulator final : public eda::ISimulator, public eda::IJobProvider {
    std::string simulatorId() const override { return "verilator"; }
    void sim_build(const eda::SimBuildParams& p, eda::Callback<eda::Error, eda::SimBuildResult> cb) override {
        cb(eda::Error::None, eda::SimBuildResult{ .jobId = jobService().submit(toRequest(p)) });
    }
    std::string jobType() const override { return "sim.build"; }
    void startJob(const eda::JobRequest&, eda::JobContext& ctx) override {
        // 复用现有 Verilator 编译逻辑；ctx.processHost().spawn(...); ctx.log/progress/registerArtifact
    }
};
EDA_REGISTER_PLUGIN(eda-sim-verilator, VerilatorSimulator);
```

---

## 附录 B：冻结 schema 草案

### B.1 `eda.plugin-manifest.v1`（见 §5.1 示例）

### B.2 `eda.job.v1`（升级现有 manifest.json；字段对齐 `SynthesisJob`）

```json
{
  "schema_version": "eda.job.v1",
  "job_id": "01J...", "retry_of": null, "job_type": "synth", "plugin_id": "eda-synth-yosys",
  "request": { "project_id": "prj-01J...",
               "params": { "top_module": "top", "strategy": "baseline" },
               "require_confirm": false },
  "state": "Succeeded", "created_at": "...", "updated_at": "...", "exit_code": 0,
  "trace_id": "trc-...",
  "transitions": [ { "state": "Queued", "timestamp": "...", "operator": "ui", "reason": "submit", "exit_code": 0 } ]
}
```

### B.3 `eda.jobreport.v1`

```json
{
  "schema_version": "eda.jobreport.v1",
  "job_id": "01J...", "job_type": "synth", "plugin_id": "eda-synth-yosys", "state": "Succeeded",
  "timing": { "created_at": "...", "started_at": "...", "finished_at": "...", "duration_ms": 3120 },
  "exit_code": 0,
  "command": { "executable": "/opt/oss-cad-suite/bin/yosys", "argv": ["-Q","-s","run.ys"] },
  "environment": { "os": "linux", "arch": "x64", "tool_versions": { "yosys": "0.47" } },
  "artifacts": [ { "id": "netlist", "path": "artifacts/top.json", "schema": "eda.netlist.yosys-json.v1",
                   "size_bytes": 18234, "sha256": "ab12...", "role": "primary" } ],
  "metrics": { "lut": 96, "ff": 64, "bram": 0, "fmax_mhz": 88.4 },
  "diagnostics": [ { "code": "SYN2003", "severity": "warning", "stage": "synth",
                     "ir": { "file": "src/top.v", "start_line": 42, "end_line": 42 },
                     "summary": "inferred latch for signal 'q'", "raw_log_line": 187, "hint": "..." } ],
  "logs": { "combined": "logs/combined.log", "truncated": false }
}
```

### B.4 `eda.target-profile.v1`（升级 tang-nano-9k.json，终于被解析）

```json
{
  "schema_version": "eda.target-profile.v1",
  "id": "tang-nano-9k", "version": "1.0.0", "vendor": "Sipeed",
  "family": "gowin", "device": "GW1NR-LV9QN88PC6/I5",
  "synth":  { "tool": "yosys", "family": "gw1n", "top": "${top}" },
  "pnr":    { "tool": "nextpnr-himbaechel", "device": "GW1NR-LV9QN88PC6/I5", "varch": "gowin" },
  "pack":   { "tool": "gowin_pack", "device": "GW1N-9C" },
  "program":{ "tool": "openFPGALoader", "board": "tangnano9k" },
  "constraints": { "format": "gowin-cst", "template": "constraints/*.cst" },
  "pins":   { "database": "fpga/pins/tang-nano-9k.json", "package": "QFN88" }
}
```

---

## 附录 C：进程外插件控制通道协议（JSON-RPC 2.0）

```
握手:    → {"jsonrpc":"2.0","method":"eda.hello","params":{"pluginId":"...","abi":1,"buildInfo":"..."}}
         ← {"jsonrpc":"2.0","result":{"protocol":"eda.jsonrpc.v1","capabilities":[...]}}
调用:    → {"jsonrpc":"2.0","id":1,"method":"synth.run","params":{...}}
         ← {"jsonrpc":"2.0","id":1,"result":{...}}           （或 error）
事件:    ← {"jsonrpc":"2.0","method":"job/output","params":{"jobId":"...","line":"..."}}
大数据:   → 插件写共享内存段 sigflow-shm-<id>-<seq> → 控制通道 {"method":"shm.ready",
         "params":{"segment":"...","schema":"...","sha256":"...","size":N}}
         分配失败 → 降级 base64 或临时文件传 path
取消:    → {"jsonrpc":"2.0","id":2,"method":"$/cancel","params":{"requestId":"..."}}
心跳:    ←→ {"method":"eda.ping"}/{"result":"pong"}（空闲超时 30s 断开报 Crashed）
```

---

## 附录 D：插件管理中心 UI 设计

```
┌─ 插件与工具链管理中心 ───────────────────────────────────────────┐
│ [插件] [工具链] [元件库]                                        │
│  名称              能力            形态     来源      状态        │
│  eda-sim-verilator sim/verilator  内部集成  内置      ✓ 可用     │
│  eda-sim-icarus    sim/icarus     声明式   用户目录   ✓ 可用     │
│  eda-synth-yosys   synth/yosys    可外接·内 内置      ✓ 可用     │
│  eda-hdl-slang     hdl/slang      内部集成  内置      ⚠ ABI/构建指纹│
│                                         不匹配（原因可展开）      │
│  [＋添加插件…] [＋从目录扫描…] [启用] [禁用] [移除]                 │
│  能力默认选择（下拉）：                                           │
│  仿真: [Verilator ▾]   综合: [Yosys ▾]   布线: [nextpnr ▾]       │
│  烧录: [openFPGALoader ▾]  AI: [DeepSeek ▾] (外部…)             │
│  工具链（可执行文件，兼容 fpga.*_path）：                         │
│  yosys: [自动发现 ✓] /opt/oss-cad-suite/bin/yosys  [浏览…]      │
│  verilator: [未找到: 原因xxx]                  [浏览…] [扫描 PATH] │
└──────────────────────────────────────────────────────────────────┘
```

---

## 附录 E：双向同步算法现状与最小改造（核心资产保护）

> 目标十一：**算法不动**，只做"去 UI 化、包成插件"。本附录给出最小改造面，全部改动集中在数据通道，不碰同步算法本身。

### E.1 现状闭环（精读确认）

```
① 代码→树→画布：
   wxEVT_STC_MODIFIED → VerilogManager::EditBlock → HousekeepBlocks → 300ms ONE_SHOT timer
   → fragment 解析("module _tmp;\n"+delta) → 全量重解析整文件(old_tree=nullptr)
   → UpdateTreeFromTS(&cursor, editing_top_block->self, fp, x, map)
   → AddChild → ProcessEvent(EVT_SIGFLOWNODE_ADD) → MainFrame::OnSFNodeAdded → 刷新树/属性/画布

② 画布→代码：
   AddWire → sftree->AddNewWire(tn) → AddSignal → EVT_SIGFLOWNODE_ADD
   → VerilogManager::SigFlowNodeAdded → 父块末尾 InsertText(node->ToVerilog())
```

**已确认缺口（R12）**：`OnSFNodeDeleted` 不调 verilogMgr；`SigFlowNodeDeleted` 空壳 → 画布删节点/删线不回删源码。

### E.2 最小改造三件套（P2，均不碰同步算法）

1. **`DesignNodeRef`（修 C2/R12，最小侵入）**
   - `SigTreeNode` 加 `uint64_t uid`（Arena 单调计数分配，`Clone` 时复制 uid）。
   - `DefinitionTable/InstanceTable` 键改 uid 或加 uid→ptr 索引（保留 name 查找）。
   - outMap（VerilogManager.h:51 以裸指针为 key）改 uid key。
   - 画布 `SecondElement::self`/`Pin::self/top_self`/`TopModuleBox::self`/`Wire::GetSelf` 存 `DesignNodeRef{uid}`，取用时经树查表解析；Arena reset 后查表失败即安全失效，杜绝悬垂。
   - **同步算法（UpdateTreeFromTS/AddChild/AddWire）不动**：只在 Clone 复制 uid、注册时建 uid 映射。

2. **`ITextDocument`（修 C1，Scintilla 解耦）**
   - 接口子集（VerilogManager 与 SigFlowNodeAdded 用到的）：`GetText/SetText/GetRange/InsertText(pos,text)/DeleteRange(pos,len)/GetLineCount/PositionFromLine` + `OnModified(type,pos,len,text)` 回调。
   - `Block` 的 marker handle（startHandle/endHandle）改**文档内 offset 区间**（startByte/endByte），行号绑定下沉到 UI 层。
   - Scintilla 实现类 `ScintillaTextDocument` 留在核心；`eda-hdl-treesitter` 只依赖接口。

3. **`ICommandBus`（修 H7/R18）**
   - 覆盖写路径清单（精读确认）：`AddChild/RemoveChild/AddSignal/RemoveSignal/PortConn/PortReName/ReIdentifier/AddInPort/AddOutPort/TopDelPort/AddWire/DeleteWire/DelSecondNode`（SigTree.cpp:1212-1525、CanvasPanel.cpp:903-1004）。
   - 每个命令实现 `execute/undo/redo`；`DoEditUndo` 从空实现改为命令栈 pop（修 R18 的撤销项）。

### E.3 可直接搬入插件的核心资产（原样，仅改依赖）

| 资产 | 位置 | 去向 |
| --- | --- | --- |
| 3 段 TSQuery（top/second/net）+ FormalizeExpression | `SigTree.cpp:27-128/173-248` | `eda-hdl-treesitter` |
| 节点→行号 outMap + CollectBlocks/AppendBlocks | `VerilogManager.cpp:155-216` | `eda-hdl-treesitter`（uid 化） |
| SecondNodeTopoLevel 拓扑分层 + CompleteAutoWiring | `SigTree.cpp:1598-1670`；`CanvasPanel.cpp:1024-1402` | `eda-ir` 布局层 |
| Statement/StatementSequence/AlwaysStatement | `Statement.h`/`SigTree.h:543-582` | `eda-ir` |
| LogicBridge::SchematicBuffer 模型 | `research/LogicBridge.h:76-89` | `eda-hdl-slang`（重建时） |

### E.4 禁止改动（硬约束）

- `UpdateTreeFromTS` 的查询文本与匹配逻辑、`FormalizeExpression` 占位符算法、Arena 分配语义、`CompleteAutoWiring` 布线算法——**原样**。
- 唯一允许的"算法外"改动：`SigTreeNode` 加 `uid` 字段、`ClearNode` 的释放方式修正（R11，这是 UB 修复而非语义改动）。

---

*本文件为最终落地规划（v2.0）。所有接口与 schema 需在 P0 评审会逐项冻结后进入实现；冻结项见 §14.1 与 §15。*
*已弃用 `EDAHarness.md` 不再更新，冲突以本文件为准。*
