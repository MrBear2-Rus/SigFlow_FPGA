# Part 3：Gateway、报告与上下文

## HTTP 边界

`clients/gateway.py` 仅实现已冻结的 `GET /api/v1/health` 和 `GET /api/v1/capabilities`，对应共享协议 `edu.api.v1`。连接固定到 127.0.0.1；端口、token、预期实例由可信宿主配置。health 不附 token，capabilities 附 Bearer token。成功信封必须匹配协议、预期 instance_id 和 edu edition。

客户端不跟随重定向、不自动重试。响应最大 1 MiB，默认 socket timeout 为 3 秒，允许配置不超过 30 秒；该超时不是整次调用的墙钟 deadline。错误只返回固定错误码，不转发服务端 message/body。`GatewayConfig` 的 repr 隐藏 token。

`capabilities().available_ids` 仅返回 ready 且未 disabled 的 eda.synth / eda.sim.build / eda.sim.run。未知能力不进入可用集合；可用性不等于执行授权。

只读探测入口读取 `SIGFLOW_GATEWAY_PORT`、`SIGFLOW_GATEWAY_TOKEN`、`SIGFLOW_GATEWAY_INSTANCE` 三个环境变量，由宿主注入后运行：

```powershell
.\.venv\Scripts\python.exe -m sigflow_edu_agent.clients.probe
```

输出仅包含实例和可用能力。本客户端尚未实现报告拉取、Job 创建、取消、grant 或 receipt；报告输入通过以下独立 Python 适配接口提供。SigFlow `b7e5186` 已扩展共享 OpenAPI，客户端覆盖范围不能与服务端契约范围混为一谈。后续 HTTP 适配应以仓库契约和实际服务验收为准。

## 报告规范化

`ReportEvidenceSource` 消费一个指定 JobReportView、受信 EvidenceAnchor 目录以及指定 diagnostic_id。构造时复制输入，避免调用方后续修改同一对象影响证据快照。接收 `edu.jobreport.v1` 的完成态报告，Succeeded 和 Failed 都可携带有效诊断；工具失败不意味着诊断无效。

放行条件包括：

- project_id、revision、job_id 与请求绑定一致。
- completeness 为 complete，snapshot_id 和 input_fingerprint 非空；legacy_unverified 不放行。
- 指定诊断唯一存在，confidence_kind 为 tool 或 rule；hypothesis 不能作为事实发布。
- 每个 evidence_ref 都能解析到可信目录，未过期，绑定同一工程、revision、Job 和 snapshot。
- 产物 id/hash 与规范化报告的产物条目一致；SourceRef 如存在则核对工程、revision、非空 hash 和合法行列范围。

EvidenceAnchor / SourceRef 为内部对象，并非共享协议新增字段。报告的 artifacts `{id, hash}` 结构是此适配器的内部规范化约定，当前共享 Schema 仅给出泛型对象。S1/S2 接入时应显式完成映射，不能要求其他组件未经契约评审直接发送此格式。

Hash 校验是受信目录与报告间的一致性比较，未读取产物重算 hash，也未证明目录真实性。SourceRef 校验不读取源码确认行号存在。宿主负责可信来源、产物内容验真与即时有效性；快照目录的 expired 标志不会自动更新。当前保留第一个引用作为发布卡片的主引用，所有引用仍须通过校验。

```python
from sigflow_edu_agent.context.report import ReportEvidenceSource
from sigflow_edu_agent.domain import RunRequest
from sigflow_edu_agent.loop import TeachingLoop

source = ReportEvidenceSource(
    report, anchors, job_id=job_id, diagnostic_id=diagnostic_id,
    combinational_goal=True, origin="tool", context_items=fragments,
)
loop = TeachingLoop(RunRequest(project_id, revision, issue_id, question), source, provider)
state = loop.run_until_pause()
```

示例变量由宿主提供，anchors 为 `EvidenceAnchor` 序列，fragments 为 `ContextItem` 序列；测试示例见 [test_context.py](../tests/test_context.py)。默认 origin 为 fixture，只有已验真的工具输入才设置 tool。`origin` 区分证据供应环境，`confidence_kind` 表示诊断依据类型，两者不能互相替代。`combinational_goal` 必须来自课程/工程约束，不能只因诊断包含 latch 就认定锁存器错误。

报告错误转为 `WaitingEvidence`，reason 保留稳定码，例如 stale_report、incomplete_report、hypothesis_not_fact、unresolved_evidence、expired_evidence、mixed_context；不会发布卡片或自动启动工具。刷新需要宿主提供新的证据源/新 run，重试同一快照不会获取远端更新。

## 上下文预算

`build_context` 接收最多 64 个片段，按 selection → report → rtl → wave → course 保留，同优先级保持输入顺序。片段必须绑定同一工程和 revision，ID 唯一且文本为有效 UTF-8。超预算整块省略，不截断代码或 Unicode 字符；输出 omitted 项与 byte_budget / token_budget 原因。

默认文本预算 8192 bytes、4096 estimated tokens，估算按 UTF-8 字节数计数，不是 DeepSeek tokenizer 实测值。预算仅统计 text，不包含标识符、JSON 和系统提示；DeepSeek 适配器再执行完整请求 16 KiB 限制。领域证据检查要求保留片段总量不超过 8192 bytes；定制预算也应服从该边界。

Evidence 携带 anchor、confidence_kind、context_items 和 omitted，经 CardContext 传给 Provider。片段属于数据，不获得工具权限；确定性结构检查不构成针对提示注入或答案泄露的完整语义保障。

## 验证

测试读取仓库 [golden.json](../../contracts/edu-agent/v1/fixtures/golden.json)，组合规范化报告样例，并启动本地 ThreadingHTTPServer 检验真实 HTTP 请求、鉴权头、信封、实例隔离、禁用能力和错误脱敏。该服务为测试 fixture，不是实际 C++ Gateway 进程。真实 Gateway 和可信报告目录联调仍需单独验收。
