# RAG v0.1 内部 U2 联调接口

本版本供 TeachingLoop 集成，不是学生教学质量正式验收版。按交付要求，Git 仓库携带预构建数据库；来源使用边界不因提交而改变。版本指纹及已知限制见 [v0.1 交付记录](rag-v0.1-release.md)。本机完整冻结结果与修复前备份仍保存在未提交的 audits 目录。

## 接入

现有调用方式不变。真实宿主提供与项目、revision 绑定的工程 Evidence；E2 检查有效后，E3 自动查询并装配课程材料，再进入 E4。课程异常仍非阻断地记录为 unavailable；核心教学状态机顺序未改。

```python
from pathlib import Path
from sigflow_edu_agent.courses import SQLiteCourseProvider
from sigflow_edu_agent.domain import RunRequest
from sigflow_edu_agent.loop import TeachingLoop

loop = TeachingLoop(
    RunRequest(project_id, revision, issue_id, question),
    engineering_source, card_provider,
    course_provider=SQLiteCourseProvider(Path('data/course.sqlite')),
)
state = loop.run_until_pause()
print(state.course_status, state.course_diagnostics)
```

可运行的离线示例使用明确标记的工程 fixture，不访问真实模型或 EDA：

```powershell
# 在仓库的 sigflowAgent 目录执行
python -m examples.rag_v01_u2
```

## 输入与输出 DTO

|接口|输入|输出与边界|
|---|---|---|
|`SQLiteCourseProvider(db_path=None).prepare(question, concept_ids)`|question 字符串、concept_ids 字符串 tuple；unknown 被移除|CourseBundle：候选材料，不是已装配证据。未知概念 ID 抛 ValueError；已登记但无证据的概念返回 no_match / unsupported_concept_ids。|
|`prepare_course_context(query, concept_ids=None, db_path=..., top_k=3, candidate_limit=None)`|concept_ids 是 list；生产 provider 指定 candidate_limit=20|PreparedCourse；默认旧 Top3 兼容。新增 selection_diagnostics 默认空 dict。|
|`retrieve_course_candidates(..., limit=20)`|受限候选池|agent_context 为空，context_status=not_packed。不能直接当 Agent 证据。|
|`pack_courses(evidence, bundle)`|合法工程 Evidence、CourseBundle|原四元组不变：`(packed_evidence, admitted_refs, status, omissions)`。|
|`resolve_evidence_citations(cited_ids, admitted_ids, index_version=..., db_path=...)`|只传当前装配 refs 的 ID 作为白名单|验证当前 DB 版本、child ID、原文 hash、来源跨度；拒绝未装配、伪造、重复、非字符串 ID。|

CourseBundle 原有字段保留：status、materials、interface_version、question、required_sources、required_evidence_keys、retrieved_candidates、unsupported_concept_ids、selection_diagnostics。新增状态沿 selection_diagnostics 传递，未增加第二套检索 DTO。

CourseMaterial 包含 reference、text、parent_id、source_id、coverage_keys、selection_score、candidate_rank。CourseReference 的 passage_id 在 evidence-v2 中是 child ID，parent_id 是父资料 ID；index_version 绑定当前库；material_kind 区分 source_excerpt / translated_source / authored_note；source_content_hash 绑定英文原文或笔记正文。source_spans 记录 document_id、source_url、行范围、原文与原始源文本 hash、document_hash、normalization、locator_kind。未审机器译文有独立 translation_id；不是官方中文译文。

Evidence.course_diagnostics 原有默认值为 `{}`；RunState 新增同名字段，默认 `{}`，在 E3 成功装配后复制结构化状态。state 是 deepcopy，读取不能修改循环内部。旧构造函数和 pack_courses 四元组不变；disabled / unavailable 时诊断为空，按既有状态处理。model 的 CardContext 读取 Evidence.course_diagnostics，序列化时去除 debug。

## U2 应读取的状态

|字段|含义|
|---|---|
|course_status / packing_status|no_match：无候选；ok：候选成功装配且没有记录的缺口/遗漏；partial：已装配但有来源/要求缺口、预算遗漏或选择上限留下候选；omitted：有候选但全部未进入上下文。disabled / unavailable 延续旧契约。|
|retrieval_status|候选检索 ok / no_match，和装配状态分开。|
|source_coverage|候选阶段的来源覆盖，包含 candidate_count / returned_count；covered 只表示检索到该来源。|
|missing_sources|明确指定来源中无相关候选的 source ID。|
|missing_required_sources|实际装配后缺失的指定来源；可由预算或候选缺失造成。|
|missing_required_evidence|查询推导的定义/交叉引用要求未装配；不是完整事实级 gold。|
|unsupported_concept_ids|已登记但无 grounded 材料的概念。|
|answerability=not_assessed|充分性未验证。即使 ok 或来源 covered，也不能保证完整回答。|
|omission_counts|byte_budget / token_budget / request_budget / engineering_over_budget / item_limit 计数。|
|candidate_not_selected_count|选择上限留下的候选数；不等于预算遗漏数，也不代表 Agent 已见过候选正文。|
|budget_used / budget_remaining / budget_limits|字节、UTF8 字节上界 token 估计和完整请求预算。|
|debug|仅宿主诊断：retrieved_candidates、selected_evidence、packed_evidence、omitted_evidence、candidate_not_selected。不传模型；候选列表不能作为引用白名单。|

判断顺序：先处理 disabled/unavailable；再看 retrieval_status；随后看 packing_status、missing_sources、missing_required_sources、omission_counts 和 candidate_not_selected_count；始终保留 answerability=not_assessed 的边界。partial 不触发新的教学状态机分支，课程证据继续为辅助层，工程证据身份与优先级保持。

8192 字节上下文、4096 UTF8 字节上界 token 估计、16 KiB 完整请求保护保留。已有工程文本过大时不删除工程内容来让课程适配；记录课程省略，真实请求仍受最终 serializer/provider 限制。引用必须来自本次 admitted_refs，不能使用所有候选 child ID。authored_note 的行号仅指 `sigflow-note:///team-note:...` 本地笔记；官方辅助链接不是笔记原文出处。

## 离线验收

```powershell
python -m pip install -e ".[course-build]"
python -m unittest tests.test_rag_v01_closeout tests.test_courses tests.test_rag_evidence_units -v
```

以上构建回归测试还需要登记的课程原始快照及官方原文，路径要求见测试和构建说明；它们不是使用预构建数据库的前置步骤。9 项新增测试包含子案例：概念正例/改写/负例、实际 Adapter→Provider→装配→U2、无匹配、已登记但不支持的概念、指定来源缺失、多来源部分覆盖、预算省略、错误及未装配引用。既有测试继续覆盖 Parent/Child hash、来源行号、本地笔记、请求保护与兼容 DTO。未调用真实模型；这些测试不证明模型回答质量或完整真实工程联调。
