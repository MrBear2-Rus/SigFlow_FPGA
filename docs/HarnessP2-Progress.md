# Harness P2 进展（IR 与前端插件化）

> 日期：2026-09-21
> 基线仓库：`SigFlow_FPGA_Cmake`（P1 DoD 已达，见 `docs/HarnessP1-Gate-Review.md`）
> 依据：`docs/HarnessPlan.md` §10.5 / 附录 E（**算法不动**）、§13 P2（P2-1…P2-8）
> 硬约束：`UpdateTreeFromTS` 查询/匹配、`FormalizeExpression` 占位符、Arena 分配语义、
> `CompleteAutoWiring` 布线算法 **原样保留**；唯一允许的算法外改动 = `SigTreeNode` 加 `uid` + `ClearNode` 释放修正（R11）。

---

## P2-1a Step1：稳定节点标识（已完成）

- `main/SigTree.h`：`SigTreeNode` 新增 `std::uint64_t uid = 0;`（`Clone` 经拷贝构造复制 uid，不改算法）。
- `main/SigTree.h/.cpp`：`SigFlowTree` 新增 `nextUid`、`uidIndex`（uid→节点）、`ReindexUids()`、`NodeByUid()`；
  `ClearTree()` 一并清空索引。
- `ReindexUids()` 仅给缺失 uid 的节点分配并重建索引，**不修改任何同步/布线算法**。
- 验收：**完整应用构建** `sigflow.exe` 成功（零行为改动）。

### 待做（P2-1a Step2+）

1. 在解析/加载路径接 `ReindexUids()`（`VerilogManager` 解析后），并保证编辑过程中的 uid 稳定。 ← **Step2 已完成**
2. `VerilogManager` 的 `outMap`（现以 `SigTreeNode*` 为键）改为 uid 键。
3. 画布 `SecondElement::self` / `Pin::self` / `TopModuleBox::self` / `Wire::GetSelf` 由裸指针改为 `DesignNodeRef{uid}`，经 `NodeByUid` 解析（杜绝 Arena reset 悬垂，修 C2/R12）。
4. `SigTree` 去 `MainFrame*`（→ `Context&`，修 C1），物理拆包 `eda-ir`。

### P2-1a Step2：uid 稳定分配（已完成）

- `main/SigTree.h`：`Arena::make<T>` 在分配时对带 `uid` 成员的类型（`SigTreeNode` 及派生）赋唯一 `uid`（成员检测 `HasUidMember` + `if constexpr`）；`nextUid_` 不随 `reset()` 归零。
- `main/VerilogManager.cpp`：解析（`UpdateTreeFromTS`）后调用 `m_tree->ReindexUids()`。
  - 新建节点获得稳定 uid；已有节点保留 uid；仅赋标识、重建索引，**不改同步算法**。
- 验收：**完整应用构建** 成功。

### P2-1a Step3（部分完成）

- **`outMap` 改 uid 键（已完成）**：
  - `SigTree.h/.cpp`：`UpdateTreeFromTS` 的 `outMap` 键由 `SigTreeNode*` 改为 `std::uint64_t`（8 处写入点 `outMap[node->uid]`）。
  - `VerilogManager.h/.cpp`：`SetFileNode`/`CollectBlocks`/`AppendBlocks` 改 uid 键，消费时经 `m_tree->NodeByUid(uid)` 解析（`AddBlock` 仍取节点）。
  - `MainFrame.h/.cpp`：`maps` 与临时 `map`、`emptyMap` 类型改 uid 键。
  - 加载路径（两处）在 `LinkInstsWithDefs()` 后调用 `sigTree->ReindexUids()`；编辑路径（`VerilogManager`）解析后亦调用，确保 `NodeByUid` 可解析。
  - 验收：**完整应用构建** 成功；无遗留指针键 map。
- **画布 `DesignNodeRef{uid}`（未做，需真机验证）**：`SecondElement::self`/`Pin::self`/`TopModuleBox::self`/`Wire::GetSelf` 由裸指针改 uid，所有消费点经 `NodeByUid` 解析（修 C2/R12）。涉及 `CanvasPanel`/`CanvasEventHandler`/`SFNPropertyPanel`/`MainFrame` 等，属 GUI 运行时相关改动。
- `SigTree` 去 `MainFrame*` → `Context&`（修 C1），物理拆包 `eda-ir`（未做）。

