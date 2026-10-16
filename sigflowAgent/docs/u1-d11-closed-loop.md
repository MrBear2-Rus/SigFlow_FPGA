# U1 D11：真实 Core/Yosys 教学闭环

日期：2026-10-15。Python >=3.11，仅使用标准库；本次使用本目录 `.venv` 的 Python 3.13.5，不要求 Conda 或 UCAgent。构建使用已有 MinGW/MSVCRT 缓存与随仓库部署的 Yosys；POSIX 路径尚未实测。

此入口在自己创建的临时目录内运行两个真实 Yosys 综合 Job：基线、教学计划审批后的复测。使用真实 CoreJobService、官方插件、Gateway HTTP 和独立 Python sidecar。任务完成与教学反馈中的 `outcome_indeterminate` 不代表 RTL 功能正确。

## 构建（已有本地依赖/编译器）

仓库根目录 PowerShell：

```powershell
cmake -S . -B build-main-msvcrt -DSIGFLOW_BUILD_TESTS=OFF -DSIGFLOW_BUILD_U2_INTEGRATION=ON
cmake --build build-main-msvcrt --target edu_u2_gateway -j 4
```

`SIGFLOW_BUILD_U2_INTEGRATION` 默认 OFF；启用时要求 `SIGFLOW_BUILD_EDU_AGENT=ON`。目标只链接 Core、Gateway、平台层、官方 Yosys 插件与线程库，不链接 wx，也不导入旧 `agent` 包。GNU Windows 构建将编译器的三个运行时 DLL 复制到宿主旁边，无须先构建 GUI。现有构建树仍使用自己的编译器和依赖缓存，不下载工具。

Windows 真实工具来自 `external/fpga-tools/runtime/yosys/bin/yosys.exe`；POSIX 使用 PATH 中的原生 `yosys`。宿主实际运行 `yosys -V` 探测，缺失或不能执行时失败，没有跳过后算通过的分支。

## 运行

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.host.closed_loop --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --mode rule --timeout 180 --output sigflowAgent/docs/closed-loop-rule.json
```

`rule` 为默认值，移除继承的三个 `DEEPSEEK_*` 设置，并将配置路径指向自己的不存在文件；显式移除 `SIGFLOW_U2_ANCHORS`。子进程只导入 `sigflowAgent` 包，以 `-P` 禁止当前目录抢占导入。所有子进程使用 argv，Windows 使用隐藏窗口标记。

可选的真实模型验收：

```powershell
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.host.closed_loop --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --mode deepseek --timeout 180 --output sigflowAgent/docs/closed-loop-deepseek.json
```

DeepSeek 模式保留当前用户的本机配置。Windows DPAPI 配置必须由有权解密的正常 Windows 用户运行；受限身份可能读取不到该配置。缺 Key/Provider 明确失败 `model_unconfigured`。主会话的初始卡必须 `producer=model`，即使存在配置，模型失败后规则降级也会使验收失败。最终证据反馈仍应 `producer=rule`。辅助取消会话始终使用规则模式，不额外调用模型。

## 验收与报告

成功退出码为 0，JSON 包含 `status=passed`、`real_core=true`、`real_yosys=true`、`model_mode`、不同的 `baseline_job_id`/`result_job_id`、互不重合的新旧证据 ID、递增的状态版本、`Completed`/`outcome_indeterminate`、`grant_used_jobs=[1,1]`、`cleanup=completed`、`functional_correctness=not_evaluated`。失败退出码为 1，仅返回固定错误码，不输出原始异常、子进程 stderr、问题、凭据或本机路径。输出目录须已存在。

运行器还断言：Agent 不能签发 grant；显式错误或非字符串 provider 在消耗 grant/创建 Job 前被拒绝；重复 Gateway 请求返回同一 Job；另一请求不能重复消耗同一审批步骤；教学 create/action 重放不创建新 Job 或改变最终版本；旧状态版本被拒绝；另一个等待审批的会话取消后不会创建 Job。

Gateway 的 Agent、Gateway UI、教学 UI 为三个不同随机身份。凭据经 stdin 交付；sidecar 只获得 Gateway Agent 身份和自己的教学 UI 身份，永远不获得 Gateway UI 签发身份。Core ready 行只含协议/实例/nonce/端口/工程 ID。Core 停机先停止接受请求，再取消自己工程的 Job，并等待服务 worker 退出。父进程有界地关闭 sidecar、关闭 Core stdin，必要时仅终止自己持有的子进程；最后由 TemporaryDirectory 清理自己创建的工程，不触碰用户工程。

Core 元数据中的 artifact role 可以是插件内 artifact ID，报告 role 可以是 `primary`。可信归属由项目/revision/Job/快照、artifact ID/hash/schema 校验建立。`--gateway-evidence` 与 `diagnostic_id=job-report` 从真实报告生成任务观察，不猜测源码位置或诊断根因。

## 定向测试

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& .\sigflowAgent\.venv\Scripts\python.exe -P -m unittest discover -s sigflowAgent/tests -p test_closed_loop.py -q
& .\sigflowAgent\.venv\Scripts\python.exe -P -m unittest discover -s sigflowAgent/tests -p test_host_verification.py -q
```

