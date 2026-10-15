# RAG 接入与单文件课程库

当前交付为 2026-10-15 RAG v0.1：仓库携带预构建数据库，554 Parent / 762 Child；运行不需要原始快照。最新调用和状态契约以 [U2 联调说明](rag-v0.1-u2-integration.md) 为准，验证与指纹见 [v0.1 交付记录](rag-v0.1-release.md)。下方保留 2026-10-12/13 的历史实现和验证记录，其中数量、Top3 装配描述及“尚未提交”不代表当前交付状态。

2026-10-12：课程检索代码归入 `sigflow_edu_agent/rag/`，教学流程通过 `courses.SQLiteCourseProvider` 调用。本次只完成本地接入；未调用真实大模型、未运行真实 EDA、未推送 Git。

2026-10-13：在单文件 SQLite＋FTS5 中新增 lowRISC 固定提交、Yosys 0.47 和 Verilator v5.052 官方资料。新增资料版本独立于 SigFlow 工具运行版本；本次不运行 EDA。文件级来源、许可依据、切分统计、审阅状态和完整验证见[外部资料实施记录](external-materials-catalog-2026-10-13.md)。

## 运行

在 `sigflowAgent` 目录运行：

```powershell
python -m sigflow_edu_agent --provider rule --once
python -m sigflow_edu_agent --provider rule --once --no-course
python -m sigflow_edu_agent --provider rule --once --course-db D:/path/to/course.sqlite
python -m sigflow_edu_agent.rag.query_cli "组合逻辑为什么出现锁存器"
```

开发目录默认库为 `data/course.sqlite`。安装后部署应通过 `--course-db` 或 `SIGFLOW_COURSE_DB` 指定库的位置。程序只读打开已有文件，缺库时不会创建空数据库；教学流程将 `course_status` 标为 `unavailable`，保留原工程提示能力。

核心 `TeachingLoop` 的课程接口仍为可选参数，旧调用方不配置时为 `disabled`。开发 CLI 默认启用课程；`--demo` 保留原来不使用课程的演示。

规则模式不会自行声明课程引用，所以即使 `course_status=ok`，卡片的 `course_refs=[]` 也正常。查看完整接入及引用结构：

```powershell
python -c "from examples.rag_smoke import main; raise SystemExit(main())"
```

该演示使用真实本地 SQLite、示例工程证据和固定的离线候选，不评测真实模型阅读理解。调用真实模型仍需原有显式凭据。

## 查询过程

1. E1/E2 取得并核对工程证据；工程证据无效时不查询课程。
2. E3 调用 `SQLiteCourseProvider.prepare()`，传入学生问题和诊断概念。
3. `rag.agent_adapter.prepare_course_context()` 调用检索器；同一只读事务读取正文、来源及内容标识，整理最多三段材料，不把本地文件路径传给模型。中文问题使用本地 `passage_mappings` 的中文标题、术语和常见问法召回英文原文；有对照译文时可选择译文单份进入上下文，并明确标为未教师审阅、非官方译文且关联原文 hash/行范围，不默认重复装入英文全文。
4. `courses.pack_courses()` 复用现有 `context.builder`，工程文字优先，课程按完整片段加入，课程片段预算为 8192 UTF-8 字节。超过预算时按完整片段省略，保留 `partial/omitted` 状态和 `byte_budget` 原因，未装入的 passage 不进入引用 allow-list。DeepSeek 最后仍检查整个请求的 16 KiB 上限。
5. E4 把实际保留的课程正文与 `course_refs` 交给候选生成模块。候选可增加 `course_passage_ids`，原四字段候选仍接受。
6. `checked_card()` 拒绝虚构、重复或没有提供过的课程编号，从本轮可信记录补齐卡片来源。译文引用只能回到其对应官方 passage；课程不能充当当前工程事实或工具执行证据。
7. 候选重试保留同一份材料；学生请求下一提示时重新取工程证据、重新查询课程。

课程状态为 `disabled / ok / partial / omitted / no_match / unavailable`。`no_match` 表示检索没有命中；`partial/omitted` 表示命中后有材料因预算未装入，省略记录保存在 `course_omitted`。AI 辅助原创讲解始终标记为待审阅，不冒充官方原文。

## 数据库整理

运行仅使用一个文件 `data/course.sqlite`。不再读取活动 manifest，不保留 `.previous`、版本号文件、回滚记录，也不支持旧表结构或旧 FTS 分词器。旧库明确报错并要求重新构建；没有自动迁移或兼容入口。

