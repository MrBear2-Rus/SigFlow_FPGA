# RAG 与 U2 开发阶段合并验证（2026-10-15）

合并 RAG v0.1 提交 `ffd6e5e` 与传入提交 `3834da6e09125c45051b4d6690eb716f95f742e1`。冲突文件为 README.md、loop.py、ports.py，保留双方能力：CourseProvider / CourseBundle、课程上下文及状态、DetailedPlan / PlanChecker / OutcomeChecker、baseline_evidence 和模型调用前的 assess 门禁。

联合行为修复：课程材料省略使用 `course:` 命名空间，与工程观测省略分开。教学模式评估仍拒绝工程省略、缺失证据、不支持模式或无真实 detector 的模式；可选课程预算省略不再把有效工程证据判成不完整。新增两项联合回归验证该边界。

既有模拟模型引用测试改为使用门禁支持的 latch 工程 fixture，并明确组合设计目标；位宽模式尚无生产 detector，不能为了调用 mock 绕过门禁。实际数据库装配、引用 ID、来源定位、译文身份和请求预算断言均保留。

|验证|结果|
|---|---|
|课程接口、概念门禁、计划及结果检查、引用链路联合测试|78/78 通过|
|合并后完整 Python unittest|293 项，27 个失败记录、4 个错误记录、6 项跳过|
|开发评测|40/40 通过；不是真实模型或工具验收|
|标准库模式 U2 示例|进入 WaitingStudent，课程 partial，充分性 not_assessed|
|数据库|SHA256 与 v0.1 交付一致，未修改|

完整测试的失败记录包含子测试，不应简单相减计算通过用例数。五项 RAG 质量失败沿用原断言；其他失败与错误均位于本机环回 HTTP 测试，表现为 SERVICE_UNAVAILABLE / WaitingEvidence（前次独立复现为 WinError10013）。未放宽这些断言，也未修改 HTTP 实现。六项历史 UCAgent 测试因未配置依赖跳过。

首次测试中的默认临时目录权限错误通过将本次测试进程的 TEMP/TMP 指向项目 tests/.rag-tmp 复核消除，未为此修改产品代码或测试断言。本地原始日志、冲突三方副本及暂存区快照保存在未提交的 audits/merge-2026-10-15。

验证范围仅 Python Agent 与 RAG；同次传入的 C++、GUI、硬件捕获等改动保持原合并结果，本轮没有构建或验收这些模块。
