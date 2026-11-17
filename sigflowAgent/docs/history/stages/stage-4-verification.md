# 阶段 4：计划与复测检查

日期：2026-10-10。状态：本地纯逻辑完成；UA-05 真实执行闭环仍待联合验收。

## 接口与行为

`checkers` 导出 `PlanStep`、`DetailedPlan`、`PlanChecker`、`OutcomeChecker`、`Outcome`。
详细计划绑定 project、revision、snapshot、goal、步骤 provider/参数/依赖、预期观察、停止条件和最多 3 Job。
参数与输入序列复制为不可变值；摘要包括所有这些字段，参数键和停止条件规范化。
摘要是本地身份，不是 Gateway grant；摘要变化必须由可信适配器重新审批。

本地允许矩阵（并非共享协议扩展）：

| 能力 | provider | 必需参数 | 可选参数 |
|---|---|---|---|
| eda.synth | yosys | top | 无 |
| eda.sim.build | iverilog / verilator | top、testbench | 无 |
| eda.sim.run | iverilog / verilator | build_step_id | timeout_ms（1–60000） |

未知参数（包括任意 opts）、未知 provider、缺失输入、重复步骤、重复依赖、前向/循环依赖、build/run provider 或依赖关联不符均拒绝。
top 和 testbench 只接受 HDL 模块标识符，不能传入路径；top 是本地字段，真实 adapter 须映射到共享 top_module。真实文件归属与 artifact 解析仍由执行适配器负责。
`PlanChecker.check` 在循环构建和授权边界重新校验详细计划。
原 `Plan` 保持离线兼容，两种计划均显式 `gateway_ready=False`。

`OutcomeChecker.compare(old, new, target_revision, *, project_id, matches_expected,
seen_job_ids=(), snapshot_id=None)` 返回 matches/mismatch/indeterminate/rejected。
旧证据允许旧 revision；两份证据必须完整、属于同项目与概念，结果必须属于目标 revision，
不能复用旧 Job、已见 Job 或旧证据 ID。详细执行在提交前检查当前证据 anchor，并要求结果 anchor 绑定计划 snapshot。
`matches_expected` 仅接受可信端口的严格 bool/None，不从文本猜测功能结论。

`TeachingLoop(..., baseline_evidence=old)` 显式传入旧版本基线；source 始终收集目标 revision。
未传基线时使用循环初次采集证据，保留既有同版本复测用法。
失败 Job 停止后继；默认详细计划的观察不符也停止后继。反馈只表达本次覆盖范围。
不符进入 WaitingEvidence，清除 grant；显式 refresh 后进入 WaitingStudent，不自动重跑。
重做验证须由宿主创建新计划/循环并重新审批；默认端口始终拒绝执行。

## 实测记录

工作目录：`sigflowAgent`。先新增行为测试，运行下述定向命令产生
`ModuleNotFoundError: sigflow_edu_agent.checkers`，确认能力缺失，再实现。
新增畸形 baseline 测试先观察 AttributeError，再改为稳定 ValueError。

```powershell
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -p test_checkers.py -v
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -v
```

首次阶段 4 检查：定向 9 tests OK；当时工作区全量 74 tests OK（6 个可选上游集成 skipped）。
审查补充模块标识符与提交前 snapshot 测试，先观察 7 个失败断言，再修复。
随后将上述命令的 `-v` 换为 `-q` 重跑：定向 11 tests OK；当时全量发现 107 项，101 项通过、6 项跳过。
独立审查又发现提交前已存在 Job 可被误当新结果、多步结果只看最后一步、旧 Plan 畸形能力触发 TypeError。
新增回归先复现问题，再分别加入提交前 Job/证据去重、全步骤三态汇总和类型拒绝。
修复后定向 14 tests OK；独立复审确认三项已修复。
最终整体验收发现端口注解的 DetailedPlan 仅在 TYPE_CHECKING 下导入，
导致运行时 get_type_hints 失败；补充回归复现后改为可解析的正常导入。
最终定向 15 tests OK。全量结果见阶段 6 报告。
多步结论中任一步不符要求重新规划；任一步未知且无不符时标记 outcome_indeterminate。
卡片引用所有已执行步骤的证据，不以最后一步成功掩盖此前不确定性。
`-S` 隔离主机 site 初始化，证明核心检查不依赖 UCAgent 或模型依赖。
本阶段未据此声明真实 UCAgent、Gateway、grant、Job 网络联调通过。

## 再规划与交接

- UA-05 计划校验、前后证据边界和失败不执行后继：本地完成。
- S1/S2 仍需真实 snapshot/artifact、build Job 关联、目标版本报告与授权摘要绑定。
- U1 仍需真实执行适配器、StageManager 生命周期、持久化及恢复。
- 预期观察目前是受限计划文字，由可信 adapter 给出 bool/None；没有波形/硬件语义自动判定器。
- 阶段 5 可继续消费 Evidence；阶段 6 可直接复用这里的真实检查器。共享 wire contract 未改。
