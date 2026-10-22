# 阶段 21：参考元数据宿主动作与显式恢复

日期：2026-10-17。实现位置：sigflowAgent。前置协议见[阶段 20](stage-20-durable-reference.md)，设计及任务边界见[阶段计划](u2-stage21-reference-host-plan.md)。

## 交付范围

常驻 Runtime 增加内部 `reference_request`、`reference_confirm`、`reference_reconcile` 动作。请求 challenge 与状态、检查点、动作结果、事件同事务提交；确认和恢复沿用阶段 20 的原 receipt 日志。普通宿主状态只发布材料标识、摘要和固定状态，不提供参考正文，不生成 L4 卡片。

HTTP capabilities 的 L4 仍为 unavailable，三个参考动作在 HTTP 上均返回 `403 receipt_unavailable`；附带未知字段返回 `400 unknown_fields`。内部端口配置不会打开 HTTP 能力。现有 TeachingLoop、SessionBinding 策略和只读检查点格式保持兼容。

## 内部可信装配

`Runtime(..., reference_provider=provider, receipt_port=port)` 必须成对传入。缺省均为 None，内部动作也拒绝为 receipt_unavailable。provider 接收完整 SessionBinding 并返回 ReviewedReference；port 提供 lookup/consume，若带 GatewayConfig，实例必须与 store 相同。普通 HTTP、模型输出和请求字段不能配置上述端口、正文、reviewer 或文件路径。

当前 CLI/启动器不装配真实参考材料。两位 reviewer 标识的格式和集合约束不构成教师身份认证；正式材料供应需要另行提供可信登记、审阅与撤销契约。

| 内部 action kind | reference payload | 提交后的 reference 状态 |
| --- | --- | --- |
| reference_request | `{}` | awaiting_confirmation，包含 challenge 元数据 |
| reference_confirm | `{challenge_id, receipt_id}` | confirmed / pending / unavailable |
| reference_reconcile | `{}` | confirmed / pending / unavailable |

调用形式为 `runtime.action(sid, learner, kind, action_id, expected_version, reference=payload)`。expected_version 为内部整数；每个动作仍使用宿主 action_id 和 payload 摘要进行幂等控制。不同 payload 复用 action_id 返回 action_conflict。receipt_id 只进入私有 reference_records，不进入动作参数副本或通用动作结果。

仅允许只读 WaitingStudent/L3、当前完整绑定和当前版本。每个被接受的动作使用一个已有 steps 预算，不增加卡片、提示等级、模型调用或 Job。无材料、材料变化或传输失败发布固定原因码，保留教学会话；finish 和取消仍可使用。

## 事务与版本边界

gate.draft(current_version, current_version + 1) 重新检查材料并生成下一版本 challenge，本身不写日志。ReferenceJournal.commit_request 在同一 DurableStore 事务中先执行内部 _commit，再按已更新的会话版本执行 _prepare。新 challenge、状态、checkpoint、action result 和 event 共同成功或回滚。绑定不符、取消、CAS 冲突、未知操作、挑战版本不符或 SQL 写入失败均不能留下部分发布。

私有 journal 的 consuming 在外部 Core.consume 前提交。Core 成功后 release 可先于普通动作结果提交：它仅表示内部核销确认，不是正文已发布。若此后进程中断，重启仍先隔离会话；显式重新取证和 reference_reconcile 后才能重新发布当前元数据。普通 action replay 从不调用 Core，也不返回正文。

Runtime 使用 metadata_only gate；返回值仅含 reference_id、sha256、policy、reviewers，状态另外标记 `body_available=false`。正文只由 gate 从可信 provider 读取并验证，不能进入 state/checkpoint/action/event。发布投影将取消、RecoveryRequired、终态和过期 challenge 隔离；旧参考动作重放返回当前投影，避免复活旧挑战。

阻塞 receipt 调用期间，其他请求不会在 Runtime 全局锁内等待该会话的锁。has_pending 仅作持久预留的快速检查，最终准入仍由 claim 的事务/CAS 决定；重复 action_id 的重放先于忙碌检查。取消从已提交的绑定/run 直接失效日志，不访问被外部调用持有锁的 session.state。

## 恢复与撤销

