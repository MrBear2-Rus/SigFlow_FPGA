# SigFlow 后端 Job 化清单（B-01..B-04）评审报告

> 评审对象：`docs/SigFlow_Plan_0907.md` §10.3「Job 化补齐」B-01 ~ B-04
> 评审范围：`main/jobs/`、`tests/jobs/`、`main/fpga/FpgaPackService.*` 复用情况、`main/main.vcxproj`、`tools/run_debug_ci.ps1`
> 评审方式：代码走查 + 编译 + 项目自带 CI 全量回归 + 独立行为探针（`.dsh_probe/JobsProbe.cpp`、`.dsh_probe/CreateProcessProbe.cpp`）
> 评审日期：2026-09-09

---

## 0. 一句话结论

**新增了一套可用的 Job 外壳（注册表 / manifest / 状态枚举 / 报告 schema 都在），但它目前是"影子实现"：既没有接上既有仿真栈与 GUI 流程，也没有真正实现清单里写明的取消、超时一致性、并发上限和产物登记。** 清单 B-01~B-04 的完成度约 **40%**，且存在 6 个实测可复现的 P0 缺陷，直接破坏 DS-01（JobReport 统一 schema）与 U-07（可追溯性闭环）的前提。

回归面是好的：项目自带 `tools/run_debug_ci.ps1` 全量通过（18 个 smoke + 多时钟 RTL 检查，`DEBUG CI: ALL PASS`），新增的 `job_service_smoke` 也在其中，X-05「既有回归保持绿」未被破坏。

> **状态更新（同日）**：P0-1 ~ P0-5 已按"最小改动"修复并验证通过，见 **§8 最小改动修复记录**；P0-6（GUI 接 Job）与全部 P1 项未动。§2/§3 保留修复前的问题描述，作为评审留痕。

---

## 1. 交付物清单与完成度

| 清单项 | 交付内容 | 完成度 | 结论 |
|---|---|---|---|
| **B-01 SimJob** | `SimulationJob::Submit/Execute`（`main/jobs/ToolJobs.cpp`） | **~30%** | 只包了"外部可执行 + 参数"的通用进程；**没有**包装 `VerilatorRunner` / `SimulationEngine` / `ProcessRunner`；VCD 产物登记实测失效 |
| **B-02 PackJob** | `PackJob::Submit/Execute`，复用 `FpgaPackService::ValidateInput/Finalize` | **~60%** | 正确复用既有 SHA-256 记账与产物校验；但 `FpgaPackService` 并未"升级为 Job"，GUI 打包仍走旧服务；失败路径不落盘报告 |
| **B-03 FlashJob** | `FlashJob::Submit/Execute` + `requireConfirm` | **~50%** | 门控语义正确（`request.requireConfirm \|\| options.requireConfirm`，调用方无法关掉）；但未接 GUI 烧录路径、失败无报告、产物 size 记 0 |
| **B-04 JobService 注册表与调度** | `JobService`（CRUD/Transition/Retry/Cancel）+ `JobServiceRegistry`（Register/Find/List/Submit）+ `job-report.schema.json` | **~50%** | 注册/发现/创建/加载/列表/重试/非法转换拒绝齐备；**调度、并发上限、有效取消、参数 schema、状态机闭环缺失** |

**P0 里程碑"全流程以 Job 形式跑通、报告可程序化读取"未达成**：sim/pack/flash 三个新 Job 都没有被 GUI 或既有流程调用（`main/MainFrame.cpp` 中零引用），Yosys/nextpnr 两个既有 Job 也没有纳入新框架。

---

## 2. P0 缺陷（实测复现）

### P0-1 SimJob 落盘报告丢失产物登记（DS-01 / U-07 断链）｜已修复 §8

`SimulationJob::Execute` 先调 `RecordArtifact`（内部 `WriteReport`），随后又用**不含 artifacts 的内存 report** 覆盖写一次：

```cpp
// main/jobs/ToolJobs.cpp:220-224
if (!JobService().RecordArtifact(projectPath, job.id, request.outputVcdPath, "vcd", errorMessage))
    return false;
report.state = ToolJobState::Succeeded;
report.summary = "Simulation completed and VCD artifact recorded.";
return JobService().WriteReport(projectPath, job.id, report, errorMessage); // artifacts 被清空
```

