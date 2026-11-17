# SigFlow Agent 开发文档

## 关键入口

| 目标 | 文档 |
| --- | --- |
| 启动、配置和本地验证 | [模块入口](../README.md) · [环境指南](guides/environment.md) · [DeepSeek 配置](guides/deepseek-local-config.md) |
| 对接 GUI、U1、Gateway 或 UA-10 | [接口索引](interfaces/README.md) · [GUI 与 U1/U2 教学接口](interfaces/u1-u2-teaching.md) |
| 确认当前能力、依赖和验收缺口 | [U2 状态](status.md) |
| 接手 L4 增量 | [L4 专题](features/l4-reference/README.md) · [实施任务](features/l4-reference/task.md) |

## 配套文档

- [教学主循环](reference/teaching-runtime.md)、[教学策略](reference/policy-v1.md)与[教师评审](reference/teacher-review.md)记录运行边界与教学质量的核查方式。
- [教学辅助内容](features/teaching-support/README.md)涵盖时间线、课程引用、学习记录和检查器状态。
- [课程 RAG](features/course-rag/README.md)汇总部署、检索接入、资料来源和交付版本。

## 历史与验收

- [验收证据](evidence/README.md)用于定位阶段报告和可复查的 JSON 结果。
- [开发历史](history/README.md)保留阶段计划、实施记录、交接说明与旧状态快照。
- [目录与维护规则](documentation-layout-plan.md)说明后续增量的规格、接口、任务与验收文档如何更新。
