# 阶段 20：持久参考发布协议

日期：2026-10-17。基线提交 a8431c5 已按要求推送；本阶段新开发保持未暂存、未提交、未推送。实施顺序和约束见[计划](u2-stage20-durable-reference-plan.md)。

## 问题与范围

阶段 19 的 ReferenceGate 仅保留进程内 challenge。如果 Core 已核销，而响应丢失或 Agent 在本地发布前崩溃，重新签发会混淆已使用与未使用授权。本阶段增加持久协议，恢复时始终使用原 receipt/run/action/审批版本，并重新核验当前教学上下文和材料。

本阶段提供内部可信宿主端口，未启用生产教学 HTTP L4。审阅材料供应、教学动作结果的原子发布接线和真实 GUI 签收仍需后续阶段；协议测试中的正文与 reviewer 标识均为明确 synthetic fixture，不能作为教师验收。

## 日志与状态机

`ReferenceJournal(store)` 复用现有 DurableStore 的 SQLite 连接、单进程锁、FULL 同步及事务。reference_records 外键绑定现有 sessions，每 session 一条记录。保存全 SessionBinding、run、challenge、材料/审阅上下文摘要、原审批版本和恢复所需的私有 receipt_id；正文、问题、API key 不入新增日志或机器报告。

```text
request → ready → claim/consuming → Core.consume → release/released → 返回正文
                         │
                         └─ 异常/重启 → 保留同一操作 → 显式 reconcile
```

prepare 拒绝覆盖 consuming。invalidate 保留 receipt 和审计信息；已失效但仍携带 receipt 的记录同样不能被新请求覆盖，以免丢失不确定操作。即使该 receipt 先前已 released，也采用这一保守规则；新的独立教学问题应使用新 session。尚无 receipt 的失效 challenge 可以替换。

claim 的同 receipt 重放幂等，换 receipt 拒绝。release 只允许 consuming→released 或 released 幂等。数据库事务内检查绑定、取消/终态、WaitingStudent、整数 level=3 和当前版本，避免阻塞期间的状态变化被忽略。读取内部审计记录允许在隔离/取消状态进行，不因此授予发布权限。

## 恢复 gate

`DurableReferenceGate(binding, run_id, provider, journal)` 使用可信材料端口提供 ReviewedReference，不接收模型或普通 HTTP 传入的材料/审阅身份。

- `request(state_version)`：检查当前 L3 上下文；生成 300 秒随机 challenge/action，持久化元数据后返回，不返回正文。
- `confirm(receipt_id, challenge_id, state_version, port)`：核对 Core active receipt 的完整绑定，先持久化 consuming，再核销；成功回执核验、重新获取相同材料并提交 released 后，才返回正文。
- `reconcile(state_version, port)`：仅恢复 consuming/released。receipt 标识只能来自私有日志；恢复沿用旧审批版本，当前本地版本则用于重新取证后的 CAS。
- `invalidate()`：本地失效但保留审计。取消/终态不得继续核销或返回正文。

材料正文摘要、reference_id 及 reviewer 集合摘要必须保持一致。阻塞 Core 调用后还会重新核验材料，released 重放也重新读取材料；材料撤回或变化不返回旧答案。未知回执、畸形回执或本地 release 失败保持 consuming，不自动签发或再次执行新操作。

active receipt 必须处于本地 challenge 有效期内，SQLite 预留完成后再次检查 TTL。Core 已 consumed 的同操作恢复可以跨 TTL，但必须通过 Core 同 run/action/version 的可信重放确认。DurableStore 重启仍先转为 RecoveryRequired；必须由可信宿主显式重新取证后恢复 WaitingStudent，gate 不绕过这条门禁。

## 测试证据

日志层从缺失实现的 15 项失败开始，初步 15 项通过；独立审查发现失效记录覆盖问题，新增 2 项回归先失败后修复，最终 17 项通过。覆盖全作用域隔离、竞争核销仅一个赢家、回滚、取消、重启、原审批版本与当前版本的区分及私有正文不落盘。

gate 初始缺实现用例失败，随后 16 项通过；SQLite 预留期间过期的新增回归先失败，修复后最终 17 项通过。覆盖响应丢失、本地提交失败、发布后重启重放、材料/审阅变化、取消、TTL、畸形回执和失效记录保留。CLI 的 1 项用例验证 --durable 必须显式启用，默认探针保持原行为。

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& sigflowAgent/.venv/Scripts/python.exe -P -m sigflow_edu_agent.host.receipt_acceptance --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --durable --output sigflowAgent/docs/receipt-stage20-durable.json
```

[真实 Core 报告](receipt-stage20-durable.json)记录：一次 UI 签发；Core 成功核销后故意丢弃响应；重新打开 Agent 数据库并验证 RecoveryRequired 拒绝自动恢复；显式 synthetic 重验后沿用原 receipt/action/run/版本 7，Core 核销响应依次 replayed=false、true；released 再读不增加核销调用。总 Job=0、模型调用=0、临时工程清理完成。

该探针重新打开数据库来验证持久边界，未声称强杀操作系统进程或真实 GUI 重启。用于日志的教学 session/snapshot/Job 上下文是 synthetic，不作为真实工程或教师材料证据。外部核销由真实 Core 执行，故障是运行器主动丢弃已成功响应。

最终全量数字、源码摘要、独立审查及开发评测见[验收汇总](verification-stage20.json)；[开发评测](evaluation-stage20.json)仍为 40 项自动 probe，[交接报告](evaluation-stage20-handoff.json)继续标记冻结集缺失和教师签收未完成。

## 再规划

下一阶段优先完成生产宿主的元数据发布与显式恢复动作接线，确保教学动作事务、旧动作重放和取消同样保持材料隔离；仍须使用可信服务端材料供应，默认禁止 L4。随后分别验收真实审阅材料、GUI 双确认、团队冻结 ModelCase/教师评分、动态激励和其余真实 detector。不得将此内部协议的 released 状态解释为已经开放生产参考答案。

最终全量运行 586 项：553 成功、33 个单项跳过，另有 2 个类级跳过覆盖 18 个未运行方法；共收集 604 个方法，跳过记录 35，失败/错误均为 0，耗时 102.972 秒。独立源代码/规格审查通过，两处 P2 均修复复核；审查者独立复现了失效记录覆盖修复前后的行为，没有重复执行全量或真实 Core。开发评测 40/40 通过，摘要与当前代码匹配。