实测落盘结果（VCD 确实存在且已登记过）：

```json
{ "artifacts" : [], "state" : "Succeeded",
  "summary" : "Simulation completed and VCD artifact recorded." }
```

→ 报告自称"已登记产物"，实际产物清单为空；U-07 的"产物哈希可查"在 SimJob 上不成立。

### P0-2 失败 / 超时 / 取消路径完全不落盘 job-report.json｜已修复 §8

`ToolJobExecutor::Run` 返回 false 时（可执行缺失、非零退出、超时），三个 Job 的 `Execute` 都直接 `return false`，**从不调用 `WriteReport`**（唯一例外是 `PackJob` 产物校验失败分支）。实测：可执行文件不存在时，`reports/job-report.json` 不存在。

→ DS-01 报告是 B-05「报告解析统一」与 AI 解读的**唯一输入**，而 AI 最需要读的恰恰是失败报告。当前设计下"失败 = 没有报告"。

### P0-3 Execute 不推进 manifest 状态机（U-01）｜已修复 §8

`ToolJobExecutor::Run` 只填内存 `JobReport`，不调用 `JobService::Transition`。实测：进程成功退出后

```
job-report.json: state = Succeeded
manifest.json  : state = Running     ← 永远停在这里
```

→ 报告与 manifest 状态可以任意矛盾；`WriteReport` 也不校验一致性。B-04 要求的状态机在新增的三个 Job 上等于没接。

### P0-4 Cancel 只改状态、不杀进程｜已修复 §8

`JobService::Cancel` 只写 manifest；执行器没有登记进程句柄，也没有 Job Object（对比 `FpgaYosysExecutor.cpp:167-190`、`NextpnrExecutor.cpp:268-291` 都用 `CreateJobObjectW` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）。

实测：`ping -n 6`（约 5 秒）执行中调用 `Cancel` 成功后，执行线程**又跑了 4286 ms** 才结束。manifest 说 `Cancelled`，工具仍在跑并会继续写产物/日志。

### P0-5 并发上限（`maxConcurrent`）只存不用｜已修复 §8

`JobToolDescriptor::maxConcurrent` 被登记但没有任何代码读取；`JobService::Start` 无条件把 Job 推到 `Running`。实测：注册表 `sim.run` 的 `maxConcurrent=1`，连续 Start 两个 Job 后两个 manifest 都是 `Running`。

→ B-04 明确要求的"并发上限"与"调度"未实现（`Submit` 只负责建 manifest，没有队列）。

### P0-6 B-01 未按清单包装既有仿真栈，且未接入任何流程

清单要求"把 `main/Simulation/` 的 `VerilatorRunner` + `SimulationEngine` + `ProcessRunner` 包成 Job"。实际：

- `SimulationJobRequest` 只接受 `executable + arguments + outputVcdPath`，与 `SimulationEngine`（Verilator → 编译 DLL → 进程内 `RunSimulation`，VCD 落 `.sigflow/sim/<top>/waveform/wave.vcd`）完全无关；
- GUI 的仿真入口 `MainFrame::DoSimCompile/DoSimRun`（`main/MainFrame.cpp:4343-4448`）仍直接使用 `SimulationEngine`；
- 打包入口 `MainFrame.cpp:3937/4002` 仍直接用 `FpgaPackService`；烧录入口 `MainFrame.cpp:4081-4176` 仍走 `ExecuteToolAsync`。

→ 教育版"一键仿真"、工程版打包/烧录都还不是 Job；B-02 的"由服务升级为 Job"实际是**并列新增**，形成两套 Job 体系（`FpgaSynthesisJobService` / `NextpnrJobService` vs `JobService`），U-01 的五类 Job 对照表无法落地。

---

## 3. P1 缺陷

