# U1/U2 组合服务开发接入

本文适用于原生工作台调用 U1 组合服务的教学扩展 `u1.u2-teaching.v1`。调用与投影以[客户端](../../../main/agent/AgentApiClient.cpp)、[服务路由](../../../agent/src/sigflow_edu_agent/teaching_routes.py)和[接口测试](../../../agent/tests/test_teaching_http.py)为核对入口。其他协议见[接口索引](README.md)，当前验收范围见[状态页](../status.md)。

## 启动与身份

桌面端以 `--u2-teaching-if-available` 尝试启用扩展。源树联调可用 `sigflowAgent/bin/sigflow-shared-u2.cmd --bootstrap-stdin` 显式启用。启动器设置 U1 与 U2 的源码根后运行 `sigflow_edu_agent --u2-teaching`。核心包为 `sigflow-teaching-core`，启用 U2 需要 Python 3.11+ 和 SQLite。

启动器不生成 token。SigFlow 可信启动方通过 stdin 提供现有 bootstrap：type、protocol、instance_id、nonce、gateway_url、gateway_token、ui_token、data_directory。启用扩展要求 Gateway 为规范 IPv4 loopback 和绝对数据目录；数据写入 `<data_directory>/u1-u2`，按 instance 摘要隔离文件。仅在实际宿主生命周期保证属于同一逻辑实例时重用 instance_id 和 data_directory；不同实例不能冒用旧标识恢复会话。

就绪消息沿用 [Sidecar 启动协议](../../../docs/edu-agent/sigflow/sidecar-bootstrap.md)。从 `GET /api/v1/capabilities` 的 `data.teaching` 核对扩展版本、可用状态和 `base_path`。该对象同时报告模型、课程、学习记忆、预测及 HTTP L4 能力。教学请求通过 UI Bearer 身份访问，每次请求必须提供恰好一个由可信 UI 断言的 `X-Learner-ID`。Host 和 Origin 检查沿用基础服务。

## 路由与数据

所有成功响应外层沿用 `edu.api.v1` envelope。普通教学响应的 `data.schema_version` 为 `u1.u2-teaching.v1`，时间线及学习记录使用各自版本。写操作返回 202 时表示已受理，后续通过 GET 读取正式状态。

| 路由 | 请求与返回 |
| --- | --- |
| POST `/api/v1/u2/sessions` | body 为 project_id/revision；Idempotency-Key 必须；201 返回外部 session_id |
| GET `/api/v1/u2/sessions/{sid}` | 同 owner 范围元数据 |
| POST `/api/v1/u2/sessions/{sid}/runs` | question/job_id/issue_id 必须；可带 snapshot_id、diagnostic_id、intent、mode；Idempotency-Key 必须；202 返回 run_id |
| GET `/api/v1/u2/runs/{rid}` | phase/teaching_status/state_version/cards/run/omitted，及已提交预测/验证元数据、审批态 plan_card |
| POST `/api/v1/u2/runs/{rid}/actions` | `kind`、`action_id`、`expected_version`，按动作附加字段；202 接受元数据 |
| POST `/api/v1/u2/runs/{rid}/cancel` | body 为 `{}`；202，仅依赖 owner，不依赖 Gateway 当前版本可读 |
| GET `/api/v1/u2/runs/{rid}/events?after=0&limit=100` | 只含扩展事件元数据，sequence 与 high_watermark 使用十进制字符串 |
| GET `/api/v1/u2/runs/{rid}/timeline?limit=12` | `u2.teaching-timeline.v1`。省略 `after` 读取最近一页，指定时按序号续读。 |
| GET `/api/v1/u2/runs/{rid}/records` | `u2.learning-records.v1`，在当前保存修订下读取同一学习者的受限记录。 |

创建 run 默认 intent=diagnose、mode=rule、diagnostic_id=job-report；snapshot_id 可省略，由认证 report 确定。mode 可显式为 deepseek，使用 U2 原配置、预算和降级逻辑；有配置不代表真实 API 已通过本阶段质量验收。intent=verify 必须同时提供现有详细 plan 和 gateway_plan_hash，plan 工程/版本/快照须与可信 report 一致；其他 intent 不接受这两个字段。

将状态响应的 `state_version` 原样写入动作请求的 `expected_version`。两者均为十进制字符串。

```json
{"kind":"hint_next","action_id":"hint-1","expected_version":"5"}
```

允许 hint_next、finish、refresh_evidence、observe_verification；execute_plan 另带真实 grant_ref，recover 另带 question，prediction_request/prediction_submit 另带 prediction。波形 payload 与[预测协议](../history/stages/stage-18-prediction.md)一致。收到 challenge 前不提交预期；读取 observed 之前必须先显式提交独立预期。普通 HTTP reference/L4 动作全部拒绝。

同键同内容重放复用外部和内部身份；同键变更输入为 409。状态过期或投影碰到并发提交时 409，应重新 GET，再决定是否提交新的明确动作；不得自动重放结果未知的写请求。原始问题不保存到 U1 关联库，崩溃在创建与 attach 之间时，客户端用完全相同请求和键显式重试。`StartRequiresRetry` 表示预留关联尚未完成；恢复后的 RecoveryRequired 必须显式 recover。

每个工程由 U2 限制一个活动教学会话。WaitingStudent 仍占用该位置；完成当前交互时发送 finish，需要中止则 cancel，再开始下一 run。旧版本会话不可在新工程 revision 下继续；取消仍可用，新的教学会话须绑定新的已保存同步工程和新证据。事件游标过期返回 410，应重新读取当前状态后从最新高水位继续，不把事件当完整教学正文日志。

## 只读辅助数据

- 时间线按事件序号升序返回，`after` 使用最后一条事件的 `sequence`。游标失效返回 410，可重新读取最近一页。历史运行保留原修订的事实。见[时间线行为](../features/teaching-support/u2-teaching-timeline.md)。
- 学习记录可能返回 `pending`、`unavailable` 或 `ready`。切换运行时丢弃旧响应。字段见[学习记录 Schema](../../../contracts/edu-agent/v1/schemas/u2-learning-records.schema.json)。
- 运行状态的 `course_context` 是背景材料，卡片的 `course_refs` 是该卡实际引用。检查器的 `checks` 分开报告卡片证据、复测工具和功能正确性，后者为 `not_assessed`。见[辅助分区验收](../features/teaching-support/u2-support-sections-task.md)。

## 可信外部依赖注入

`sigflow_edu_agent.teaching_factory.compose(bootstrap, teaching_memory=..., reference_provider=..., receipt_port=...)` 是受信任的 Python 组合入口。默认尝试装配可用的 UA-10 适配器。设置 `SIGFLOW_U2_MEMORY_DISABLE=1` 或显式提供 `teaching_memory=None` 可关闭，以能力响应确认配置。普通 HTTP 请求无权选择工厂或本机路径。L4 的原生方案及放行条件见[功能专题](../features/l4-reference/README.md)。

关闭服务时执行有界工作者停止和资源清理；关闭异常仍尝试所有已登记资源。Runtime/验证器的等待都有上限，不能将服务 close 返回解释为所有外部 I/O 已优雅结束。进程级验收还检查真实子进程退出码与 stdout 协议。
