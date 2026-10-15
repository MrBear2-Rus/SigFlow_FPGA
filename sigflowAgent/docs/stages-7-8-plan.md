# 阶段 7–8：计划审批材料与只读报告接入

状态：本地实现完成，真实联合验收待完成。依据本地 task.md 阶段 6 再规划及用户继续实施指令。

**Goal:** 将 U2 详细计划输出为可评审的共享 PlanCard，并接入固定 Gateway 报告路由。
**Architecture:** 纯计划映射不产生授权；只读客户端获取报告，受信证据目录绑定后进入现有主循环。
**Tech Stack:** Python >=3.11 标准库，unittest 与本地 HTTP fixture server。

## 约束与方案

继续使用当前 sigflowAgent，保留已有未提交阶段 4–6，不 push。
不修改 agent、共享 Schema、C++ 或团队任务状态，不清理工作区外文件。
优先已有契约映射与只读联调；另两个方案为直接开发 Job 执行服务、先开发 L4，
均需要跨组授权与恢复机制，本批不采用。

## Task 7：审批卡映射

- [x] `contracts/plan.py` 消费 DetailedPlan，导出 PlanCard 及本地绑定信息。
- [x] 将内部步骤 ID 确定性映射 s1..sN，top 映射 top_module，保持 build 依赖。
- [x] timeout_ms 不等于 duration_ticks，不允许静默丢弃或单位猜测；不能映射时拒绝。
- [x] 分别记录内部 fingerprint、规范化审批材料摘要；后者为本地交接摘要，未约定为 Gateway plan_hash 算法。
- [x] provider、snapshot、预期观察与预算必须保留在审批材料，不能只展示部分授权语义。
- [x] 测试映射、hash 稳定与敏感字段变化、非法参数/ID/版本；输出 stage-7-plan-contract.md。

## Task 8：只读报告与证据桥

- [x] 扩展 GatewayClient.job_report(job_id, project_id, revision, snapshot_id)，仅 GET 冻结路径。
- [x] 校验路由 ID、认证、握手实例、信封和报告绑定；拒绝重定向、超限、畸形响应。
- [x] `context/gateway_report.py` 将 wire artifact_id/sha256 明确映射为现有适配器 id/hash。
- [x] GatewayReportSource.collect 使用宿主提供的可信锚点目录；不从响应伪造 anchor/source_ref。
- [x] HTTP 测试覆盖正确路径、缺报告、过期、跨项目、legacy、不完整和旧版本。
- [x] 将报告源接入 TeachingLoop 演示测试；无证据时 WaitingEvidence，不猜测“无错误”。
- [x] 完成 stage-8-gateway-report.md、手动对接说明和本地 task.md 再规划。

测试策略：先补失败测试再实现；新旧测试均需通过；可选真实 UCAgent 与核心隔离分开记录。
本地 HTTP fixture 测试不等于真实 C++ Gateway/EDA 联调。

## 已发现契约边界

GrantQueryData 不返回 snapshot_id、used_jobs、steps；不能用“active”直接等同完整执行授权。
PlanCard 与 CreateJobRequest 的参数集合不同；审批展示不可直接当作 Job 提交体。
DetailedPlan.timeout_ms 与协议 duration_ticks 无可靠自动换算。
报告 wire artifact 字段是 artifact_id/sha256/role；内部 id/hash 只用于规范化，不修改共享字段。