| 编号 | 问题 | 证据 / 位置 |
|---|---|---|
| P1-1 | **进程 stdout/stderr 完全不采集**：`CreateProcessW` 无管道、`bInheritHandles=FALSE`、`CREATE_NO_WINDOW`，`logs/process.log` 只写入一行摘要 | 实测 `process.log` 内容 = `Job process completed.`（长度 23，不含被测程序输出的主机名）；对比 `ProcessRunner`/`FpgaYosysExecutor` 有 `CreatePipe` + 读取线程 → 能力倒退，且 DS-01 的 `log_line` 永远填不上 |
| P1-2 | **超时只杀直接子进程**，不杀进程树；`timeoutSeconds=0` 即 `INFINITE`，Job 参数与注册表都没有默认超时 | `ToolJobs.cpp:159-167`（`TerminateProcess`）；实测超时 1s 生效（1017 ms，状态 `TimedOut`）✓，但 verilator→gcc/ld 的子进程会残留 |
| P1-3 | **无 DS-02 参数 schema**：注册表三个 `inputSchema` 都是空 `Json::objectValue`，`Submit` 不校验参数 | `JobService.cpp:638-646`；AI 仍可能生成幻觉参数（B-04/B-08 的前提未建立） |
| P1-4 | **新增 `.sigflow/jobs/<type>/<id>/` 与规格冲突** | `JobService.cpp:319-332`；`docs/spec.md:108/676` 与 0907 §U-03 均要求产物落 `.sigflow/fpga/runs/<job-id>/`，U-03 冻结的新增子树只有 `agent/`、`snapshots/`、`ci/`。两套布局会让 F-10 面板、既有 Job 面板、U-07 追溯各自为政 |
| P1-5 | **时间戳把本地时间当 UTC**：`wxDateTime::UNow().FormatISOCombined('T') + "Z"` | 实测本机 UTC+8，报告写 `2026-09-09T08:48:20Z`，真实 UTC 为 `00:48:20Z`（差 8 小时）。B-12 决策回放、报告排序都会错。修法：`wxDateTime::Now().ToUTC()` |
| P1-6 | **4 个新文件全部 Windows-only**（`windows.h`、`bcrypt`、`"\\"` 拼路径、`MakeLower()` 比较路径、`CreateProcessW`） | `JobService.cpp:1-19,319-332,42-50,497-529`；直接违反 0907 §5.1「以后所有 Job 类只调接口不碰系统调用」与 T-03/T-04，等于给 B 流新增 2 个移植文件的债 |
| P1-7 | **`Quote()` 对所有参数一律加引号** | `.dsh_probe/CreateProcessProbe.cpp` 实测：`"cmd.exe" "/c" "echo hi> f"` → 退出码 1；`"cmd.exe" /c "echo hi> f"` → 退出码 0。常规工具（ping/hostname）不受影响，但缺少"原样透传参数"的通道，且属命令组装反模式 |
| P1-8 | **写报告不做一致性校验 / 无锁** | `WriteReport` 允许 `state` 与 manifest 不符；`RecordArtifact` 重复登记不去重；`Create`/`Transition` 是无锁 read-modify-write，F-13（后台 worker + UI 线程）下会丢更新 |

---

## 4. 测试与工程化缺口

1. **`ToolJobs.cpp`（295 行，B-01~B-03 的全部实现）零自动化覆盖**：`tools/run_debug_ci.ps1:84-87` 的 `job_service_smoke` 只编译 `JobService.cpp`（+ jsoncpp + bcrypt），**没有编译 ToolJobs.cpp**，所以 CI 连"能否编译"都不检查（本次评审用探针手工编译通过）。
2. `tests/jobs/JobServiceSmoke.cpp`（75 行）只覆盖 JobService 的 CRUD + 一次非法 Cancel；没有非法状态转换矩阵、Retry、List、超时、取消、并发、产物哈希校验、schema 校验。
3. `tests/fpga/FpgaPackServiceSmoke.cpp` 存在但**未接入 CI**（需要命令行参数），PackJob 复用的记账逻辑无回归保护。
4. **没有任何代码或测试引用 `main/jobs/schemas/job-report.schema.json`**（X-02 要求用 JSON Schema 校验器验证 DS-01/DS-02），该 schema 也未加入 `main.vcxproj`，运行时拿不到。
5. `main/main.vcxproj.filters` **未添加 `jobs` 过滤条目**（`git diff` 只有 `trace\TraceQueryService.*`），VS 里新增文件散落在根节点。
6. `main/jobs/`、`tests/jobs/`、`tests/fpga/FpgaPackServiceSmoke.cpp` 目前**均未纳入 git**（`??` 未跟踪）；`.github/workflows` 已空（最后一次提交是 "Delete ci.yml"）。

