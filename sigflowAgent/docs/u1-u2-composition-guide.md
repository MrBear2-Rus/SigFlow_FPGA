# U1/U2 组合服务开发接入

更新日期：2026-10-22。实现见[持久关联](stage-32-u1-bindings.md)、[宿主组合](stage-33-u1-composition.md)与[真实验证](stage-34-formal-u1-acceptance.md)。运行时仅标准库；U1 基础模式保持 Python >=3.9，启用 U2 需要 Python >=3.11 和 SQLite。无需 UCAgent 或激活 Conda。

阶段 35 基于 deb6bd9 适配。当前团队基础发布与[B 阶段范围登记](../../docs/edu-agent/ucagent/b-stage-scope.md)保持一致：默认不开启该扩展，基础 v1 不包含持久教学工作流。本文描述本地显式启用的 `u1.u2-teaching.v1` 组合，尚待双方接口/UI/目标平台接收，不能据本地测试宣称 B 阶段已联合冻结或产品验收通过。当前实测与剩余输入见[阶段 35](stage-35-current-adaptation.md)。

## 启动与身份

源树启动器为 `sigflowAgent/bin/sigflow-shared-u2.cmd --bootstrap-stdin`。它使用本目录 venv，在进程内依次设置 U1 和 U2 源码根，再运行 `sigflow_edu_agent --u2-teaching`。正式安装分别安装 U1 以及 `sigflow-u2-teaching`，然后运行同一主程序开关；U2 导入名称固定为 sigflow_u2。

启动器不生成 token。SigFlow 可信启动方通过 stdin 提供现有 bootstrap：type、protocol、instance_id、nonce、gateway_url、gateway_token、ui_token、data_directory。启用扩展要求 Gateway 为规范 IPv4 loopback 和绝对数据目录；数据写入 `<data_directory>/u1-u2`，按 instance 摘要隔离文件。仅在实际宿主生命周期保证属于同一逻辑实例时重用 instance_id 和 data_directory；不同实例不能冒用旧标识恢复会话。

ready 沿用 U1 的单行协议；`GET /api/v1/capabilities` 返回 teaching 字段声明扩展 schema、base_path、模型/课程/预测配置状态及 HTTP L4 关闭状态。UI token 持有者通过 Bearer 访问，每个教学请求还必须有且仅有一个 X-Learner-ID。该 learner 值由可信 UI 断言，不是独立学生登录认证。Host/Origin 检查沿用 U1。

## 路由与数据

所有成功响应外层沿用 edu.api.v1 envelope，data.schema_version 为 u1.u2-teaching.v1。教学等待是 pending，run=null；客户端应轮询 GET 或读取元数据事件，不把 202 接受回执当作完成。

| 路由 | 请求与返回 |
| --- | --- |
| POST `/api/v1/u2/sessions` | body 为 project_id/revision；Idempotency-Key 必须；201 返回外部 session_id |
| GET `/api/v1/u2/sessions/{sid}` | 同 owner 范围元数据 |
| POST `/api/v1/u2/sessions/{sid}/runs` | question/job_id/issue_id 必须；可带 snapshot_id、diagnostic_id、intent、mode；Idempotency-Key 必须；202 返回 run_id |
| GET `/api/v1/u2/runs/{rid}` | phase/teaching_status/state_version/cards/run/omitted，及已提交预测/验证元数据、审批态 plan_card |
| POST `/api/v1/u2/runs/{rid}/actions` | kind/action_id/expected_state_version，按动作精确附加字段；202 接受元数据 |
| POST `/api/v1/u2/runs/{rid}/cancel` | body 为 `{}`；202，仅依赖 owner，不依赖 Gateway 当前版本可读 |
| GET `/api/v1/u2/runs/{rid}/events?after=0&limit=100` | 只含扩展事件元数据，sequence 与 high_watermark 使用十进制字符串 |

创建 run 默认 intent=diagnose、mode=rule、diagnostic_id=job-report；snapshot_id 可省略，由认证 report 确定。mode 可显式为 deepseek，使用 U2 原配置、预算和降级逻辑；有配置不代表真实 API 已通过本阶段质量验收。intent=verify 必须同时提供现有详细 plan 和 gateway_plan_hash，plan 工程/版本/快照须与可信 report 一致；其他 intent 不接受这两个字段。

动作 expected_state_version 为 GET 返回的规范十进制字符串。例如：

```json
{"kind":"hint_next","action_id":"hint-1","expected_state_version":"5"}
```

允许 hint_next、finish、refresh_evidence、observe_verification；execute_plan 另带真实 grant_ref，recover 另带 question，prediction_request/prediction_submit 另带 prediction。波形 payload 与[预测协议](stage-18-prediction.md)一致。收到 challenge 前不提交预期；读取 observed 之前必须先显式提交独立预期。普通 HTTP reference/L4 动作全部拒绝。

同键同内容重放复用外部和内部身份；同键变更输入为 409。状态过期或投影碰到并发提交时 409，应重新 GET，再决定是否提交新的明确动作；不得自动重放结果未知的写请求。原始问题不保存到 U1 关联库，崩溃在创建与 attach 之间时，客户端用完全相同请求和键显式重试。`StartRequiresRetry` 表示预留关联尚未完成；恢复后的 RecoveryRequired 必须显式 recover。

每个工程由 U2 限制一个活动教学会话。WaitingStudent 仍占用该位置；完成当前交互时发送 finish，需要中止则 cancel，再开始下一 run。旧版本会话不可在新工程 revision 下继续；取消仍可用，新的教学会话须绑定新的已保存同步工程和新证据。事件游标过期返回 410，应重新读取当前状态后从最新高水位继续，不把事件当完整教学正文日志。

## 可信外部依赖注入

`sigflow_edu_agent.teaching_factory.compose(bootstrap, teaching_memory=..., reference_provider=..., receipt_port=...)` 是受信任的 Python 组合入口。MemoryAdapter、ReviewedMaterialProvider 和 receipt_port 的定义与责任见[U2 交接说明](u2-integration-handoff-2026-10-21.md)；普通请求不选择工厂或本机路径。默认 CLI 未配置实际 UA-10 和审阅材料，capabilities 应如实显示。

关闭服务时执行有界工作者停止和资源清理；关闭异常仍尝试所有已登记资源。Runtime/验证器的等待都有上限，不能将服务 close 返回解释为所有外部 I/O 已优雅结束。进程级验收还检查真实子进程退出码与 stdout 协议。

本轮没有改原生 GUI、demo、Core、共享冻结 schema 或 U3 数据。GUI/S1 后续按该显式扩展接入，并独立完成目标平台和交互签收；不能将旧基础路由的存在当作已经连接 U2。