前者验证隔离、握手拒绝、缺宿主失败、子进程 ready 超时与清理。后者使用明确标注的 HTTP fixture 验证适配器边界，包含真实 Core 的 build 插件 `eda-sim-verilator` 和 run 插件 `eda-sim-verilator-run` 精确区分及错误 ID 拒绝；fixture 测试不替代真实 Yosys 验收。

## 最终验收记录

三阶段实现与组合独立审查通过。最终复审确认清理失败、管道排空超时、报告写入失败均返回失败，不会沿用执行成功的状态。

| 检查 | 实际结果 |
| --- | --- |
| 全量 unittest | 421 项，415 通过、6 项历史可选 UCAgent 实验跳过，68.448 秒 |
| 旧 agent 独立回归 | 124/124 通过；与新服务使用独立包路径 |
| 开发评测 | 40/40 通过；合成开发场景，不能替代真实执行 |
| 真实规则闭环 | 通过；2 个真实 Yosys Job，模型调用 0 次 |
| 真实 DeepSeek 闭环 | 通过；2 个真实 Yosys Job，初始卡来自模型，实际调用 1 次 |
| 两种真实模式共同检查 | 可信新旧证据不同、WaitingApproval v5→Completed v8、各 grant 使用 1 次、重放稳定、旧版本拒绝、审批前取消、清理完成 |

机器证据：[规则闭环](closed-loop-rule.json)、[DeepSeek 闭环](closed-loop-deepseek.json)、[开发评测](evaluation-u1-closed-loop.json)、[源码与构建摘要汇总](closed-loop-verification.json)。报告仅保留脱敏执行事实，不保存 Key、token、原始问题或模型卡片。全量测试中故意触发缺宿主失败的回归会输出 `gateway_missing` JSON；应以 unittest 的最终结果及独立真实闭环报告判断是否通过。

复跑全量及开发评测，在仓库根目录的正常 Windows 用户终端执行：

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& .\sigflowAgent\.venv\Scripts\python.exe -P -m unittest discover -s sigflowAgent/tests -v
& .\sigflowAgent\.venv\Scripts\python.exe -P -m sigflow_edu_agent.evaluation --suite sigflowAgent/tests/evaluation/development.json
```

## 尚未验收

本次主路径为同一 revision/snapshot 上的真实综合闭环。真实 Verilator build/run、跨 revision 的学生编辑与重新审批、GUI 操作、硬件功能正确性和教师教学质量验收均未覆盖。不能用综合成功或证据更新替代这些验收。

## 失败与关闭期限（最终复审修正）

验收状态还取决于子进程排空、临时目录清理和报告落盘。任一步异常都返回非零退出码和 `status=failed`，不会保留 `cleanup=completed`。如果真实执行已完成，其事实保留在失败报告的 `execution` 字段，与整体验收结果分开。

每个子进程关闭时先有界等待自身退出，必要时仅终止该直接持有的子进程，再给 stdout/stderr 读取线程合计 1 秒排空时间。读取线程独占自己的缓冲流关闭操作；父线程不会在读取尚未结束时同步关闭缓冲流，因此后代继承管道句柄不会无限延长关闭期限。排空未结束会报告固定错误 `child_drain_timeout`。即使一个子进程关闭失败，也会继续尝试关闭另一个拥有的子进程。不会按进程名批量终止进程。

若报告输出文件无法写入，stdout 仍给出脱敏 JSON（`error=report_write_failed`），不会输出文件路径或原始异常。输出文件本身在这种情况下不可用，须以 stdout 的失败结果为准。有限后代管道回归在独立测试进程中运行并等待该后代自然结束，测试不会遗留挂起进程。