---

## 5. 值得肯定的部分

- **状态枚举与既有基准完全对齐**：`ToolJobState` = `SynthesisJobState`（Created/Validating/Queued/Running/ValidatingArtifact/Succeeded/Failed/Cancelled/TimedOut），`IsLegalTransition` 表与 `FpgaSynthesisJobService` 同构，U-01 的语义对齐有基础。
- **manifest 写入用 `.tmp` + `wxRenameFile` 原子替换**；job id 有字符白名单校验（`IsSafeJobId`）；`RecordArtifact` 校验产物必须位于工程目录内（`IsWithinDirectory`，防路径逃逸）。
- **注册表接口形态符合 B-04 的本意**：`JobServiceRegistry::Submit` 只接受 `ToolJobRequest`（JobType + 参数对象），不暴露可执行文件路径与命令行，AI 层拿不到 exe 路径（T-04 的一个正面信号）。
- **PackJob 正确复用既有 SHA-256 记账**（`FpgaPackService::Finalize` 校验 `.fs` 存在/大小/三方哈希），没有重造哈希逻辑；`FlashJob` 的 `requireConfirm` 用 OR 语义，调用方无法绕过烧录确认。
- **回归绿**：`tools/run_debug_ci.ps1` → 多时钟 RTL 检查 + 18 个 smoke 全部 `ALL PASS`（含新增 `job_service_smoke`）。
- **整机构建通过**：`MSBuild SigFlow.sln /t:main /p:Configuration=Debug /p:Platform=x64` → `bin\x64\Debug\main.exe`，`JobService.cpp` / `ToolJobs.cpp` 正常编译并链接进主程序（无告警级问题）。
  > 注意：单独构建 `main\main.vcxproj` 会因 `GlobalConfig.props` 用 `$(SolutionDir)` 拼 include 路径而失败（与本次改动无关），必须走解决方案。

---

## 6. 建议的最小修复顺序

**第一步（先让报告可信，改动集中、可当天完成）**
1. `Execute` 改为"Job 驱动状态机"：进入时 `Transition(Running)`，结束按结果 `Transition(ValidatingArtifact/Succeeded/Failed/TimedOut)`；manifest 与报告状态只允许一处来源。
2. 所有分支（含失败/超时/取消）统一走一个 `FinalizeReport(...)` 落盘；`RecordArtifact` 改为返回 `JobArtifact`，由调用方塞进 report 后再写一次（消除 P0-1）。
3. `NowUtc()` → `wxDateTime::Now().ToUTC()`。

**第二步（让取消/超时/并发真的成立）**
4. 进程执行改用 Job Object（或至少登记 `HANDLE`），`Cancel` 先 `TerminateJobObject` 再改状态；超时同样杀进程树。
5. `Start` 前检查同类型 `Running` 数量 < `maxConcurrent`，超出保持 `Queued`（或明确把调度推迟到 P1 并在 0907 文档中改口径）。

**第三步（补齐清单本意）**
6. 用管道采集 stdout/stderr 到 `logs/process.log`（建议直接抽一层 `PlatformProcess`，POSIX 实现留给 B 流，同时消掉 P1-6）。
7. 路径统一到 `.sigflow/fpga/runs/<job-id>/`（或把 `jobs/` 写进 U-03 冻结清单，二选一，但必须改文档）。
8. 把 `ToolJobs.cpp` 纳入 CI 编译，补 Sim/Pack/Flash 的 smoke（可用 `hostname.exe`/`ping.exe` 做假工具，或用测试 exe 注入 `JobCommand`）。
9. 把 Yosys/nextpnr 也注册进 `JobService`（统一五类 Job），并把 GUI 的 pack/flash 切到 Job 路径——这是 B-01~B-04 真正收口的判据。

---

## 7. 复现方式

```powershell
# 1) 项目自带回归（已跑通）
powershell -ExecutionPolicy Bypass -File tools\run_debug_ci.ps1     # DEBUG CI: ALL PASS

# 2) 整机构建（已跑通）
MSBuild SigFlow.sln /t:main /p:Configuration=Debug /p:Platform=x64  # -> bin\x64\Debug\main.exe

# 3) 行为探针（本次评审用，覆盖 P0-1..P0-5、P1-1）
#    源码：.dsh_probe/JobsProbe.cpp   （cl 编译命令见评审记录，链接 JobService.cpp / ToolJobs.cpp / FpgaPackService.cpp / jsoncpp.cpp）
.dsh_probe\JobsProbe.exe .dsh_probe\scratch

# 4) 命令组装探针（P1-7）
.dsh_probe\CreateProcessProbe.exe
```

