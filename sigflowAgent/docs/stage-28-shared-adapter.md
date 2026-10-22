# 阶段 28：正式共享宿主的 U2 组合适配

日期：2026-10-21。入口为 `sigflow_u2.host.shared_adapter`，配合[独立安装名称](stage-28-namespace.md)使用。本组件不注册 HTTP 路由、不增加数据库或调度器；U1 负责认证、外部 session/run 的生命周期、外部标识与 TeachingHandle 的持久关联及共享协议发布。

## 可信调用契约

`SharedRunContext` 包含 session_id、run_id、instance_id、learner_id、project_id、revision、snapshot_id、job_id、diagnostic_id、issue_id。外部 session/run 使用现有 `sess-正整数` / `run-正整数` 形式。它们必须来自宿主实际认证、工程与 Job 上下文，不能直接接受模型或普通请求提供的可信身份。

`SharedTeachingAdapter(runtime, resolve_evidence, secrets=())` 的 resolver 接收 evidence_id，返回已经授权的完整 `Evidence`，不能只返回 `EvidenceAnchor`。适配器将完整 Evidence 的规范化摘要与现有已提交 checkpoint 内的 baseline/results 摘要比较，再核对 evidence/project/revision/snapshot/job/artifact/hash。只接受基线和明确记录的验证结果 Job；同一 evidence_id 下更换报告正文或 artifact 也会拒绝。网关允许的裸 64 位十六进制摘要和 `sha256:` 前缀形式均保留原值，不在计算完整 Evidence 摘要之前改写。

| 调用 | 行为 |
| --- | --- |
| `start(context, question, idempotency_key=..., mode='rule', intent='diagnose', plan=None, gateway_plan_hash=None)` | 委托 Runtime.create；稳定幂等键包括外部上下文，重放复用原内部会话；同键改问题拒绝 |
| `project(handle)` | 返回 `edu.u2.shared-projection.v1` 内部组合结果，字段包括 phase、teaching_status、state_version、cards、run、omitted，审批态可附 plan_card |
| `action(handle, payload)` | 校验明确动作、精确字段、十进制字符串 expected_state_version 后委托现有 Runtime |
| `cancel(handle)` | 委托现有取消，不执行 Job |
| `events(handle, after=0, limit=100)` | 返回既有内部事件，尚不是冻结共享事件协议的映射 |

允许动作是 hint_next、finish、refresh_evidence、observe_verification、execute_plan、recover、prediction_request、prediction_submit。execute_plan 仍须真实 grant_ref，recover 需 question，预测动作需 prediction；无隐式 L4、客户端 level 或 receipt 注入。动作与取消的返回仅为 accepted/session_id/run_id/replay/pending，不转发 Runtime 重放中的私有 result；卡片始终经 project 的输出边界发布。

## 共享投影与状态边界

卡片只取已检查的 L1–L3，最多最后 16 张，card_id 使用原序号，遗漏显式记录。证据映射为真实 job_report ref，fixture 标记仍保留；模型/规则来源使用 teaching 卡的 source，不伪造 rule_match 或诊断。课程共享 course_id 使用已准入 passage_id，chapter 使用 section，title/url 来自已核对的课程元数据；额外保留 passage_id/index_version。此为当前契约的字段映射，不宣称另有课程主键服务。

现有共享 agent-run 没有教学等待态。WaitingStudent、WaitingApproval、RecoveryRequired 等返回 pending、run=null；等待时可另外返回卡片。仅 Completed 且有卡片时产生共享 run：含模型准入卡片为 succeeded，否则 degraded。失败、超预算、取消不伪造成功终态。WaitingApproval 的 plan_card 必须匹配当前版本与工程范围且 requires_approval=true，公开审批材料不构成授权。

输出拒绝显式提供的秘密及工厂已知模型秘密、本机路径和不合规课程 URL。resolver 外部调用之后重新读取完整状态；发生取消、恢复或版本推进时拒绝陈旧发布。可信宿主仍须完成认证和传输层敏感数据控制，内部 events 不可未经公共事件映射直接暴露。

## 验证与再规划

14 项真实 Runtime/SQLite 测试覆盖解释终态、教学等待/显式升级、创建和动作重放、跨范围、取消围栏、恢复隔离、真实证据核验、审批拒绝、后继 Job、私有重放隔离、报告/artifact 替换拒绝及网关摘要前缀。卡片、计划和终态 run 使用仓库 U1 的实际 ContractSchemaValidator 和冻结 schema 验证。

独立审查发现的后继 Job 范围、完整 Evidence 固定以及摘要前缀兼容问题已补回归；前缀测试先失败再修复并由审查者复验。U1/U2 同进程双向导入和离线 wheel 验证单独记录。下一步由 U1 注册本组件、持久化 TeachingHandle、冻结教学等待/动作/事件扩展并由 S3 对接 UI；本阶段未修改 agent、共享 schema、Core 或 demo，不宣称原生主界面已经组合该引擎。