## P2-7 Arena UB（R11）复核

- `SigTree.cpp::ClearTree()` 现为 `root->ClearNode()` + `arena.reset()`；`ClearNode()` 只断父子链接、不 `delete`（对象由 `Arena::reset()` 逆序析构）。**R11 已满足**。

## P2-5 `eda-lib-basic`（第一增量完成）

- 契约扩展：`include/eda/api/component_library.hpp`（`IComponentLibrary` + `ComponentTemplate`/`LibraryShape`/`LibraryPoint`，wx-free）。
- 新增插件 `InnerPlugin/eda-lib-basic/`（`BasicComponentLibrary`）：解析 `canvas_elements.json`（nlohmann），提供 `Components()`/`Find(type)`。
- 测试 `tests/plugins/eda_component_library_smoke.cpp`：加载真实元件库、`and`/`nand` 引脚与图形、缺失返回 null。
- **第二增量**：删除死路径 `main/ToolboxModel.{h,cpp}`（`ToolboxPanel` 仅 include 未调用；`tools.json` 不存在），同步清理 CMake 显式列表 / `main.vcxproj` / `.filters` / `ToolboxPanel.cpp` include；全仓 0 引用。
- **未做**：`CanvasModel`/`ToolboxPanel` 接入 `IComponentLibrary`（`CanvasModel` 的 jsoncpp→wx `Shape` 变体解析保真，改动需 GUI 运行时验证）。

## P2-8 去硬编码（第一增量完成）

- `main/Composer` 新增 `LegacyAssistant()`（角色化查找，插件名集中在 `Composer.cpp` 一处常量）；`MainFrame` 两处不再硬编码 `"DeepSeek_Assistant"`。
- **未做**：旧 `ISigPlugin` 兼容层下线（依赖 P3 把 DeepSeek 迁移到 `IPluginInteraction`）。

## P2-3 `ICommandBus`（第一增量完成）

- 契约扩展：`include/eda/api/command.hpp`（`ICommand` + `ICommandBus`）。
- 核心实现：`core/src/eda-core/CommandBus.{h,cpp}`（撤销/重做双栈；执行成功才入栈；新命令清空 redo；失败/空命令不入栈）。
- 测试 `tests/plugins/eda_command_bus_smoke.cpp`。
- **未做**：把编辑写路径（`AddChild`/`RemoveChild`/`AddSignal`/`AddWire`/`DeleteWire`/…，附录 E）包成命令并接入画布/菜单（GUI）。

## P2-6 `eda-hdl-slang`（决策 + 删死码）

- 决策：**先删死码**（slang 分析块为注释、`LoadProject` 仅被注释调用、`UpdateTreeFromSlang` 空且无调用）。
- 已删：`SigTree::UpdateTreeFromSlang` 及 `SigTree.h` 的 `<slang/ast/Compilation.h>`；`AsyncAnalysisCenter::LoadProject` + `SlangProject` + `AsyncAnalysisCenter.h` 的 `<slang/driver/Driver.h>`；补 `SigTree.h` 显式 `<cstddef>`（原靠 slang 头传递）。
- **`main/` 代码级 slang 归零**；验收：**完整应用构建** 成功。
- **CMake 清理完成**：移除 `SIGFLOW_SLANG_*` 变量/选项、`SIGFLOW_SLANG_LOCAL` 判定、本地源码 `add_subdirectory`、`target_include_directories`/`target_link_libraries` 的 slang 分支（`CMakeLists.txt` 仅剩说明注释）。`external/slang-src` 保留但不再参与构建。

## P2-1b 解耦 + 同步回归（已完成）

- **去 `MainFrame*`**：`SigFlowTree` 的 `MainFrame* m_parent` → `wxEvtHandler*`（`MainFrame` 隐式转换，调用点零改动）；`SigTree.cpp` 去掉 `#include "MainFrame.h"`；`m_parent->GetEventHandler()` → `m_parent`。
  - 意义：`SigFlowTree` 不再依赖整个 wx 应用，**可用桩 `wxEvtHandler` 独立驱动**（修 C1 的前半）。
- **双向同步回归样例**：`tests/ir/sig_tree_sync_smoke.cpp`（非 GUI）
  - 覆盖：`Project→File→Top` 结构、`TopNode` 名称、`outMap` 以 uid 为键、uid 唯一且 `NodeByUid` 可解析、`ToVerilog` 往返（`module top`/`endmodule`）、二次解析 uid/指针稳定、`ClearTree` 复位。
  - 19 项断言 **ALL PASS**。
