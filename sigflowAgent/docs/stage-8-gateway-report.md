# 阶段 8：只读 Gateway 报告与教学证据桥

日期：2026-10-11。实现范围为只读客户端和可信证据适配；未执行真实 C++ 服务或 EDA。

## 接口

`GatewayClient.job_report(job_id, *, project_id, revision, snapshot_id) -> dict`
只请求现有 `GET /api/v1/jobs/{jobId}/report`。每次查询先执行 health，核对配置的
instance_id、edu edition 与 edu.api.v1，然后使用 Bearer token 读取报告。
报告 data 自身不具有 health 的 protocol_version/instance_id 字段，不能混用响应模型。
报告信封仍必须具备 schema_version、request_id、trace_id。

Job、项目、revision、snapshot 均须为不超过 256 字符的非空 ASCII opaque ID；
允许字母、数字、下划线、连字符及内部点/冒号，拒绝路径分隔、查询、百分号转义、
控制字符与点段。输入无效时不发送请求。

只连接 127.0.0.1 的配置端口，不跟随重定向，不自动重试，不接受任意 URL 或 HTTP 方法。
响应最多 1 MiB；连接使用已有 大于 0 且不超过 30 秒的超时配置。
JSON 拒绝重复键、NaN/Infinity；错误只暴露白名单错误码，不转发服务器正文。

读取后必须匹配 Job/项目/revision/snapshot。版本不符为 STALE_REVISION，
Job/项目不符为 INVALID_RESPONSE。legacy 报告不能用于本链路。
客户端允许 Schema 的九种 Job 状态和 partial/unavailable，保留原始事实；
终态与证据完整性由证据层进一步判定。读取成功不等于证据可用。

## 证据桥

`GatewayReportSource(client, *, job_id, project_id, revision, snapshot_id,
diagnostic_id, anchors, combinational_goal=False, origin='tool', context_items=(),
max_bytes=8192, max_tokens=4096)` 实现既有 EvidenceSource.collect。

调用方显式选择诊断与可信 anchors，绑定期望项目/版本/快照。anchors 来自可信宿主
证据目录，不能从报告正文、模型输出或诊断位置自行拼造。构造时复制目录；
每次 collect 都重新取报告并核对全部绑定，hint_next 不重用缓存报告。
不同项目或版本调用在发出 HTTP 前拒绝。

`normalize_report_artifacts` 显式映射：

| Wire 字段 | 内部字段 | 处理 |
|---|---|---|
| artifact_id | id | 保留原值，重复 ID 拒绝 |
| sha256 | hash | 保留原值；64 位十六进制或 sha256: 前缀格式，缺失拒绝 |
| role | 无 | 可为字符串或 null，不用于推导证据 |
| schema | 无 | C++ 实现提供的可选字符串/null 元数据 |

本适配器拒绝路径字段、未知 artifact 字段、畸形哈希与畸形元数据。
这比共享 artifacts 通用 object 定义更严格；任一 artifact 不符合要求会使本次
取证失败，包括不被选中诊断引用的 artifact，不将其静默忽略。
SHA 文本按原样与可信 anchor 匹配，不自动补造或从本机文件读取。
仅有哈希不代表在 Python 中重新校验了文件内容，文件真实性仍由 Gateway 和宿主负责。

随后复用 ReportEvidenceSource 检查完整报告、confidence_kind、每条 evidence_ref、
artifact/hash、源码位置和上下文预算。未找到指定诊断、缺 anchor、过期、哈希不符、
非终态、不完整或旧版本均不生成新教学卡；空诊断列表不自动推断“设计正确”。
本地 HTTP fixture 必须显式传 origin='fixture'；真实服务才能使用默认 tool 来源。

## 手动只读对接

宿主需提供 Gateway 端口、Agent token、instance_id 和已经存在的 Job 对应的
project_id、revision、snapshot_id。下例不创建 Job，不调用 DeepSeek：

```python
import os
from sigflow_edu_agent.clients.gateway import GatewayClient, GatewayConfig

client = GatewayClient(GatewayConfig(
    port=int(os.environ['SIGFLOW_GATEWAY_PORT']),
    token=os.environ['SIGFLOW_GATEWAY_TOKEN'],
    instance_id=os.environ['SIGFLOW_GATEWAY_INSTANCE'],
))
report = client.job_report(
    os.environ['SIGFLOW_JOB_ID'],
    project_id=os.environ['SIGFLOW_PROJECT_ID'],
    revision=os.environ['SIGFLOW_REVISION'],
    snapshot_id=os.environ['SIGFLOW_SNAPSHOT_ID'],
)
print(report['job_id'], report['state'], report['completeness'])
```

代码从 sigflowAgent 目录、该目录虚拟环境运行。程序不自动加载 .env，
token 应经可信启动材料注入，不写入脚本或日志。证据目录尚未提供时只验证读取，
不能为了让主循环通过而手工编造 source_ref 或 evidence ID。

## 验证记录

先运行新增测试复现缺少报告接口和证据桥，再实现。按 C++ ReportNormalizer.cpp
补充 nullable schema/role 回归，先观察拒绝合法结构，再修复元数据兼容。

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p 'test_gateway*.py' -v
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_plan_contract.py -v
Remove-Item Env:SIGFLOW_UCAGENT_SOURCE -ErrorAction SilentlyContinue
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
$env:SIGFLOW_UCAGENT_SOURCE = (Resolve-Path '..\..\UCAgent\UCAgent').Path
.\.venv\Scripts\python.exe -m unittest discover -s tests -q
```

- Gateway 三组测试 24 项通过，计划映射 8 项通过。
- 核心隔离：发现 140 项，134 项通过、6 项可选集成跳过。
- 含真实 UCAgent 类集成：140 项全部通过，无跳过。
- 现有离线评测 40 项通过，结果归档 [evaluation-stage8.json](evaluation-stage8.json)。
- 阶段 6 的 evaluation-development.json 保留其历史源码摘要，不覆盖历史证据。
- 独立只读代码审查核对共享 Schema、C++ artifact 元数据与证据桥，未发现剩余需修改缺陷。
- 20 份 Markdown 的相对链接、围栏和尾部空白检查通过，归档评测源码摘要与当前实现一致。

本地 HTTP 测试验证真实网络传输代码及报告→证据→主循环路径，测试服务返回合成
fixture；不能据此声称真实 C++ Gateway、工具输出真实性或 EDU-01 闭环完成。

## 阶段再规划

U2 已可交接审批材料、只读报告接口和所需证据目录结构。
下一阶段需要 U1/S1 确认审批扩展字段与 Gateway plan_hash 的完整绑定规则，
S2 提供真实报告/锚点目录，再运行真实只读联调。
GrantQueryData 缺 snapshot_id、used_jobs、steps，active 状态不足以开放执行。
Job 提交、取消、授权持久化与恢复继续留在 U1/S1 联合边界；L4 仍默认拒绝。
