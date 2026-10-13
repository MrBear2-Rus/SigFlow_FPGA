# SigFlow Job 三层体系收敛方案

> 建立日期：2026-10-06（P0 阶段产出）
> 前置阅读：[SigFlow-冗余代码与优化建议.md](SigFlow-冗余代码与优化建议.md)（本方案是其"实施顺序"第 4 步的展开）
> 铁律：**收敛全程不得破坏插件机制**（见 §4 不变式清单）。

## 1. 三层现状

| | 第 1 层：专用综合 Job | 第 2 层：旧通用 Job | 第 3 层：新契约 Job |
| --- | --- | --- | --- |
| 代码 | `main/FpgaSynthesisJob.{h,cpp}` | `main/jobs/JobService.{h,cpp}` + `ToolJobs` + `PlatformProcess` | `include/eda/api/jobs.hpp` + `core/src/eda-core/JobService.{h,cpp}` + `IProcessHost` 平台实现 |
| 执行引擎 | 内嵌（vcvars/工具命令拼装） | `PlatformProcess`（管道 + 手工树杀 + 进程登记表） | `IProcessHost`（CreateProcessW + Job Object 整树兜底；POSIX fork/setpgid） |
| 状态机 | 内部小型状态集合 | `ToolJobState` 九态 + `IsLegalTransition` | `eda::JobState` 九态 + `IsLegalTransition` |
| 报告 | 自有 JSON | `schemaVersion="1.0"` 自有字段（durationMs/summary/errors/stage） | `edu.jobreport.v1`（artifacts/metrics/diagnostics/origin） |
| 取消 | 无独立取消 | `RegisterProcess` 句柄 + `CancelRequested` 标志 | `IProcessHost::Cancel()`（整树） |
| 超时 | 无 | 0=INFINITE；描述符默认 600s | `timeoutSec`（<=0 不超时；服务默认值） |
| 消费者 | 旧 GUI 综合 + 历史 Job 面板 | MainFrame 默认旧路径（4 处分支：综合/布线/打包/烧录） | **Agent Gateway（唯一全量现役）**；GUI 需 `SIGFLOW_USE_JOB_SERVICE=1` 试点 |

## 2. 语义差异与映射（已锁定为回归哨兵）

### 2.1 状态：1:1

`ToolJobState` 与 `eda::JobState` 是**同名同序九态**，序列化名字一致：
`Created / Validating / Queued / Running / ValidatingArtifact / Succeeded / Failed / Cancelled / TimedOut`。

映射已固化为 `core/src/eda-core/JobsMigration.{h,cpp}`（`jobs_migration::LegacyJobStateFromName` / `JobStateToLegacyName`），`tests/job_tests.cpp` 的 TestJobsMigrationStateMap 同时核对旧层 `ToString(ToolJobState)` 与九态映射。已覆盖状态的序列化名字变化会使测试失败；新增状态或改变状态语义仍须人工更新映射与回归用例，不能只依赖枚举顺序。

### 2.2 类型：五组对偶

| 旧 `ToString(ToolJobType)` | 新 `IJobProvider::jobType()` |
| --- | --- |
| `simulation` | `sim.build`（另有 `sim.run`；历史迁移归并 `sim.build`） |
| `synthesis` | `synth` |
| `pnr` | `pnr` |
| `pack` | `pack` |
| `flash` | `flash` |

### 2.3 其余差异（切换前必须补齐/对齐）

| 差异 | 现状 | 收敛动作 |
| --- | --- | --- |
| 仿真主流程 | `SimulationEngine` 专用 runner（`sc_time_stub.cpp` 查找/生成**必须保留**） | P1 迁移为 `sim.build`/`sim.run` provider 调用，保留 stub 逻辑 |
| 历史记录 | 第 2 层自有 JSON 存储，旧 Job 面板直读 | 新层增加只读加载（按 §2.1/§2.2 映射翻译），不做格式改写 |
| 参数校验 | `JobServiceRegistry::ValidateParameters`（jsoncpp schema） | 沿用，入口切到 Gateway/新服务时不搬语义 |
| 回调线程 | 旧层经 pipe/CallAfter 衔接 UI | 新层 `ctx.log/progress` 在 worker 线程；MainFrame 增加事件队列转发 |
| `PYTHONPATH` 类环境注入 | 旧 `PlatformProcess` 自带 | 由第 3 层 `startJob` 内部前置自检承接（Verilator python3/POSIX sh 已修复并实测） |

