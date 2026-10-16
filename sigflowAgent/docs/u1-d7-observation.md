# D7：验证任务的显式只读续查

日期：2026-10-15。范围为 U2 长 Job 等待、结果证据稍晚到达、进度展示及后继审批。复用 D4 的持久提交账本与 D6 的可信动态目录，不自动恢复执行授权。

## 动作与状态

原路径对有界等待后仍 Running/Unknown 的任务进入 RecoveryRequired，没有普通 HTTP 续查动作；Succeeded 但尚无可信结果锚点时直接 Failed。新增 `observe_verification`，仅适用于当前进程仍持有的验证会话：

| 当前状态/原因 | 允许动作与结果 |
| --- | --- |
| RecoveryRequired / verification_status_pending | 显式观察已经提交的步骤，不提交新 Job |
| RecoveryRequired / verification_evidence_pending | 重新读取可信目录与报告，等待匹配的结果证据 |
| 查询仍为 Running/Unknown | 保持 RecoveryRequired，清空对外卡片，增加版本；需要新的显式观察动作 |
| 观察成功且有可信证据、计划已经结束 | 进入 E7 并发布有限反馈；没有观察比较器时为 outcome_indeterminate |
| 观察成功且存在后继 | 暂停 WaitingApproval，原因 successor_approval_required；显式 execute_plan 并重新核对 grant 后才可继续 |
| Failed/Cancelled/TimedOut/Rejected | 保留已确认失败，不因网络抖动或矛盾的新响应而清除 |
| 进程重启、取消或无 live 会话 | 不开放观察动作；重启不还原执行授权或未保存的执行上下文 |

普通 cancel 动作可取消 live 的待查验证会话，仍要求 action_id 和当前版本；独立 `/cancel` 继续允许中断 pending，并绕过客户端动作容量与重启会话的 live 限制。教学 Cancelled 和真实 Job 终态继续分别表达。

教学 HTTP 路由沿用 D3 的扩展协议 `u2.host.v1`：

```json
{"kind":"observe_verification","action_id":"observe-1","expected_version":"7"}
```

提交到 `/api/v1/teaching/sessions/{sid}/actions`，需要 UI token 和匹配的 X-Learner-ID，返回 202 后轮询 state。此动作不接受 question、grant_ref、Job ID 或可信证据。动作键同请求幂等、不同请求冲突；旧版本不能推进。重复观察受每会话动作上限和主循环阶段步预算约束，不增加模型调用数。

## VerificationPort

`GatewayVerification.observe(plan, step_index, idempotency_key)` 及其不可变审批绑定端口仅观察既有记录：

- 精确核对当前 instance、project、revision、snapshot、完整计划指纹、UI hash、本地审批包 digest、步骤、能力和 plugin。
- 只从持久账本读取 Job ID；没有记录时拒绝，有未知提交但无 Job ID 时返回 Unknown，不枚举或猜测 Job。
- 公共 health 先验证当前实例/协议，再发送认证 GET Job/report；不读取 grant，不 POST Job 或 cancel。
- 失效、用尽或撤销的 grant 不妨碍读取已知 Job；读到成功也不恢复后继执行权限。
- 无结果锚点、报告不完整或缺少可证实诊断时不返回 Evidence；工具成功不推断预期符合。

`VerificationResult` 新增可选 `job_id`（默认 None，兼容原三参数构造）。Job ID 可以在 Running/Unknown 时已知，不依赖结果 Evidence 才显示。主循环不会把 job_id 与 Evidence 中不同的 Job 当作有效结果。

临时 TIMEOUT/SERVICE_UNAVAILABLE，无论发生于基线重新取证还是验证端口握手，均保持可显式重试的 RecoveryRequired，不自动重试网络、不重交 Job。动态目录临时缺失、报告不完整或诊断缺失保持等待证据；实例、权限、快照或证据完整性错误继续拒绝。已经确认的失败终态和 ObservationMismatch 是持久停止事实，跨适配器刷新及对账也不能覆盖。

## 状态进度与 trace

state 新增 verification，包含 completed_steps、step_count、last_observed_step_index、job_id、job_state、result_evidence_ready、can_observe。字段描述最后一次可信观察；不是 Gateway 的实时推送，也不是执行权限。重启或取消后的响应强制 can_observe=false。RecoveryRequired 始终隔离旧卡片。

state 同时提供有界 trace：stage、status、reason、字符串 state_version。仅包含内部阶段与稳定原因码，不包含原问题、模型输入、源码、grant、请求 Header、执行幂等键或凭据。trace 受原有最多 32 步预算约束。

## 账本兼容

验证表新增 approval_digest 列，旧表在初始化时以空字符串迁移。新请求在预留事务内写入完整审批包摘要，observe 和 cancel_owned 都按该摘要核对；相同 UI hash 下不同版本/plan_id 的本地材料不能接管原记录。

旧记录的空摘要不能直接获得只读权限。可信调用方必须使用原 grant 和完整原请求做已有键核对，或提交符合现有约束的可信 Core provenance 对账，才补齐摘要；该路径不会重交 Job。取消门禁的新摘要包含本地审批包，旧版门禁仍参与读取与新提交的原子拒绝条件，不因迁移丢失已有取消意图。

## 验证与再规划

定向命令（在 sigflowAgent 目录）：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_verification_observation.py -q
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_host_coupled.py -q
```

最初 11 项新测试观察到 10 个缺接口错误和 1 个缺证据被终止的断言失败；实现后通过。后续回归复现并修复旧 port 误报能力、关闭期间提交、取消入口差异、审批包隔离、握手瞬断、失败终态保持，以及基线网络瞬断导致永久终止的问题。最终定向测试为 19 项观察测试及 5 项组合 HTTP 测试，通过独立审查。

组合 fixture 演示：同一 Job 先 Running，后 Succeeded 但缺结果锚点，随后可信目录补入；同一教学会话通过显式观察进入 E7，全程仅一个 Job POST、零模型调用。另覆盖 410 游标后状态高水位重同步。

仍未完成真实 Gateway/EDA、动态锚点的可信生产方、完整功能观察比较器或跨进程验证恢复。下一步在真实服务环境提供上述输入并执行受控联调；不把 fixture 成功记为真实工具验收。
