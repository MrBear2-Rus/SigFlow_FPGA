# U2 阶段 21：参考元数据宿主接线实施计划

> 执行方式：采用 subagent-driven-development 的独立任务实施与复审流程。沿用当前工作区；本阶段不暂存、不提交、不推送。

**目标：** 将阶段 20 的持久协议接入常驻教学宿主的内部动作、提交和显式恢复流程，保持公开 HTTP L4 关闭。

**架构：** Runtime 仅通过显式传入的可信材料提供方和 receipt 端口启用内部动作。请求 challenge、状态、检查点、动作结果和事件在同一 SQLite 事务提交。确认和 reconcile 允许外部操作跨越本地事务，但其未知结果保留原 receipt；只有提交后的元数据可发布。正文不进入宿主状态、检查点、动作结果或事件。

**技术栈：** Python 3.13、标准库 threading/sqlite3/unittest，已有 Core receipt 协议，无新增依赖。

## 设计与约束

- 采用内部元数据动作接线；直接开放 HTTP 正文或将正文写入 L4 卡片会扩大材料和 UI 签收范围，本阶段不采用。维持进程内 gate 无法完成宿主恢复，故继续复用宿主数据库。
- SessionBinding 的 `l1-l3-l4-denied` 策略和公开 capabilities 保持。内部元数据发布不等于授予 L4 正文能力。
- 新动作 `reference_request`、`reference_confirm`、`reference_reconcile` 仅允许已重新取证的只读 WaitingStudent/L3 会话。使用现有 steps 上限，0 新模型调用、0 新 EDA Job、不增加提示等级或卡片。
- 内部请求 payload 为 `{}`；确认为 `{challenge_id, receipt_id}`；恢复为 `{}`。receipt 只存入私有 reference_records；幂等 fingerprint 仅保存 payload 摘要。
- challenge.state_version 必须等于请求提交后的宿主版本。请求提交失败必须回滚 challenge、状态、动作结果与事件。
- 确认遇到超时后发布固定 pending/unavailable 元数据，继续保留教学会话。只能显式 reconcile 原操作，禁止自动重试、换 receipt 或重新签发。
- 重启先进入 RecoveryRequired；普通 recover 重新取证不查询或消费 receipt。旧 ready challenge 不能直接确认；consuming/released 记录需显式 reconcile。
- 取消/终态/上下文改变立即禁止发布，保留未知操作审计。旧动作重放不能重新暴露已失效 challenge 或恢复前元数据。
- HTTP 对三个参考动作均继续拒绝，即使进程内部配置了测试端口。不得接受请求注入材料、reviewer、路径、来源或端口。
- 仅清理测试自行创建且持有的临时目录，不删除无关或沙箱外文件。禁止真实 API 调用。

## 任务 1：原子请求提交

文件：`host/store.py`、`host/reference_journal.py`、`tests/test_reference_journal.py`。

接口：将现有 commit 的实现抽为 `_commit(db, session_id, action_id, state, checkpoint, result)`，只供同库事务复用；公开 commit 行为不变。提供 `ReferenceJournal.commit_request(binding, run_id, challenge, action_id, state, checkpoint)`；使用 `_commit` 后在同一事务执行 prepare 的校验和写入，result 固定为 state。

- [x] 先增加请求提交版本一致、失败全部回滚、取消/CAS/未知操作阻止覆盖的测试，并观察失败。
- [x] 实施最小事务复用及 journal 接口，运行 journal/store 回归。
- [x] 独立复核事务范围及旧 API 兼容性。

## 任务 2：内部宿主动作及恢复

文件：`host/durable_reference.py`、新增 `host/reference_actions.py`、`host/runtime.py`、`host/http.py`，新增 `tests/test_host_reference.py`，扩展 gate 测试。

接口：Runtime 新增成对可信参数 `reference_provider` / `receipt_port`，默认均为 None。action 新增内部 `reference` payload。gate 提供只生成下一个提交版本 challenge 的 draft；metadata_only 模式返回固定元数据且不返回正文。reference_actions 负责 payload、预算、上下文预检、固定错误投影和版本步数推进。

- [x] 先写真实 SQLite/真实教学核心的请求、确认、重放、丢失回执后重启恢复测试，确认旧入口拒绝导致失败。
- [x] 接线原子 request 提交、metadata-only confirm/reconcile、当前状态投影及旧动作失效隔离。
- [x] 测试取消竞态、材料变化、错误 payload、learner 隔离、步数预算和 HTTP 默认拒绝；运行宿主定向回归。
- [x] 独立审查，复现并修复实质问题。

审查修复：禁止在 Runtime 全局锁内等待外部操作持有的 session 锁。新增 DurableStore.has_pending 的只读快速准入检查，保留 claim 的最终幂等/CAS 判定；取消直接使用持久绑定和 run 失效日志，不读取 session.state。Event 回归先失败后通过，独立复审另以第二次 confirm 复现验证通过。

## 任务 3：验证与交接

文件：新增 `docs/stage-21-reference-host.md`、`docs/verification-stage21.json`，更新 `README.md`、`docs/task.md`、`docs/u2-remaining-development-plan.md`。

验证入口：新增 `host/reference_host_acceptance.py` 和 `tests/test_reference_host_acceptance.py`，receipt_acceptance 增加显式 `--host-metadata`。真实 Core 路由、真实宿主动作和数据库重开完成联合核验，教学证据、正文和 reviewer 均明确为 synthetic/fixture；不作为真实教学材料或 GUI 签收。

- [x] 运行新服务全量测试、已有开发评测；记录跳过范围、源码摘要与真实模型调用 0。
- [x] 检查文档链接、开发口吻、结果计数及不含正文/receipt/私密数据的报告。
- [x] 更新阶段结论和下一阶段依赖；确认新增改动保持本地。

## 验收边界与再规划

本阶段验收内部动作和元数据持久边界，不替代真实受审材料、两位教师身份认证、GUI 双确认或正文呈现验收。后续在上述材料和 UI 契约齐备后单独设计正文访问与撤销，不通过现有 action replay 投递正文。
