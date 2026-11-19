# U2 阶段 20：持久参考发布协议实施计划

> **For agentic workers:** Use subagent-driven-development for the isolated journal implementation and independent source review. Preserve the user's working checkout and all stashes; do not commit, push, or delete plan workspaces during this stage.

日期：2026-10-17。基线为已推送的 a8431c5。本阶段接续阶段 19 已识别的核销/发布恢复缺口，沿用授权的分阶段规划、实现、测试与开发文档流程。

**Goal:** 在 Core 已支持同 run/action/version 幂等核销的条件下，提供可持久恢复的参考发布协议，不因未知核销结果产生新授权，也不恢复未经重新取证的教学发布。

**Architecture:** 新增 ReferenceJournal 复用 DurableStore 的同一 SQLite 连接、事务、实例和单进程锁；仅保存 challenge、材料摘要、作用域、版本和恢复所必需的 receipt 标识。DurableReferenceGate 在外部核销前持久化 consuming，取得可信核销成功后持久化 released，再返回重新检索并核验的正文。生产教学 HTTP L4 继续关闭，缺审阅材料不得生成替代答案。

**Tech Stack:** Python >=3.11；仅标准库 sqlite3、hashlib、json；现有 GatewayReceiptClient 与 ReviewedReference。

## Global Constraints

- 新 Python 服务、测试和文档位于 sigflowAgent；保留 agent 和既有 stash。
- 不删除无关文件或沙箱外文件；测试只清理自行创建并持有的临时目录。
- 不将材料正文、原始源码、问题、模型输出或 API key 写入新增 journal、检查点或机器报告。receipt_id 只保存在私有 journal 中用于同操作恢复，不发布到状态/报告。
- learner/session/project/revision/snapshot/Job/issue/instance/run、材料摘要、审批 action/version 必须绑定；改变作用域或材料时不发布。
- 接口不签发 receipt；无自动重试执行、新授权、EDA Job 或模型调用。重新协调必须由可信宿主显式调用。
- 核销未知时保持 consuming，不能以新请求替换；取消和终态禁止后续核销/正文发布。
- Core 的 consumed 重放可跨 TTL；本地尚未确认核销的 active receipt 仍必须在 challenge 有效期内。
- 旧进程的内存授权不继承；DurableStore 重启隔离与重新取证门禁保持。
- 本阶段不启用生产 HTTP L4，不改教师审阅状态，不声明 GUI、动态仿真或团队 Gate 已验收。

## Task 1：复用宿主事务的 ReferenceJournal

**Files:** 新增 host/reference_journal.py、tests/test_reference_journal.py。

**Interfaces:** `ReferenceJournal(store)`；`prepare(binding, run_id, challenge)`、`get(binding, run_id)`、`claim(binding, run_id, challenge_id, receipt_id)`、`release(binding, run_id, challenge_id)`、`invalidate(binding, run_id)`。返回记录为独立 dict，含 `challenge`、`phase`、`receipt_id`；所有作用域来自 SessionBinding，禁止传入调用方 JSON 作用域替换。prepare 拒绝 consuming，以及已 invalid 但仍携带 receipt 的记录；后者保留审计材料，不能用新请求覆盖不确定操作。claim 首次 ready→consuming，同 receipt 幂等，换 receipt 拒绝；release 仅 consuming→released，重放 released 幂等。每 session 一条有界记录，绑定现有 sessions 外键。

- [x] 写入作用域、状态机、原子性、重新打开、取消/终态、正文不落盘和串行竞争失败回归。
- [x] 实现 journal 并验证失败后不留下部分状态。
- [x] 独立源代码审查，定向测试通过。

## Task 2：可恢复发布 gate

**Files:** 新增 host/durable_reference.py、tests/test_durable_reference.py；复用 host/reference.py、clients/receipts.py，不重复现有进程内 gate。

**Interfaces:** `DurableReferenceGate(binding, run_id, provider, journal)`；`request(state_version)` 返回无正文 challenge；`confirm(receipt_id, challenge_id, state_version, port)` 首次确认；`reconcile(state_version, port)` 为显式恢复；`invalidate()` 取消。恢复使用私有 journal 原 receipt/run/action/version，不能由新 payload 更换。每次正文发布都重新核验 provider 的 ReviewedReference 和摘要；已 released 的重放只重新获取相同材料，不再核销。

- [x] 写失败回归：核销响应丢失、核销后本地提交失败、发布后响应丢失、重新打开、同操作幂等、换 receipt/run/learner/revision 拒绝、材料修改、TTL 和取消门禁。
- [x] 最小实现并定向回归；已有进程内 ReferenceGate 和主循环默认拒绝继续兼容。
- [x] 扩展真实 Core 协议探针，只使用标明 synthetic 的材料，证明未知结果恢复到同一核销，0 Job/0 模型。
- [x] 独立审查和全量/开发回归；产出阶段文档及摘要，更新 task.md 再规划。新开发不自动推送。

生产宿主的原子状态发布接线和审阅材料供应仍需单独阶段验收；本阶段先完成可独立验证的持久协议，不把协议成功写成真实答案发布或 GUI 签收。
