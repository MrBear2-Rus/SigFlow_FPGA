# 阶段 7：计划审批材料

`sigflow_edu_agent.contracts.build_plan_card(plan, plan_id, state_version)` 将
`DetailedPlan` 映射成冻结的 `ApprovalPackage`。`.card` 与 `.binding` 每次返回新的
深层字典副本，调用方修改展示内容不会修改已绑定材料或 `.digest`。不提交 Job，不生成 grant。

## 映射与限制

边界使用 `PlanChecker` 重新验证完整计划，仍限定最多 3 个 Job，不因为共享 Schema
允许 16 个而扩大本地权限。每步预算为 1，计划 `max_jobs` 原样保留（可以大于当前步骤数）。
`plan_id` 必须完整匹配 `plan-[1-9][0-9]*`；`state_version` 必须是非负 Python int，
拒绝 bool、浮点数与字符串，直接转成十进制字符串，避免经过浮点数造成大整数失真。

原步骤按顺序映射到 s1…sN，依赖及 `build_step_id` 同步映射，`top` 改为 `top_module`。
描述包含计划目标和本步具体操作。`eda.sim.run.timeout_ms` 没有可靠的 tick 换算，
遇到它会抛出 `ValueError('UNSUPPORTED_MAPPING: ...')`，不能静默丢弃。
不带 timeout 的运行计划可以保留 build 步骤引用。

已读取实际 `contracts/edu-agent/v1/schemas/plan-card.schema.json`：顶层与步骤对象允许
additionalProperties，因此 card 顶层增加 project_id、snapshot_id、expected_observations，
步骤增加 provider。这些是本地审批语义必需扩展；消费端必须展示并保留它们，不能假设所有共享
客户端会理解扩展。params 仍严格使用共享白名单。完整原始计划（含原始步骤 ID、provider、
项目/revision/snapshot、观察目标、停止条件、完整预算）同时保存在 binding.plan 中。

## 摘要契约

binding 包含 plan_id、十进制字符串 state_version、internal_fingerprint、plan。
internal_fingerprint 是现有 DetailedPlan 的指纹，独立于新审批材料摘要。

`.digest` 是 UTF-8 编码的以下对象规范化 JSON 的 SHA-256 十六进制摘要：
`{"format":"sigflow-local-approval-v1","card":...,"binding":...}`。
规范化使用 sort_keys=True、ensure_ascii=False、separators=(',', ':')、allow_nan=False；
数组顺序保留。字典插入顺序不影响摘要；原始步骤 ID、外部 plan_id、状态版本与所有绑定语义
都会影响摘要。冻结包持有 JSON 字符串，不持有可变输入引用。

该算法仅是本地交接完整性契约，不是已约定的 Gateway plan_hash，也不是签名或执行授权。
已读取 sigflow.openapi.yaml 的 CreateJobRequest：其参数与 PlanCard 不同，例如 run 使用
build_job_id，build 的参数白名单也不同。因此卡片不能直接作为 Job 请求，更不能据此声明
gateway_ready。真实执行仍需要另外实现能力映射、快照与授权绑定。

## 验证记录

- 新增测试先运行，因 contracts 模块尚不存在而失败；实现后 8 项专用测试全部通过。
- 覆盖字面完整 card/binding、嵌套防变更、摘要顺序稳定和语义变化、ID/版本非法值、
  大于 2^53 的整数、边界重新验证、timeout 拒绝及 synth 映射。
- 使用环境已有 jsonschema 对实际共享 PlanCard Schema 验证包含三种能力的卡片，通过；
  jsonschema 仅用于这次人工验证，核心及单元测试未引入第三方依赖。
- 阶段 8 集成完成后全量复核：核心 134 项通过、6 项可选集成跳过；
  配置真实 UCAgent 源码后 140 项全部通过。详见 [阶段 8](stage-8-gateway-report.md)。
- 没有进行真实 C++ Gateway/EDA 执行联调，没有修改共享 Schema。

## 再规划

审批展示材料已形成，但共享客户端不一定消费新增扩展字段。U1/S1/S3 应核对
provider/snapshot/expected_observations 的展示与完整绑定，再确定双方统一的 plan_hash。
先继续只读报告接入，不把本地摘要或卡片作为执行授权。
