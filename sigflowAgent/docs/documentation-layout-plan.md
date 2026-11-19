# SigFlow Agent 文档目录整理方案

盘点基线为 `SigFlow_FPGA_CMake` 的 `60f3b4c`。L4 文档按当前工作区计入现状。本次整理已建立[文档导航](README.md)、[接口索引](interfaces/README.md)和[状态页](status.md)。下方保留盘点基线与维护规则。

## 原有文档结构

| 位置 | 现有内容 | 在整理中的用途 |
| --- | --- | --- |
| [项目 README](../../README.md)、[构建指南](../../docs/BUILD.md)、`docs/` 顶层 | 主项目入口、构建说明、早期架构草案及模块报告 | 从项目入口进入构建说明。保留早期草案的版本和状态标识。 |
| [团队协同](../../docs/edu-agent/Cooperate.md)、[团队决策](../../docs/edu-agent/DECISION-2026-10-07.md) | SigFlow 与 UCAgent 两侧的规格、分工、任务和交付约定 | 作为跨组分工与决策的查阅入口，按原路径引用。 |
| [Agent GUI 规格](../../docs/agent/SigFlow-Edu-Agent-GUI-Spec.md)、[主界面任务](../../docs/gui/task.md)、[GUI 第二轮](../../docs/GUI-ROUND2/README.md) | Agent GUI 和主界面改造的规格、任务与验收 | 沿用各自模块的维护入口。涉及 L4 的任务链接到功能规格。 |
| `docs/virtual/` | 虚拟硬件阶段报告和脚本生成的验收证据 | 继续沿用现有路径。[证据归档脚本](../../tools/virtual/archive_stage_evidence.py) |
| [Sidecar 说明](../../agent/README.md)、[发布说明](../../agent/RELEASE.md)、[全加器示例](../../examples/full_adder/README.md) | Sidecar 运行与发布、示例使用方法 | 随对应实现及示例维护。 |
| [共享契约变更记录](../../contracts/edu-agent/v1/CHANGELOG.md)、`contracts/edu-agent/v1/` | Agent/Gateway OpenAPI、U2 私有宿主 OpenAPI、Schema 和契约样例 | 作为各自协议的机器可读来源。版本及适用范围以相应契约声明为准。 |
| [Agent 入口](../README.md)、`sigflowAgent/docs/` | 运行指南、策略、接口设计、阶段任务、交付核对和 JSON 验收结果集中在同一层。整理前的工作区另有 L4 专题目录和独立试点设计。 | 在 Agent 模块下按用途建立稳定入口。 |

基线中 `docs/` 有 68 份 Markdown，分布在顶层和已有专题目录。`sigflowAgent/docs/` 有 96 份 Markdown 和 60 份 JSON，基线中的文件均在顶层。[当前 L4 专题](features/l4-reference/README.md) · [当前 L4 试点设计](features/l4-reference/design.md)

## 现有接口文档与对接入口

| 对接场景 | 当前来源 | 适用范围 |
| --- | --- | --- |
| GUI 与 U1/U2 组合服务 | [U1/U2 组合接入指南](interfaces/u1-u2-teaching.md)、[C++ 调用端](../../main/agent/AgentApiClient.cpp)、[Python 路由](../../agent/src/sigflow_edu_agent/teaching_routes.py) | `u1.u2-teaching.v1`、`/api/v1/u2`。指南覆盖会话、运行、动作、事件和取消。后续增加的[时间线](features/teaching-support/u2-teaching-timeline.md)与[学习记录](features/teaching-support/u2-support-sections-task.md)另有说明。整理时应按当前实现和测试补齐这些接口，并链接已有的[学习记录 Schema](../../contracts/edu-agent/v1/schemas/u2-learning-records.schema.json)。 |
| U2 独立宿主 | [U2 私有宿主 OpenAPI](../../contracts/edu-agent/v1/u2.host.openapi.yaml) | `u2.host.v1`，调试和兼容用途，使用 `/api/v1/teaching/...`。 |
| IDE、Sidecar 与工程证据 | [基础 Agent API](../../contracts/edu-agent/v1/agent.openapi.yaml)、[EDA Gateway API](../../contracts/edu-agent/v1/sigflow.openapi.yaml)、[Sidecar 启动协议](../../docs/edu-agent/sigflow/sidecar-bootstrap.md) | 基础会话 API、工程状态和证据 API，以及进程启动和就绪握手。 |
| U3、课程与记忆接入 | [U2 可信组合交接](history/u2-integration-handoff-2026-10-21.md)、[UA-10 接入](../ua10_module/docs/U2_INTEGRATION.md)、[RAG v0.1 联调](features/course-rag/rag-v0.1-u2-integration.md) | Python 注入边界与当前资料指向。模块自有安装和组件接口由相应模块维护。 |
| L4 参考解 | [L4 接口契约](features/l4-reference/contract.md) | `u2.native-reference.v1` 草案。申请、确认、读取与恢复接口需随试点实现和验收推进。 |

`interfaces/README.md` 作为对接入口，按调用方列出协议版本、路径或调用入口、适用状态、文档归属和契约链接。`interfaces/` 中的接入说明维护最小调用示例、身份、错误与版本兼容规则。共享 OpenAPI、Schema 和样例继续由 `contracts/edu-agent/v1/` 维护。L4 草案随专题迭代，索引直接链接；试点通过且跨组契约定稿时，再随接口变更提交对应的机器可读定义和契约测试。