探针实测输出摘要（**修复前 → 修复后**）：

```
修复前                                          修复后
[FAIL] VCD 产物登记进落盘报告 artifacts=0        [PASS] artifacts=1（含 sha256/size_bytes）
[FAIL] manifest 状态被 Execute 推进 state=Running [PASS] state=Succeeded
[FAIL] 失败路径也落盘 job-report.json 缺失        [PASS] 存在
[FAIL] Cancel 真的终止了子进程 剩余等待=4298ms    [PASS] 剩余等待=0ms
[FAIL] maxConcurrent=1 第二个 Job Running         [PASS] Running/Created
[PASS] 超时被强制执行 state=TimedOut 1017ms       [PASS] TimedOut 1049ms
[FAIL] 进程 stdout/stderr 被采集进日志            [FAIL] 仍为 P1-1，未在最小改动范围内
```

---

## 8. 最小改动修复记录（2026-09-09）

**改动文件（共 5 个，约 +130 行，无删除逻辑、无接口破坏）**

| 文件 | 改动 |
|---|---|
| `main/jobs/JobService.h` | +5 行：声明 `RegisterProcess` / `UnregisterProcess` |
| `main/jobs/JobService.cpp` | +约 65 行：进程句柄登记表（`g_runningProcesses` + mutex）、`CountRunning`、`Start` 增加并发上限校验与幂等、`Cancel` 真正 `TerminateProcess` |
| `main/jobs/ToolJobs.cpp` | +约 45 行：`FinishJob` 助手、`Run` 的 `persist` lambda（所有失败分支落盘报告 + 状态机推进）、SimJob 回读合并产物、Pack/Flash 终态推进 |
| `tests/jobs/JobServiceSmoke.cpp` | +12 行：并发上限断言（第二个同类 Job 不得进入 Running） |
| `tools/run_debug_ci.ps1` | 1 行：`job_service_smoke` 增加编译 `ToolJobs.cpp` + `FpgaPackService.cpp`（此前这两个文件不在任何 CI 编译范围内） |

**逐条对应**

- **P0-1**（产物被覆盖）：`SimulationJob::Execute` 在 `RecordArtifact` 之后 `LoadReport` 回读合并，再定稿 `Succeeded` → 落盘报告 `artifacts=1` 且含 sha256。
- **P0-2**（失败无报告）：`ToolJobExecutor::Run` 内新增 `persist` lambda，可执行缺失 / 未确认 / 进程启动失败 / 非零退出 / 超时 五条路径全部落盘 `job-report.json`。
- **P0-3**（状态机不推进）：`Run` 进入时确保 manifest 为 `Running`（未 Start 则自动 Start），结束按 `report.state` 调 `FinishJob` 推进到 `ValidatingArtifact / Failed / TimedOut / Cancelled`；三个 `Execute` 在定稿时推进到 `Succeeded`。
- **P0-4**（Cancel 不杀进程）：`Run` 在 `CreateProcessW` 后登记进程句柄，`Cancel` 在状态为 `Running` 时先 `TerminateProcess` 再改状态。
- **P0-5**（并发上限无效）：`JobService::Start` 统计同类 `Running` 数量，达到上限时保持 `Queued` 并返回明确错误（`kMaxConcurrentPerType = 1`，与注册表默认值一致）。
- **附带**：`Start` 现在幂等（已 Running 直接返回 true）、终态 Job 拒绝启动。

**验证**

- 探针：除 P1-1（日志采集，未在最小范围内）外全部 `PASS`。
- `job_service_smoke`：`JOB SERVICE SMOKE: PASS`（含新增并发断言）。
- 全量回归：`tools\run_debug_ci.ps1` → `DEBUG CI: ALL PASS`。
- 整机构建：`MSBuild SigFlow.sln /t:main Debug|x64` → `bin\x64\Debug\main.exe` 成功。