## 3. 收敛顺序（对应冗余报告 §实施顺序 4）

1. **P0（本轮已完成）**：差异矩阵 + 状态/类型映射 + 单测哨兵。
2. **P0.5**：新层补齐 GUI 依赖能力（历史面板只读、仿真 runner 接入、烧录 `requireConfirm` UI 流）。验收：四个端到端冒烟（仿 edu_real_tool_smoke 形态）。
   - **进度 2026-10-06：**第 1 件已有只读加载实现——`core/src/eda-core/LegacyJobHistory.{h,cpp}` 遍历旧五类存储（`<project>/.sigflow/jobs/<type>/<jobId>/manifest.json` + `reports/job-report.json`），状态/类型按 §2.1/§2.2 翻译。坏 manifest 上报 id 并继续扫描；缺失报告保留历史记录，损坏报告上报 id。已补字段类型错误回归测试，`job_tests` 通过。历史面板仍未接入加载器，因此 GUI 历史兼容尚未验收。
   - 剩余：仿真 runner 接入（保留 `sc_time_stub`）、烧录确认流、GUI worker→wx 事件转发。
3. **P1 切 GUI**：删 4 处 `else` legacy 分支；`UseJobService()` 默认 true；环境变量保留为逃生门。
4. **P2 删旧**：连续多次回归全绿后移除第 1/2 层实现与不再被引用的适配层；历史存储保留只读加载器或一次性迁移脚本（写进验收材料）。
   - **已完成的独立清理：**`main/jobs/JobRegistry.cpp` 的旧注册表从未被生产代码或测试调用；其专属 `SynthJob` / `PnRJob` 包装器也无其他调用点，已移除。`DefaultJobTimeoutSeconds` 留在仍被 GUI 使用的 `ToolJobs.cpp`，维持原来的超时值。移除后 `job_tests` 通过，`sigflow` 主程序构建通过。
   - **暂不能删除：**`FpgaSynthesisJob`、`NextpnrJob` 分别仍支撑 GUI 历史面板与默认综合/布线流程；`JobService` / `ToolJobs` 的仿真、打包、烧录路径和 `PlatformProcess` 仍在使用。需完成 P0.5、P1 并通过端到端回归后才可删，否则会造成 GUI 功能缺失。

## 4. 插件机制不变式（全程不得触碰）

- `EDA_REGISTER_PLUGIN` 静态自注册 + `extern "C" …force_link_*` 引用符号：DeepSeek、外部动态插件的注册路径不动。
- `PluginHost` 的 searchPaths（exeDir/plugins、.sigflow/plugins、SIGFLOW_PLUGIN_PATH、个人目录）不动。
- `EDA_PLUGIN_ABI_VERSION` 锁不升；`IJobProvider::startJob(JobRequest&, JobContext&)` 签名不变——**所有收敛代码都新增在新层内部**（CoreJobService、JobsMigration），核心 API 只加不改。
- 新层子进程环境注入是 `startJob` 的内部行为，不改变插件 ABI，也不改变"AI 只拿 JobType + 参数"（B-04/T-01）的边界。

## 5. 风险与对策

| 风险 | 对策 |
| --- | --- |
| 旧历史 JSON 与新存储状态名不一致 | §2.1 哨兵单测 + 只读翻译层 |
| GUI 回调线程竞态 | 事件转发队列统一处理；切换前以真实工具跑取消/超时用例 |
| 第三方 runtime 重新打包丢失 `PYTHON3 ?=` 补丁 | job 私有 python3.exe shim 已双保险（详见 VerilatorSimulator 前置自检）；两保险重构时都不得随意删 |
| 收敛途中新旧双跑导致产物覆盖 | P0.5 阶段每条流先并行影子跑，再切主线 |
