# Harness P0 阶段门禁评审（Gate Review）

> 日期：2026-09-21
> 基线仓库：`SigFlow_FPGA_Cmake`
> 依据：`docs/HarnessPlan.md` §13（P0–P4 路线）、§14（验收标准）
> 结论：**地基达标，可进入 P1**；P0 DoD 有 2 条未闭合，列为进 P1 的前置/并行收口项。

---

## 1. P0 任务清单

| 任务 | 状态 | 证据 |
| --- | --- | --- |
| P0-1 `include/eda/api/` 三层接口 | ✅（**最小子集**，非全量能力接口） | `include/eda/api/{Types,IPluginInteraction,capabilities,jobs,abi,build_info,services,events,schemas,process,toolchain,project,plugin_registry}.*` |
| P0-2 eda-core（Context/EventBus/Logger/SchemaRegistry） | ✅ | `core/src/eda-core/{EventBus,SchemaRegistry,Logger,Version}.*` |
| P0-3 PluginHost（发现/校验/状态机/诊断/能力选择） | ✅ | `core/src/eda-core/PluginHost.*`；`eda_plugin_host_smoke` 真加载 3 插件 |
| P0-4 IProcessHost + 平台层 | ✅（WIN 实测；POSIX 仅移植未编译） | `core/src/eda-platform/{win32,posix}/PlatformProcessHost.cpp`；`eda_api_contract_smoke` 真进程/超时/取消 |
| P0-5 IJobService | ✅ 核心库级 | `core/src/eda-core/JobService.*`；`eda_job_service_smoke` |
| P0-6 IToolchain / IProject | ✅ 核心 + `FindFpgaTool` 接入；`IProject` 未接 MainFrame | `core/src/eda-core/{Toolchain,Project}.*`；`MainFrame::FindFpgaTool` 调 `DefaultToolchain` |
| P0-7 CMake 重构 | ⚠️ **部分** | ✅ eda_core / `eda_add_plugin` / 静态注册表 / 源集合对齐（101 条显式列表）；❌ wx 归一、JSON 归一 |
| P0-8 MainFrame 组合器骨架 | ⚠️ 仅**插件维度** | `main/Composer.{h,cpp}`；面板组合未拆 |
| P0-9 死代码清理 | ✅ | 删除 `main/PluginStore.*`；`DeepSeek_plugin.h`/`PluginEntity` 早已不存在 |

## 2. P0 DoD（§13）

| DoD 项 | 状态 | 说明 |
| --- | --- | --- |
| `build/` 出 `eda_core` + `SigFlow` 两个 target | ✅ | `cmake --build build-gcc --target sigflow` 成功 |
| 综合/仿真经 `IJobService` 提交跑通 | ❌ | 应用仍走 legacy `main/jobs`；`CoreJobService` 未被 `sigflow` 使用 |
| 取消真杀进程树 | ⚠️ | 核心实现且有测试；**未接到应用** |
| 加载失败的插件可见原因 | ✅ | `PluginHost::DiagnosticReport` → `Composer` 打印 |
| 回归：旧路径仍可用 | ⚠️ | 完整构建通过；**未做 GUI 运行时回归** |

## 3. §14.1 自动验收门禁

依赖方向扫描、无密钥扫描、源集合对齐校验、schema 写盘校验、构建指纹校验 —— **0 项落地为 CI 检查**。

## 4. 通过项（可直接复用的 P1 地基）

- 契约层（header-only）+ `eda_core` / `eda_platform` 静态库，`sigflow` 已链接 `eda_core`。
- `CoreJobService` / `CoreJobContext`（异步、超时、取消杀树、报告、manifest 持久化）。
- `PluginHost`（静态注册 + 动态 C ABI 加载 + 构建指纹校验 + 能力选择）。
- `EDA_REGISTER_PLUGIN` 自注册 + `PluginHost::RegisterBuiltins`。
- `eda_add_plugin(KIND STATIC|MODULE)` 构建原语。
- `DefaultToolchain`（四层发现，`FindFpgaTool` 已接入）；`JsonProject`（原子写）。
- 测试：`ctest -R eda_` **4/4 Passed**；`-Wall -Wextra` 零警告；完整 `sigflow.exe` 构建通过。

## 5. 进 P1 前的 3 个收口项（建议拍板）

1. **Job 边界**：应用保留 legacy `main/jobs`、`CoreJobService` 供插件/新流程使用；还是把应用切到 `CoreJobService`？（建议前者 + 文档固化，切换成本高、收益低）
2. **§14.1 门禁**：至少加"依赖方向扫描（`core/` 不含 `MainFrame.h`、`main/` 不新增 `windows.h`）"与"源集合对齐"两条脚本。
3. **P0-7 归一结论**：wx 归一、JSON 归一明确"做/不做"，不做则写为已知偏差。

## 6. 结论

- P0 的**编译器契约、核心服务、插件宿主、进程/Job 抽象、构建原语**已就位且可构建、可测试 → **P1 可以在其上开工**。
- P0 **未完全达标**：应用未过 `IJobService`、§14.1 门禁未建、P0-7 归一未做、Composer 只拆插件维度。
- 建议：以上 3 项作为 P1 的**并行收口项**（不阻塞 P1-1 起手），并在 P1 首个里程碑一并验收。
