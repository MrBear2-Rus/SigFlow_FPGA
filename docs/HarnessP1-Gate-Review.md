# Harness P1 阶段门禁评审（Gate Review）

> 日期：2026-09-21
> 基线仓库：`SigFlow_FPGA_Cmake`
> 依据：`docs/HarnessPlan.md` §13 P1（P1-1…P1-9）、§14.2（端到端验收 B/A）
> 结论：**P1 验收标准（DoD）达成，可进入 P2。** 任务清单中的少数"完整移入/UI 接入"为 DoD 之后的收尾增强，已逐条记录。

---

## 1. P1 DoD（§13）

> 原文：`sim/verilator ↔ icarus`（声明式）**不重编译切换**；综合→布线→打包→烧录**全部有 Job 记录（含 pack）**。

| DoD 项 | 状态 | 证据 |
| --- | --- | --- |
| 仿真后端声明式切换（不重编译） | ✅ | `eda_declarative_switch_smoke`：运行时用 JSON 创建 `sim/icarus` 声明式后端，与内置 `sim/verilator` 同时可用、可经 `PluginHost::Select` 切换并执行，**未新增任何 C++ 插件代码** |
| 综合→布线→打包→烧录均有 Job 记录 | ✅ | `eda_synth_yosys_smoke`（synth）、`eda_pnr_nextpnr_smoke`（pnr）、`eda_pack_flash_smoke`（pack + flash，含 `requireConfirm` 门控）；均由 `CoreJobService` 产出状态机 + 报告 + manifest |
| 声明式工具运行时（P1-7） | ✅ | `eda_declarative_tool_smoke`：`DeclarativeJobProvider`（JSON 描述 → Job，`${param}` 占位） |

## 2. 自动化验收

| 项 | 结果 |
| --- | --- |
| `ctest -R eda_` | **13/13 Passed** |
| 完整应用构建 `cmake --build build-gcc --target sigflow` | **成功**（`sigflow.exe`） |
| `-Wall -Wextra` | 零警告 |
| 依赖方向 | `core/` 与 `InnerPlugin/` 均不包含 `MainFrame.h`；OS `#ifdef` 仅存在于 `eda-platform` 后端 |

## 3. 任务清单完成度（P1-1…P1-9）

| 任务 | 完成度 | 说明 |
| --- | --- | --- |
| P1-1 `eda-synth-yosys` | ✅ | 脚本生成器 + ArtifactValidator 移入；Job 执行体 |
| P1-2 `eda-pnr-nextpnr` | ✅（含 LogParser/Report 移入） | `CstValidator` 归 P1-6 |
| P1-3 pack/flash | ✅ | 含 `FpgaPackService` 移入 |
| P1-4 `eda-sim-verilator` | ✅ DoD（sim.build+sim.run） | 生成器/解析器移入为收尾增强 |
| P1-5 `eda-target-tangnano9k` | ✅ DoD（JSON 目标 profile 解析接入） | `FpgaPinData` JSON 化为收尾增强 |
| P1-6 `eda-cst-gowin-cst` | ◑ 编解码已落 | `FpgaConstraint`/`CstValidator` 完整移入为收尾（与 pin 库纠缠） |
| P1-7 声明式运行时 | ✅ | |
| P1-8 UI 接 `IJobService` | ◑ 未做 | DoD 以插件/服务层证据达成；UI 接入为增量 |
| P1-9 `IWaveformBackend` | ◑ 插件后端已落 | `main/trace` 切换 + `3rd/vcd` 退役为收尾 |

## 4. 明确遗留（DoD 之后）

- P1-4：`SimMainGenerator`/`StimulusParser`/`TimelineGenerator` 移入（~726 行 wx）。
- P1-5：`FpgaPinData` 引脚库 JSON 化。
- P1-6：`FpgaConstraint`(691)/`CstValidator`(332) 完整移入 + `FpgaPinBindingPanel`/`NextpnrExecutor` wx 适配。
- P1-8：`MainFrame` 流程按钮接 `IJobService` + 能力下拉选择器。
- P1-9：`main/trace/VcdLazyTraceSource` 切到 `IWaveformBackend`；`3rd/vcd` 补 LICENSE 并退役定容实现。
- P1-7：YAML 描述（当前 JSON）与声明式工具接入 MainFrame 工具发现。

## 5. 结论

- **P1 的 DoD 两幕均已由测试证明**：声明式不重编译切换（`sim/verilator ↔ sim/icarus`）与四类工具 Job 记录（synth/pnr/pack/flash）。
- `InnerPlugin/` 已含 8 个官方插件；`CoreJobService`/`PluginHost`/`IProcessHost`/`eda_add_plugin` 构成插件化执行底座。
- 据此：**P1 验收通过，可进入 P2。** §4 遗留项为收尾增强，不阻塞 P2。