1. 请求提交后的 challenge.state_version 与宿主 state_version 相同，供 UI 侧将来签发绑定 receipt；内部 receipt 路径本身不能签发。
2. confirm 核对 active receipt 并预留 consuming，再请求 Core.consume。响应丢失或成功后的材料重验失败保留 pending 和原操作，不自动换 receipt 或重试。
3. 数据库重开后状态为 RecoveryRequired，卡片与参考元数据隔离。普通 recover 使用原问题摘要和新取的工程证据进行核验，不读取或消费 receipt。
4. 原 consuming/released 记录投影为 reconcile_required。显式 reference_reconcile 从私有日志取原 receipt/action/run/审批版本，当前宿主版本用于本地 CAS；Core consumed 重放仍使用旧审批版本。
5. 原 ready challenge 经重启重验后不可确认，需要新请求。取消无需恢复会话即可将旧记录置 invalid，保留 receipt 审计；失效且携带 receipt 的记录禁止覆盖。

其他教学/预测动作使当前参考上下文失效。正文投递、GUI 双确认、教师身份认证和材料撤销后的用户呈现不在本阶段开放。

## 复现

在仓库根目录使用已有独立环境，无需开启 Conda 或安装 UCAgent：

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& sigflowAgent/.venv/Scripts/python.exe -P -m unittest discover -s sigflowAgent/tests -p test_host_reference.py -v
& sigflowAgent/.venv/Scripts/python.exe -P -m unittest discover -s sigflowAgent/tests -p test_reference_journal.py -v
& sigflowAgent/.venv/Scripts/python.exe -P -m unittest discover -s sigflowAgent/tests -p test_durable_reference.py -v
& sigflowAgent/.venv/Scripts/python.exe -P -m unittest discover -s sigflowAgent/tests -v
```

Core 探针使用已构建的可选 headless 宿主、真实 receipt 路由和明确 synthetic 材料/教学证据。增加 `--host-metadata` 才启用内部 Runtime 场景；默认探针和 `--durable` 原行为不变。

```powershell
& sigflowAgent/.venv/Scripts/python.exe -P -m sigflow_edu_agent.host.receipt_acceptance --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --durable --host-metadata --output sigflowAgent/docs/receipt-stage21-host.json
```

该探针只重开 Agent 数据库，不声称操作系统强杀、真实 GUI 重启或教师签收。真实核销成功后主动丢弃第一次响应，第二次显式核对同一操作；不运行 EDA 或模型。最终计数、摘要、审查和边界见[验收记录](verification-stage21.json)。

## 最终验证记录

最终全量运行 614 项，581 成功、33 单项跳过，另 2 个类级跳过覆盖 18 个未运行方法；共收集 632 个方法，跳过记录 35，失败/错误 0，耗时 115.086 秒。缺失原始课程快照及历史可选 UCAgent 的跳过不计作验收通过。[开发评测](evaluation-stage21.json)为 40/40，[交接报告](evaluation-stage21-handoff.json)仍记录冻结集缺失和教师签收未进行。

受限进程第一次全量有 3 个既有 DPAPI 配置错误，独立虚构载荷验证返回 Windows 0x2；没有修改配置实现或删除测试。当前 Windows 用户环境的 17 项配置测试及修复后的最终全量均通过。测试凭据仅存在于自建临时目录，不读取真实 API key，不调用真实模型。

宿主参考动作 16、journal 25、gate 19、探针 2、原 receipt CLI 1 项均通过；原宿主/store/预测定向回归通过。初始独立审查运行 111 项，发现全局锁阻塞 P2；新增两个 Event 场景先失败后通过，复审独立核验第二次 confirm、取消和 snapshot 均在外部调用仍阻塞时及时结束，并运行 71 项通过。无遗留 P1/P2。真实 Core 探针共执行两次，修复后最终代码再次通过；审查者未独立重跑真实 Core 或全量。

源码摘要、关键文件/二进制摘要、课程数据库未变化及未暂存状态记录于[验收汇总](verification-stage21.json)。阶段 20 的测试数字和摘要为其历史验收，不代表阶段 21 的当前代码摘要。

## 后续阶段

内部元数据接线可用于 U1/U2 开发对接，公开参考答案能力仍等待可信受审材料供应与 GUI 双确认契约。下一阶段先明确材料登记、教师身份与撤销条件，再单独设计正文访问和呈现；不得通过普通动作重放或持久状态投递答案。UA-07 的动态激励/其他 detector、UA-08 的团队冻结集/教师评分及目标 OS 分项验收继续独立保留。本阶段不提交或推送。
