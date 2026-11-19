# L4 参考解增量

文档基线为 `60f3b4c`。首版覆盖一个全加器进位错误、一个保存修订和一个受控原生界面。产品范围已按本轮讨论整理。新接口和原生交互尚待实现及联合验收，正式能力保持关闭。

| 文档 | 用途 |
| --- | --- |
| [spec.md](spec.md) | 定义用户流程、适用范围和可验收行为。 |
| [首版技术设计](design.md) | 说明各组件、数据流、材料来源和恢复机制。 |
| [contract.md](contract.md) | 定义 U1、U2、S3 对接所用的路由、字段、状态和错误。 |
| [adr-001.md](adr-001.md) | 记录参考解交付边界及选择理由。 |
| [task.md](task.md) | 跟踪每项实现、负责角色、依赖和退出条件。 |
| [verification.md](verification.md) | 定义验收矩阵、试点开放与关闭条件，实施后登记结果。 |

实现从 L4-T01 开始。产品意图调整先更新规格，跨组接口调整同步更新契约和测试。任务以 L4-R 需求和 L4-A 验收项关联，实际结果记录提交、构建环境和证据。设计与契约的分工是前者解释流程和边界，后者给出字段和状态细则。

文档结构参考 [GitHub Spec Kit](https://github.github.com/spec-kit/quickstart) 的规格、计划和任务流程，[Kubernetes KEP 模板](https://github.com/kubernetes/enhancements/blob/master/keps/NNNN-kep-template/README.md) 的测试及开放条件，以及 [Microsoft ADR 指南](https://learn.microsoft.com/en-us/azure/well-architected/architect-role/architecture-decision-record) 的重要决策记录方式。L4 保留这些结构中与当前授权增量直接相关的内容。
