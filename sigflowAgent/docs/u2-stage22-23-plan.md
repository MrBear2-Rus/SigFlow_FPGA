# U2 依赖复核与学习接口准备实施计划

日期：2026-10-19。

**Goal:** 明确完整 U2 的真实依赖缺口，完善 U2 自身的教学事实交付和卡片生成前历史接口。

**Architecture:** 沿用正式 U2 宿主、状态事务和 SQLite。教学事实仅投影已提交的元数据，在同一事务进入自有待交付队列；可选可信适配器在事务外交付。学习历史仅通过可信组合端口传入原有 CardProvider；没有适配器则保持禁用。

**Tech Stack:** Python 3.11+、标准库、既有 SQLite、TeachingLoop、Runtime 和 Gateway 契约。

## 全局约束

- 本轮只修改 sigflowAgent 下 U2 负责的代码、测试和文档；不实现 U3 学习数据库、画像、保留/删除或外部服务，不改 S1/S2/S3 的契约和实现。
- 依据当前团队 task/spec、U2 host 扩展文档与既有 HTTP 接口；UA-10 的 Python 签名尚未冻结，仅提供 U2 内部适配端口，不能宣称已与 U3 真服务联调。
- 不提交、不推送、不拉取，不删除既有用户文件。备份先前工作，测试仅清理自建的工作区临时目录。
- 不新增第三方依赖，不依赖外部 UCAgent，不要求激活 Conda。不调用真实 DeepSeek，不读取或输出用户密钥。
- 记录不包含问题、源码、卡片正文、Key、grant、receipt。学习历史不是工程证据、审批或掌握程度结论，不能升级提示级或增加模型调用。
- 复用现有调用链，未配置接口保持禁用；外部失败不得回滚已提交教学状态。丢失交付确认按同 event_id 重试，U3 负责最终持久化去重。

## 阶段 22：依赖审计与真实支持声明

Files: docs/u2-dependency-audit.md、patterns/support.py、tests/test_evaluation_handoff.py。

- [x] 按 UA-05/06/07/08 核对运行时、HTTP、事务/恢复、取证/审批、波形/激励、RAG、UA-10、参考材料、评测、平台与 GUI。
- [x] 用失败测试纠正“接收报告映射等于生产 detector 已存在”的支持声明；明确适配器可用和真实 detector 待交付的区别。
- [x] 生成阶段文档和可核验的依赖检查记录；已具备能力不等于完整团队 Gate。

## 阶段 23：U2 学习事实与历史端口

Files: host/teaching_memory.py、host/store.py、host/runtime.py、domain.py、loop.py、deepseek.py；新增 tests/test_teaching_memory.py、tests/test_teaching_history.py。

Interfaces:

- U2 内部适配器 `record_teaching_fact(fact) -> {event_id, accepted}`，仅 accepted=true 且 event_id 精确匹配才确认交付；并非新增共享 HTTP API。
- `get_teaching_context(*, learner_id, project_id, concept_ids, max_bytes) -> {learner_id, project_id, concept_ids, summary}`。这是 U2 的待适配端口，U3 可在自己的组合层转换实际接口，不约定其数据库或内部类型。
- `DurableStore.pending_teaching_facts(limit=32)` 与 `ack_teaching_fact(event_id)` 只供可信宿主读取/确认，普通 HTTP 和模型无调用入口。
- `Runtime(..., teaching_memory=None)` 可注入上述可信适配器；默认禁用。卡片只增加受限 `learning_history` 上下文，不改变 Evidence 或模型调用数。

- [x] 先失败回归：状态与事实原子性、回滚不交付、重放/恢复不重复、取消与终态、敏感正文排除、接收失败/错确认可重试。
- [x] 在 create/commit/cancel 的同一事务投影 run_created/card_generated/student_action_accepted/run_finished；event_id 根据宿主实例、session/run、事实类别及主体稳定生成。
- [x] 单独交付线程只读待交付元数据；失败不影响状态推进，同键重试，关闭有界；不在宿主全局锁或 SQLite 事务内调用 U3。
- [x] 在每次生成卡片前查询有界历史；验证 learner/project/concepts，一轮候选修正复用同一历史，不增加模型调用。无匹配、错误、越界、超额时降级为空。
- [x] 执行定向与完整 U2 回归、31 项 GUI 回归及真实 GUI/宿主/工程证据联合测试；分别记录模拟接收端和真实 EDA 证据。
- [x] 输出阶段交接文档、验证记录及剩余任务再规划。U3 真服务、共享签名冻结和外部验收仍保持待交付。

## 方案自检

沿用现有事件而只增加即时 callback 会在崩溃或接收失败后丢事实，因此采用 U2 元数据待交付队列。另建学习记录库或推断画像会越过 U3 职责，因此不采用。直接给模型读取全部状态会扩大敏感数据与提示权限，因此仅提供绑定范围、有界且独立标记的历史摘要。