## Agent 文档目录

```text
sigflowAgent/
├── README.md                    # 模块简介、快速启动、文档入口
└── docs/
    ├── README.md                # 关键入口、配套文档、历史与证据
    ├── status.md                # 按能力记录实现、验收及外部依赖状态
    ├── guides/                  # 安装、DeepSeek 配置、运行与排障
    ├── interfaces/
    │   ├── README.md            # 接口清单与版本、状态、对接方
    │   └── u1-u2-teaching.md    # GUI 与组合服务的对接说明
    ├── reference/               # 当前主循环、教学策略与证据规则
    ├── features/
    │   └── l4-reference/
    │       ├── README.md        # 功能入口与文档阅读顺序
    │       ├── spec.md          # 行为、边界和验收条件
    │       ├── design.md        # 申请、签发、核销、读取与恢复
    │       ├── contract.md      # 试点接入约定及正式契约链接
    │       ├── adr-001.md       # 可信确认与正文隔离决策
    │       ├── task.md          # 实现顺序、负责人和依赖
    │       └── verification.md  # 用例、执行方法和结果索引
    ├── evidence/                # 可追溯的验收输出与结果摘要
    └── history/                 # 已完成阶段的计划、交接记录及历史快照
```

## 文档优先级与归属

目录按文档用途和维护边界组织，优先级体现在 `docs/README.md` 的阅读顺序。一个功能的规格、任务和验收集中维护。比如 L4 联调时接口草案是关键材料，接口发布后主要入口变为正式契约，旧草案仍能通过专题回溯。这样的变化通过更新索引和文档状态记录，链接和路径可以保持稳定。

| 优先级 | 内容 | 现有文件与处理方式 |
| --- | --- | --- |
| 关键 | 模块入口、在用运行说明、策略与正式契约、活跃增量的规格和验收条件 | 将[环境指南](guides/environment.md)与[DeepSeek 配置](guides/deepseek-local-config.md)归入 `guides/`。复核[教学策略](reference/policy-v1.md)后归入 `reference/`。以现有组合接入文档为基础整理 `interfaces/u1-u2-teaching.md`。先按当前实现和联合验收记录更新[U2 交付核对表](history/u2-completion-status.md)，再建立 `status.md`，保留 2026-10-22 快照。 |
| 次要 | 设计背景、排障说明、已结束任务、交接与阶段记录 | 处理[总任务表](history/task.md)时，把仍适用的任务链接到对应专题，旧执行记录归入 `history/`。从[阶段 30 正文说明](history/stages/stage-30-reference-body.md)提取仍适用的校验规则并标明对应版本，阶段原文保留为历史记录。 |
| 验收证据 | 用例输出、联调回执、评估 JSON、与结论关联的日志 | 保留引用链并按阶段或功能归入 `evidence/`。每份摘要记录对应提交、环境、验证范围和执行结果。脚本读取的样例和待比较基线随契约或测试维护。 |
| 运行日志 | 本机启动、构建和调试原始输出 | 日常输出存到构建目录或 CI 产物。用于复现问题的日志摘录随验收证据保存，并去除密钥等敏感信息。 |

## 后续增量如何放置和维护

| 改动 | 文档落点 | 合入时更新 |
| --- | --- | --- |
| 小修复或操作步骤变动 | 现有指南或专题；在任务或 PR 中记录验证 | 更新受影响的步骤、行为和验证结果。 |
| 独立功能，例如教学策略扩展 | `features/<feature>/`。从规格、任务、验收三项起步，按复杂度补设计和决策记录。简短内容可用同页章节。 | 规格写清验收条件；任务引用验收条件；验收记录回填结果、提交和证据位置。功能状态变动时同步 `status.md`。 |
| 跨组接入、权限或数据契约变更 | 专题内保留提案及设计；`interfaces/` 提供统一接入入口。涉及共享接口时同步对应 OpenAPI 或 Schema、测试样例和变更记录。 | 验证调用方和服务方的兼容性，记录能力协商、状态码及生效版本。L4 正文材料继续走受控的参考解交付路径。 |
| 阶段完成或专题被取代 | 专题保留原链接及验收结论，标记完成或被取代，并链接后续版本。仅阶段快照、旧执行记录等过程资料归 `history/`。 | 新的运行规则同步到 `reference/` 或 `guides/`；验收产物按功能和版本放置并从 `verification.md` 引用。 |

新文档沿用功能名或接口名作为稳定路径。活跃专题入口记录状态、维护人和对应版本。后续迭代更新同一专题下的规格与验收；阶段编号用于保留里程碑快照。

## 迁移记录与维护检查

1. 已建立 Agent 文档导航、接口索引和能力状态。GUI 联调入口已归入 [U1/U2 接入手册](interfaces/u1-u2-teaching.md)。
2. L4 七份文档已归入 [L4 专题](features/l4-reference/README.md)，试点设计命名为 `design.md`。留有若干旧地址跳转页。
3. 已迁移运行指南、参考资料、阶段历史及 60 份原有验收 JSON。阶段复测命令将新结果写入构建目录。

每批合入前检查模块入口到活跃规格、运行指南、正式契约和验收证据的链接。路径改动涉及脚本读取时，运行对应验收脚本。已公开引用的旧路径可保留简短跳转页。
