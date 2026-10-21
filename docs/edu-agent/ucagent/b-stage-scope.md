# B 阶段范围登记

> 登记日期：2026-10-22
> 状态：**延期 / 待评审**，不属于当前 `edu.api.v1` sidecar 最小闭环。
> 原则：下列能力不能既不排期也不删除；在评审通过前不新增路由、不修改冻结 schema、不宣称已实现。

## 1. 当前边界

当前 sidecar 只提供：

- `POST /api/v1/sessions`
- `GET /api/v1/sessions/{session_id}`
- `POST /api/v1/sessions/{session_id}/runs`
- `GET /api/v1/runs/{run_id}`
- `GET /api/v1/sessions/{session_id}/events`
- `GET /api/v1/health`
- `GET /api/v1/capabilities`
- `POST /api/v1/shutdown`

没有 `POST /runs/{run_id}/cancel`、`POST /runs/{run_id}/actions`、`GET /sessions/{session_id}/state`、
历史/导出 API，也没有 E0–E7 阶段状态机。会话、run 和事件只在内存中，当前**没有 SQLite 实现**。

## 2. 延期项

| 能力 | 状态 | 归属 | 进入条件 |
|---|---|---|---|
| 版本化阶段状态机、E0–E7、工具注册表、阶段/策略/结果 Checker | 延期 / 待评审 | 教学工作流组 | 先冻结阶段、动作、证据和失败语义，并补齐契约与 fixture |
| 持久化 run 检查点、`RecoveryRequired`、崩溃恢复、预算与取消 | 延期 / 待评审 | Agent 运行时与接口组 | 需要新的恢复契约、幂等测试和宿主重启演示 |
| `POST /runs/{r}/cancel`、`POST /runs/{r}/actions`、`GET /sessions/{s}/state`、历史/导出 API | 延期 / 待评审 | Agent 运行时与接口组、教学工作流组 | 版本化路由、状态机、UI 动作和错误码共同冻结后再实现 |
| 完整 typed client 的阶段动作与事件恢复 | 延期 / 待评审 | Agent 运行时与接口组、UI 组 | 依赖上面的动作/状态路由和恢复语义 |
| 最小学习元数据 / 执行恢复检查点的 SQLite 落地 | 延期 / 待评审 | 学习数据组、Agent 运行时与接口组 | 先确定数据模型、迁移、导出/删除和故障恢复 |

## 3. 归属说明

- E0–E7、L1–L4 教学策略和参考解门控：教学工作流组。
- 学习画像、学习事件、导出/删除和 SQLite 学习数据：学习数据组。
- C++ Gateway、Snapshot、JobService、报告和波形：SigFlow 宿主 / Gateway 组。
- 教学面板和交互：UI 组。

## 4. 明确不做

- 不在当前 `edu.api.v1` 上偷偷增加阶段字段或动作路由。
- 不把内存中的 run 状态冒充为可恢复检查点。
- 不把 SQLite 学习记录写成已实现，也不在未完成迁移/导出/删除设计前创建数据库。
- 不用 `more=true`、客户端字段或纯文本事件解锁阶段动作、取消或 L4。
- 不把 B 阶段能力当作当前 sidecar 发布验收的一部分。

## 5. 评审出口

每一项从“延期 / 待评审”转为实施前，至少需要：

1. 版本化路由或 schema 变更记录，并说明向后兼容策略。
2. 成功、拒绝、超时、恢复和幂等 fixture。
3. 宿主/UI 与 Agent 双方共同确认的动作状态和错误映射。
4. 对应平台的运行证据；Windows 通过不能替代 Linux/麒麟原生验收。
