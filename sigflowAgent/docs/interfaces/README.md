# SigFlow Agent 接口索引

| 对接方 | 协议与入口 | 状态与文档 |
| --- | --- | --- |
| 原生工作台、U1 组合服务 | `u1.u2-teaching.v1`，`/api/v1/u2` | 按 `capabilities.teaching` 协商。见 [GUI 接入手册](u1-u2-teaching.md)。 |
| U2 独立宿主 | `u2.host.v1`，`/api/v1/teaching/...` | 调试和兼容环境。见[独立宿主 OpenAPI](../../../contracts/edu-agent/v1/u2.host.openapi.yaml)。 |
| IDE 与基础 Sidecar | 基础 `edu.api.v1`，启动握手与 Agent 会话 | 见 [Agent OpenAPI](../../../contracts/edu-agent/v1/agent.openapi.yaml)和[启动协议](../../../docs/edu-agent/sigflow/sidecar-bootstrap.md)。 |
| U2 与 EDA Gateway | `edu.api.v1`，工程状态、Job 报告、授权与产物 | 见 [Gateway OpenAPI](../../../contracts/edu-agent/v1/sigflow.openapi.yaml)。 |
| U3 与 UA-10 | 受信任 Python 组合、事实投递和历史查询 | 见 [UA-10 接入说明](../../ua10_module/docs/U2_INTEGRATION.md)和[辅助分区的当前接入](../features/teaching-support/u2-support-sections-task.md#配置与数据范围)。 |
| 课程提供器 | Python `CourseProvider` 接入教学流程 | 见 [RAG v0.1 U2 联调](../features/course-rag/rag-v0.1-u2-integration.md)。 |
| S3、U1、U2 的 L4 调用 | `u2.native-reference.v1` | 试点草案，见 [L4 接口契约](../features/l4-reference/contract.md)。 |

接口变更随实现更新接入手册。共享版本的机器可读定义、契约样例和兼容性记录维护在仓库的 [契约目录](../../../contracts/edu-agent/v1/)。
