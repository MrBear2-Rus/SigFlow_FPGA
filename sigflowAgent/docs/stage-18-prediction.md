# 阶段 18：显式预测与有限观察

日期：2026-10-16。Python 实现位于 sigflowAgent，保留独立的 agent 目录。端口属于私有 `u2.host.v1`，不是已冻结的共享 UI 协议。

## 动作与状态

具备 Gateway 动态证据配置的宿主发布 capabilities.prediction.configured；实际可用性仍取决于[可信波形契约](stage-17-trusted-waveform.md)。固定证据 fixture 启动方式默认 disabled。

WaitingStudent 状态接受以下两类动作。外层均需 kind、action_id、expected_version，普通调用通过认证及 X-Learner-ID 绑定；预测参数放入 prediction 对象。

```json
{"kind":"prediction_request","action_id":"predict-1","expected_version":"5","prediction":{"artifact_id":"art-example","signal_id":3,"start_tick":"0","end_tick":"0","sample_tick":"0"}}
```

request 只核验报告、产物、目录和时间范围，生成 300 秒 challenge，不查询信号值。等待状态含信号名称/位宽、timescale、摘要、Job/revision/snapshot 与 challenge，不含 observed。

```json
{"kind":"prediction_submit","action_id":"compare-1","expected_version":"6","prediction":{"challenge_id":"替换为当前32位十六进制challenge","expected":"0"}}
```

submit 验证明确的二态二进制预期和位宽，固定该预期后才查询真实事件。结果为 matches、differs 或 inconclusive；缺值/X/Z 不推测。返回 observed、有限结论、tick、timescale 和证据引用。请求中的页、来源、锚点、服务契约、Job 或本机路径均拒绝。

两类动作只消耗教学步骤预算；不启动 Job、不调用模型、不提升 L1–L3、不新增提示卡。step/version 与动作结果同事务提交；同 action/payload 重放不再读波形，改 payload 冲突。hint、finish、取消及重新取证使 challenge 失效。状态和重放必须属于当前 learner。

HTTP 的非法参数为 400；prediction_challenge_stale、prediction_unavailable、prediction_budget_exceeded 和 stale_state 为 409；scope_denied 为 403。波形服务在 worker 内变为不可用时，动作提交 unavailable 投影，普通提示仍可继续。capabilities 的 configured 只表示端口已装配。

## 恢复边界

challenge 与观察值不写入只读恢复检查点。进程重启后先发布 RecoveryRequired，隐藏历史观察；恢复必须重新取证并建立新 challenge。恢复前动作重放返回当前验证投影，不能重新发布旧观察。过期、旧 challenge、部分页及摘要变化均不会生成猜测结果。

## 运行与验证

仓库根目录运行：

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& sigflowAgent/.venv/Scripts/python.exe -P -m sigflow_edu_agent.host.closed_loop --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --scenario simulation --waves --predictions --timeout 240 --output sigflowAgent/docs/closed-loop-stage18-prediction.json
```

`--predictions` 必须与 simulation 和 --waves 同时启用。运行器在自己持有的临时工程中执行真实工具；预期 0 是独立固定输入，不从观察值反推。

定向测试包括控制器 8 项、事务宿主/HTTP 7 项，以及 CLI 参数和原仿真转移回归。覆盖提交前零查询、修改/过期/重用、四态不确定、uint64 最大 tick、摘要变化、重启恢复、跨 learner 和注入拒绝。

[真实报告](closed-loop-stage18-prediction.json)记录 q 在 tick 0 的 expected=0/observed=0、matches；新增 Job=0、模型调用=0、提示等级不变、重放通过。全量统计与独立审查状态见[汇总](verification-stage17-19.json)。

## 再规划

可复现静态预测闭环已具备。复位根因和动态仿真错配仍需独立预期、输入刺激及采样契约，不能由一次静态比较替代。后续参考答案只在已审阅材料与真实 UI 签收条件齐备后启用。