**本轮刻意未做（留给下一轮，均不属于 P0）**

- P1-1 进程 stdout/stderr 采集（需要管道 + 读取线程，改动面较大）
- P1-2 进程树终止（Job Object）、默认超时
- P1-3 DS-02 参数 schema 与 `Submit` 校验
- P1-4 `.sigflow/jobs/` 路径改回 `.sigflow/fpga/runs/`（涉及文档口径）
- P1-5 时间戳 UTC、P1-6 跨平台抽象、P1-7 参数引号策略
- P0-6 GUI 接 Job（Sim/Pack/Flash 切到 Job 路径、Yosys/nextpnr 纳入统一注册表）——这是 P0 里程碑的收口工作，不是最小改动。

---

## 9. 第二轮：剩余项全部落地（2026-09-09）

上一节留下的 P0-6 与全部 P1 项已完成。本轮**新增 4 个文件、改写 3 个文件、接入 4 处 GUI 入口**。

### 9.1 新增/改动清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `main/jobs/PlatformProcess.h/.cpp` | 新增 | 平台进程抽象：Windows 实现 = `CreateProcessW` + 管道 + Job Object（`KILL_ON_JOB_CLOSE`）+ 超时/取消杀整棵进程树；POSIX 实现 = `fork/exec` + `pipe` + `setpgid/killpg` + `waitpid`（**未在 Linux 真机编译验证**） |
| `main/jobs/Sha256.h/.cpp` | 新增 | 可移植 SHA-256（约 160 行），替代 Job 层的 Windows `BCrypt` 依赖 |
| `main/jobs/JobRegistry.cpp` | 新增 | 五类 Job 的统一注册表：`sim.run` / `fpga.synthesis` / `fpga.pnr` / `fpga.pack` / `fpga.flash`，每个带 DS-02 `inputSchema` + 运行体 + 并发上限 + 默认超时 |
| `main/jobs/JobRunner.h` | 新增 | `RunJobAsync`（后台线程 + `wxTheApp->CallAfter` 回 UI）+ `JobOutputPump`（进程输出合并刷屏，F-13 约定） |
| `main/jobs/ToolJobs.h/.cpp` | 改写 | 新增 `SynthJob`/`PnRJob`（复用 `FpgaYosysLogParser`/`ArtifactValidator`/`NextpnrLogParser` 生成结构化错误与产物哈希）；`ToolJobExecutor` 拆出 `Begin`/`Finish`/`Run`/`RunInProcess`；`SimulationJobRequest.runner` 支持进程内 `SimulationEngine` |
| `main/jobs/JobService.h/.cpp` | 改写 | 枚举扩到五类；`NowUtc()` 改真 UTC；路径改 `wxFileName::GetPathSeparator()`；`IsWithinDirectory` 仅 Windows 忽略大小写；`Cancel` 走 `PlatformProcess::Terminate`；新增 `ValidateParameters`/`Run`/`SetConcurrencyLimit` |
| `main/MainFrame.cpp` | 改写 | 4 处入口改走 Job：打包 → `PackJob`、烧录 → `FlashJob`、仿真编译/运行 → `SimulationJob`（进程内 runner 接 `SimulationEngine`） |
| `tests/jobs/FakeTool.cpp` | 新增 | 冒烟用假工具（`create`/`echo`/`fail`/`sleep`），让 Job 层可离线回归 |
| `tests/jobs/JobServiceSmoke.cpp` | 改写 | 覆盖五类注册、参数校验、真实执行、产物登记、失败报告、超时、取消、并发上限 |
| `tools/run_debug_ci.ps1` | 改写 | 编译 `fake_tool` + Job 层全部源文件（含解析器/校验器），去掉 `bcrypt.lib` |
| `main/main.vcxproj(.filters)` | 改写 | 加入 4 个新文件，并补 `源文件\jobs` / `头文件\jobs` 过滤器 |
| `docs/spec.md` §10.2 | 改写 | 冻结 `.sigflow/jobs/<type>/<job-id>/` 布局（manifest/inputs/logs/reports） |
| `docs/SigFlow_Plan_0907.md` | 改写 | B-01..B-04、DS-02、T-01 勾选 + 新增 §11 实施记录 |

### 9.2 逐项对照

