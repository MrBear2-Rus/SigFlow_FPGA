# 阶段 14：只读教学会话检查点

日期：2026-10-14。对应 UA-05 状态交接、UA-06 动作重复防护。
实现位于 `sigflow_edu_agent/runtime/checkpoint.py`，不依赖 UCAgent 或第三方库。

## 接口与生命周期

`TeachingSession.checkpoint(binding: SessionBinding) -> str` 在会话锁内生成
`readonly-checkpoint.v1` JSON。仅支持 explain/diagnose 已发布卡片后的
WaitingStudent / Completed；有 plan、grant、验证结果，或处于 Running、失败、
取消、缺证据、未恢复状态时拒绝。已绑定的 Gateway/恢复会话不能替换 learner
或其他 binding 字段。普通 TeachingSession 的初始归属必须由宿主认证后提供。

`restore_readonly_session(payload, *, binding, request, source, provider=None)`
不进行网络访问，返回 `RecoveredTeachingSession`。binding 与请求摘要必须相同。
默认 Provider 为规则路径；使用 DeepSeek 时由宿主重新注入配置好的 Provider，
不从持久化数据读取 Key 或重新创建网络地址。

恢复状态为 RecoveryRequired，版本递增，旧卡片和 occurrence 暂不暴露。
advance 和教学动作均拒绝，宿主可显式 cancel 结束且不调用任何外部端口；
宿主调用 `reconcile(expected_version)` 才重新取证。
证据须完整、项目/revision/Job 一致、snapshot 锚点有效，且整个规范化证据摘要
与检查点相同。tool/rule 证据必须具备锚点；无锚点仅允许明确 fixture。
这只能验证宿主提供的来源与既有记录一致，不能认证工具输出真实性。

取证成功后重新检查每张 L1–L3 卡片的结构与引用，再恢复原暂停状态。
不重新生成卡片、不调用模型、不提交 Job；版本再次递增，旧 UI 版本失效。
check_phase 不继承恢复前的成功观察。取证失败保留 RecoveryRequired，卡片保持
隔离，错误为固定代码；允许宿主在问题修复后再次显式 reconcile。
恢复后的每次 hint_next 仍重新取证，固定报告改变时返回 stale_report 并暂停。

原已接受 hint_next/refresh_evidence/finish 的 action_id 与规范化参数保存，
首次取证失败后的刷新同样支持，动作记录数受阶段预算与总字节数限制。
重放同动作返回当前状态，参数改变报
action_conflict；新动作必须用新 state_version。提示等级最多 L3，预算不重置。
occurrences 对同一绑定只返回一个记录；不负责跨 revision 的 issue 合并。
历史 trace 不序列化，恢复后的 trace 仅记录新的检查，宿主持久化完整历史。

## 宿主接入示例

下列变量由宿主认证和装配；该接口不注册为模型工具。`session` 为已暂停的
只读会话，`binding` 是其不可变归属，`request` 是同一原始请求。

```python
from sigflow_edu_agent.context.gateway_report import GatewayReportSource
from sigflow_edu_agent.clients.gateway import GatewayClient
from sigflow_edu_agent.runtime.checkpoint import restore_readonly_session

payload = session.checkpoint(binding)
# 宿主在同一事务中保存 payload 与已提交动作，再向 UI 发布状态。

source = GatewayReportSource(
    GatewayClient(gateway_config),
    job_id=binding.job_id, project_id=binding.project_id,
    revision=binding.revision, snapshot_id=binding.snapshot_id,
    diagnostic_id=binding.diagnostic_id, anchors=trusted_anchors,
    origin="tool", combinational_goal=True,
)
recovered = restore_readonly_session(
    payload, binding=binding, request=request, source=source,
    provider=host_provider,
)
state = recovered.reconcile(recovered.state.version)
# 仅在 state.status 已恢复且 state.cards 可用时发布卡片。
```

原会话如使用 context_items、不同 combinational_goal 或 origin，必须以相同
设置重建证据源；不一致会使摘要不匹配。锚点由宿主可信目录获取，不能由模型
提交。示例不会签发凭据，也不将 fixture 改标为 tool。

## 存储与边界

检查点最多 128 KiB，严格校验字段、UTF-8、整数类型、重复 JSON 键、版本、
等级/预算/动作计数、卡片来源和引用；未知 schema 不自动迁移。
只保存证据和请求摘要，不保存问题、证据/源码/课程正文、Provider、Key、token、
grant、receipt 或可执行计划。保留教学卡片正文，需按教学记录保护。
摘要不是签名；接口只面向可信宿主，不能接收学生直接提供的检查点。

本模块提供暂停边界的恢复格式及门禁，**不等于全服务崩溃恢复已完成**。
U1 仍需接入动作/检查点事务、模型调用前预算预留、存储完整性、保留与删除、
跨进程锁、处理中请求的恢复和异常取消。磁盘失败不能向 UI 宣称动作已提交。
暂停后未落盘的动作不能靠该格式实现 exactly-once；运行中的模型调用不能从
旧检查点自动重跑。Gateway 绑定归属仍由宿主认证及受限客户端保证。

L4 reference_request/reference_confirm 继续拒绝；真实执行恢复、grant/receipt
原子核销及 UI 双确认另行联合实现。没有修改 `agent/` 或共享 HTTP DTO。

## 验证与再规划

在 `sigflowAgent` 执行：

```powershell
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -p test_checkpoint.py -q
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
.\.venv\Scripts\python.exe -S -m sigflow_edu_agent.evaluation --suite tests/evaluation/development.json
```

定向 22 项通过，覆盖暂停恢复、重复 hint、L3/finish、预算保持、版本失效、
跨绑定拒绝、陈旧/过期/改变锚点、源异常脱敏、恶意 JSON/卡片、并发恢复、
恢复后报告变化、两类显式刷新、待核验取消，以及独立新 Python 进程恢复。
新进程验证通过 stdin 传递 fixture 检查点，没有模拟完整服务或数据库崩溃。

Windows 当前 Python 3.13.5 下，全量 219 项：213 项通过，6 项历史上游实验
跳过；40 项开发评测通过，报告见 [evaluation-stage14.json](evaluation-stage14.json)。
核心不需要激活 Conda 或安装 UCAgent。未调用真实 DeepSeek API、EDA 或教师评分。

独立审查发现首次取证失败后的 refresh_evidence 幂等记录被检查点拒绝，已扩展
动作种类、版本范围及有界数量，补两类刷新回归。增量复审通过，无剩余阻断问题。
文档链接和代码围栏检查通过；源码变更均在 sigflowAgent，没有提交或推送。

下一步由 U1 对接可信事务存储，并与 S1/S3 冻结完整 receipt 绑定与恢复语义。
UA-05/06 保留部分完成；UA-07 的真实波形完整性仍待 S2，UA-08 的冻结真实模型
批次与双教师独立评分仍待输入。本阶段不将 fixture 测试计为真实联合验收。