| 项目 | v3 基线（2026-10-12） | 当前 v4 |
|---|---:|---:|
| 当前数据库 | 3,473,408 字节 | 6,864,896 字节 |
| 可检索正文 | 266 条（257 YSYX + 9 authored_note） | 533 条（257 YSYX + 9 authored_note + 267 官方原文） |
| 登记正文 | 311 条，含 45 条不完整旧材料 | 578 条，含 45 条不完整旧材料；检索仍排除不完整项 |
| HDLBits 练习元信息 | 9 条 | 9 条 |
| 本机绝对路径 | 无 local_path 字段 | 无 local_path 字段 |
| 表结构 | user_version=3 | user_version=4，新增来源文件 provenance、双语映射和译文关联表 |

来源、材料属性、正文范围、校验值、概念及别名、相关练习和 FTS5 检索索引均保留。外部首批另登记 60 个固定来源文件（54 个用于构建），267 条官方原文 passage 各有中文检索映射，25 条核心对照译文绑定 passage ID 和原文 hash。FTS5 自动生成的内部表属于检索实现，不能当作历史数据删除。未支持概念的别名继续保留，使相关问题返回 `no_match`，而不是用相邻主题冒充答案。

`index_version` 字段保留，但其值改为资料内容的 SHA-256 标识，用来标记本次引用依据、检测数据库更新。它不指向旧文件，也不提供历史版本兼容。更新时先构建临时文件，通过完整性检查后替换当前文件；不额外保存旧库。

原始 311 条整理记录、45 条待补齐材料的登记和公式恢复审计仍保留在包内 `rag/resources/courses/`，便于继续完善资料。新增官方原文快照存放于 `data/external_sources/`，构建时按固定清单、SHA-256 和行数校验；运行时只读数据库，不读取原始源文件。原始官方文件、完整原文 passage 和对照译文不打入 Python wheel；SQLite 的本地 Git 范围沿用 2026-10-13 用户决定，本轮没有提交或推送改动。

按用户选择，旧 U3 工作区的源码、测试、整理资料和演示源码也保留作历史参考，仅删除旧 `rag/index/` 中已替代的 32 个生成文件，共 44,492,418 字节。旧源码不再打包为运行依赖，当前正式实现不提供旧版本兼容。memory 和 UA-11 已改用正式包的课程接口及概念资料。

## 重建

```powershell
python -m pip install -e ".[course-build]"
python -m sigflow_edu_agent.rag.importer --snapshot-dir D:/path/to/ysyx/2607
```

`--snapshot-dir` 必填，不再猜测原文所在目录。可指定 `--resources` 和 `--db`。构建会验证原文快照和登记范围，筛掉不完整材料，生成内容标识，整理 FTS 索引并压缩空闲页。PyYAML 仅供构建及章节登记使用，运行检索不需要第三方依赖。

## 历史验证与发布边界

以下是 2026-10-12 基线回归记录；2026-10-13 外部资料专项和现有 Agent 回归结果见[外部资料实施记录](external-materials-catalog-2026-10-13.md)。

本机完整回归：Agent 共发现 101 项，95 项通过、6 项上游 UCAgent 集成测试因环境未配置跳过；memory 76 项和 UA-11 30 项通过。合计 201 项通过、6 项跳过。包含 27 项课程资料回归和 14 项接入/当前库检查；后者使用自行编写的测试材料，无需课程原文。课程资料回归在没有快照时明确跳过，可设置 `SIGFLOW_COURSE_SNAPSHOT` 配置快照位置。

额外验证：构建 wheel 并解包到独立目录，以 `python -S` 关闭第三方依赖加载、通过显式数据库配置运行教学流程，检索和课程引用均通过。安装包包含 11 个整理资料文件，不含 SQLite 数据库。清理旧索引后，再次通过 memory 的课程引用适配测试；当前库完整性及外键检查通过，确认 266 条材料、9 条练习且无 `local_path` 字段。

```powershell
python -m unittest discover -s tests -v
```

数据库包含一生一芯正文，公开分发许可仍未确认；单独分包不等于获得授权。2026-10-13 用户决定将当前 `data/course.sqlite` 纳入 Git 提交范围，忽略规则仅为此文件开放例外，其余 SQLite 文件仍被忽略。Python 安装包仍不携带数据库或原文快照。纳入上传范围不表示来源许可已核实，目前尚未提交或推送本轮改动。