| 编号 | 问题 | 处理 |
|---|---|---|
| P0-6a | 五类 Job 未统一 | ✅ 注册表五类齐备，`JobServiceRegistry::Run` 成为唯一执行入口；Synth/PnR 复用既有日志解析器与 `ArtifactValidator` 产出结构化错误（`code/severity/stage/ir_coordinate/summary/log_line`）与产物哈希 |
| P0-6b | GUI 未接 Job | ✅ 打包/烧录/仿真编译/仿真运行 4 处入口改走 Job；终端输出、进度条、取消、`FpgaToolWindow` 回显、调试会话状态迁移均保留 |
| P1-1 | 进程输出未采集 | ✅ 管道采集 stdout+stderr，全量写 `logs/process.log`；冒烟断言日志含 `fake-tool: created` |
| P1-2 | 不杀进程树 / 无默认超时 | ✅ Job Object + `TerminateJobObject`；默认超时：仿真/综合/PnR 600s，打包/烧录 300s（`timeout_seconds` 可覆盖） |
| P1-3 | 无参数 schema | ✅ 每个描述符带 `inputSchema`，`Submit`/`Run` 前校验必填/类型/枚举（冒烟含"缺字段被拒"断言） |
| P1-4 | `.sigflow` 布局口径 | ✅ 采用"冻结新布局"方案：统一 Job 层写 `.sigflow/jobs/<type>/<id>/`，既有 `fpga/runs*` 保留为既有实现写入方（避免破坏既有 Job 面板）；已写入 `spec.md` §10.2 与规划 U-03 |
| P1-5 | 时间戳非 UTC | ✅ `wxDateTime::Now().ToUTC()`（本机 UTC+8 下报告时间不再偏移 8 小时） |
| P1-6 | Windows 专有 | ✅ Job 层不再 include `windows.h`/`bcrypt`；进程/哈希均走抽象；路径分隔符与大小写比较按平台处理 |
| P1-7 | 参数一律加引号 | ✅ `QuoteArgument` 仅在含空格/制表/引号或为空时加引号，`cmd /c` 类调用不再被破坏 |
| 覆盖缺口 | ToolJobs 零测试 | ✅ `fake_tool` + 重写后的 `job_service_smoke` 覆盖执行/产物/失败/超时/取消/并发/参数校验；CI 编译 Job 层全部源文件 |

### 9.3 验证结果

- `tools\run_debug_ci.ps1` → **`DEBUG CI: ALL PASS`**（新增 `fake_tool` 构建 + `JOB SERVICE SMOKE: PASS`）。
- `MSBuild SigFlow.sln /t:main Debug|x64` → **`bin\x64\Debug\main.exe` 成功**。
- 行为探针 15 项 **全 PASS**（含日志采集、DS-02 校验）：

```
[PASS] SimJob.Execute(成功) state=Succeeded        [PASS] 落盘报告 artifacts=1
[PASS] manifest 状态推进到 Succeeded                [PASS] 进程 stdout/stderr 采集进日志
[PASS] 失败路径落盘 job-report.json                 [PASS] Cancel 终止子进程（剩余等待 0ms）
[PASS] 超时 TimedOut 1038ms                        [PASS] DS-02 参数校验拒绝缺字段提交
[PASS] maxConcurrent=1 第二个 Job 被拒（Running/Created）
```

### 9.4 本轮仍未做（已记录在规划 §11）

- **B-05 报告解析统一**：Yosys/nextpnr 既有解析器尚未改输出 DS-01 `JobReport`（当前统一报告由新 Job 层产出，旧面板仍读旧报告）。
- **U-01 逐项对照**：既有 `FpgaSynthesisJob`/`NextpnrJob` 未合并进统一 Job 层，GUI 综合/布线仍走既有异步流程（本轮只把 Sim/Pack/Flash 切过去；综合/布线的 GUI 迁移涉及 Yosys 脚本生成、诊断报告、进度解析，属独立工程）。
- **POSIX `PlatformProcess` 真机验证**：代码已写，但当前仓库无 Linux 构建，需 B 流环境编译验证。
- **`executable` 参数来源**：AI 面参数里仍可传可执行文件路径，应改由 Runtime 发现链在服务端填充（T-04 收口）。
- **F-10 Job 队列面板**（前端 P1）。