- 构建接线：`tests/ir/CMakeLists.txt`（编译 `SigTree.cpp` + tree-sitter，链 wx core/base，POST_BUILD 拷贝 wx DLL）。
- 验收：**完整应用构建** 成功；`ctest -R "eda_|sig_tree"` **19/19**。

## P1-8 主程序接入（后端 + 四入口可选路径）

- `main/Composer`：`Providers(capability)`、`DefaultProvider(capability, preferredId)`、`UseJobService()`（环境变量 `SIGFLOW_USE_JOB_SERVICE=1`）、`SubmitJob(jobType, projectId, params, requireConfirm, error)`。
- `main/MainFrame`：`AvailableToolCapabilities()`/`ToolProviders()`/`DefaultToolProvider()`（UI 数据源）；`RunFpgaSynthesis`/`RunFpgaRoute`/`RunFpgaPack`/`RunFpgaProgram` 各增 **`IJobService` 可选分支**（默认 legacy，开关启用后经 `Composer::SubmitJob` 提交 `synth`/`pnr`/`pack`/`flash`）。
- **待做**：能力下拉框控件与"默认后端"持久化；四入口新路径的**行为等价性 GUI 验证**（终端回显/进度/取消/报告）与旧路径切换。

### 能力选择 UI（Help → Toolchain Backends）

- `MainFrame::BuildToolchainBackendsMenu(wxMenu*)`：按能力动态生成子菜单，每个能力下以**单选**列出可用后端（来自 `Composer::Providers`），勾选当前默认后端。
- 选择经 `Composer::StoreDefaultProvider` **持久化**到应用配置 `Toolchain/DefaultBackend/<capability>`；`Composer::DefaultProvider` 优先级为 preferredId → 持久化默认 → 首个就绪后端。
- `MainMenuBar::CreateHelpMenu` 挂载空的 `Toolchain Backends` 子菜单；**`wxEVT_MENU_OPEN`（Help 菜单打开时）懒重建**，解决"菜单在 `Composer` 加载插件前创建"导致的空列表问题（`m_helpMenu`/`m_helpBackendsMenu` 指针比较触发重建）。

### 关键修复：静态自注册被链接器丢弃

- **现象**：Help → Toolchain Backends 显示 `(no toolchain backends found)`；诊断仅有 `path: ... not found`，无任何插件记录。
- **根因**：`EDA_REGISTER_PLUGIN` 的自注册 TU 无外部符号被引用时，**静态库按需拉取会整体丢弃该对象**，`StaticPluginRegistry()` 为空 → `RegisterBuiltins()` 注册 0 个。
- **修复**：
  1. `include/eda/api/plugin_registry.h`：宏额外生成 `extern "C" void eda_plugin_force_link_<UniqueName>()` 锚点（引用注册变量，强制保留该 TU）。
  2. `main/Composer.cpp`：`ForceLinkOfficialPlugins()` 在 `RegisterBuiltins()` 前显式调用全部官方插件锚点。
  3. `CMakeLists.txt`：`sigflow` 补链 `eda-program-openfpgaloader` / `eda-wave-vcd` / `eda-lib-basic`（原先漏链，锚点因此 undefined）。
- 验收：**完整应用构建** 成功；`ctest -R "eda_|sig_tree"` 19/19。

## P2 其余任务（剩余）

| 任务 | 说明 |
| --- | --- |
| P2-1 | `eda-ir`：`SigTree` 去 `MainFrame*`、`DesignNodeRef`/`SignalTable`/序列化 |
| P2-2 | `eda-hdl-treesitter`：抽离 `tree_sitter_verilog` + 3 段 TSQuery + `UpdateTreeFromTS`（算法原样）+ 真增量 |
| P2-3 | `ICommandBus` 落地（编辑菜单真实现） |
| P2-4 | `ITextDocument`（Scintilla 隔离）；画布持 `DesignNodeRef` |
| P2-5 | `eda-lib-basic`（`canvas_elements.json` 活路径；删 `ToolboxModel` 死路径） |
| P2-6 | `eda-hdl-slang` 决策：重建或先删死码 |
| P2-8 | 移除硬编码 `"DeepSeek_Assistant"`；旧 `ISigPlugin` 兼容层下线 |
