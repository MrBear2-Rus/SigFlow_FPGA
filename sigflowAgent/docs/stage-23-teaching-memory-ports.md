# 阶段 23：U2 教学事实与历史上下文端口

日期：2026-10-19。范围仅为 U2 内部宿主接入点；UA-10 实际服务及共享签名仍待适配和签收。

正式 bootstrap/ready 入口的可信注入方式及进程级契约验证见[阶段 24 接线](stage-24-memory-entry.md)。默认 CLI/GUI 不自动加载接收端。

## 实现边界

Runtime 支持可信组合参数 teaching_memory，满足 ports.TeachingMemoryPort；默认 None。不自动导入 U3 模块、不读取外部代码路径、不创建 U3 学习库。私有 capabilities 的 configured/reason 区分已配置和 adapter_required，不新增共享 HTTP 路由。

U2 状态库新增 teaching_outbox 和动作类别列，仅作教学状态交付队列，不承担画像、概念掌握度、历史查询、导出、删除或保留期管理。旧库迁移不重新解释过去的请求；恢复旧卡片不计作新生成，下一层提示只为新增卡片产生事实。

## 提交后事实

| event_type | 来源及含义 |
| --- | --- |
| run_created | 已提交的教学运行创建，不代表证据检查或工具成功 |
| card_generated | 本事务新增的已检查卡片；仅级别、producer、证据 ID |
| student_action_accepted | 已提交的学生动作处理元数据；status 不代表复测成功 |
| run_finished | 教学终态 Completed/Cancelled/Failed/BudgetExceeded，不等于 EDA Job 终态 |

共同字段包括 schema_version、event_id、instance/session/run/learner/project、revision/snapshot/Job/diagnostic/issue/policy、state_version、status、evidence_origin 和 concept_ids。Job 绑定原教学依据；后续观察由卡片 evidence_ids 区分，不推断独立修复或掌握程度。fixture/tool 来源始终保留。

事实与状态在同一 SQLite 事务提交；失败不交付。event_id 由实例、session/run、类型及稳定主体生成，重放和重复取消不新增记录。不包含原始问题、卡片正文、源码、模型请求、Key、grant 或 receipt。

可信 U2 适配端口为 record_teaching_fact(fact: dict) -> dict。持久接收或去重完成后，应返回 event_id 精确相同且 accepted=true 的回执。该名称不要求 U3 重写自身方法；组合层依据双方签收的接口转换。

丢失、异常或不匹配回执保留队列，后续仍使用原 event_id；接收方负责持久化去重。独立交付线程不持有 Runtime 全局锁或 SQLite 事务，慢接收不阻塞提示/取消；关闭等待有界，未知结果不当成已接收。可信 pending_teaching_facts(limit=1..32) 和 ack_teaching_fact(event_id) 仅供组合层消费，普通 HTTP/模型无入口。损坏 JSON 被拒绝转发。

适配器运行在线程边界：事实交付和历史读取可能并发。实际 U3 适配层须使用其已约定的线程安全调用方式，不能直接跨线程复用有线程限制的 SQLite 连接；该适配语义尚需双方签收，不修改 U3 存储实现。

## 生成前历史

内部适配端口：

```python
get_teaching_context(
    *, learner_id: str, project_id: str,
    concept_ids: list[str], max_bytes: int,
) -> dict
```

结果仅有 learner_id、project_id、concept_ids、summary。范围来自可信 SessionBinding 和已取证知识点，不从问题正文提取；返回范围必须精确匹配。摘要最多 1024 UTF-8 字节，读取最多等待 250 ms，每个桥接器最多一个未完成读取。超时、错范围、无效摘要或已知模型密钥回显均降级为空。

每张新卡片读取一次，模型候选修正复用同一历史；不增加原有模型预算。learning_history 是 CardContext 的独立字段，不改变 Evidence、课程引用、审批或初始提示级。预算不足时舍弃历史、保留工程证据；DeepSeek 仍检查最终请求限额。

历史输入不直接写入 U2 状态/检查点，不跨学生缓存；生成的教学卡仍按原规则持久化。历史不解锁 L4、不产生工具权限，不将接触知识点解释为掌握。

## 验证和交接

新增 14 项回归覆盖事务回滚、重放、终态、取消、旧库迁移、历史卡片不重计、元数据边界、未知回执同键重试、慢接收隔离、历史范围/超时/线程限额及请求预算。测试接收端只验证 U2 端口，不代表 UA-10 服务。

真实 Core/Yosys/正式宿主/同款原生浮窗联合测试另核验 tool 来源的待交付事实，仍明确 configured=false、real_receiver=not_connected。真实 Verilator/精确波形/显式预测另保留 single_evaluation 和动态刺激缺失的限制。最终结果见 verification-stage22-23.json、gui-stage23.json 和 simulation-stage23.json。

本轮未修改 agent、Core、C++ GUI、U3 课程实现或 U3 学习结构；未调用真实 DeepSeek，未提交或推送。

U3 实际适配器、签名/确认语义、profile 删除后的旧队列防复活和生命周期联合验收仍待交付；正式启用前由 U3 与组合层签收，本轮不替代实现。L4 材料/正文、冻结模型集、教师评分、动态刺激及目标平台签收仍独立待办。
