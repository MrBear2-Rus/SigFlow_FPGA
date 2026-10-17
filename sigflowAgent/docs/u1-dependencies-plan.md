# U2 运行时依赖实现计划

> 执行方式：按测试驱动分阶段实施；独立模块使用 subagent-driven-development 审查。开发位于用户指定的现有工作区，不提交或推送。

目标：提供承载现有 U2 核心的真实 HTTP sidecar，使教学动作、持久化和受控验证具备可运行集成测试入口。

架构：stdlib HTTP + TeachingSession；SQLite 动作事务与预算；受限 Gateway 验证端口。Python >=3.11，无新增第三方运行依赖。

## 阶段 D1：持久状态与预算

- [x] 新增 host/store.py、tests/test_host_store.py；定义 create/get/claim/commit/action/reserve_model 接口。
- [x] 验证同键重放、冲突、版本 CAS、跨实例拒绝、持久预算、pending 重启和单进程写入锁。
- [x] 先验证缺失功能失败，再实现并运行定向测试；产出 D1 报告，核对 D2 输入。

## 阶段 D2：教学核心承载

- [x] 新增 host/runtime.py、tests/test_host_runtime.py，复用核心的状态机与检查点。
- [x] 实现固定范围会话、异步推进、动作串行、事务提交、模型预算预留、显式恢复；数据库失败禁止发布未提交结果。
- [x] 测试 L1→L2→L3、重复动作、旧版本、跨 learner、重启恢复与取消；产出 D2 报告。

## 阶段 D3：HTTP 与 sidecar

- [x] 新增 host/http.py、host/__main__.py、bin/sigflow-u2-host.cmd/POSIX 启动器。
- [x] 实现 health/capabilities、teaching sessions/state/actions/events、shutdown，基础 bootstrap/ready 与宿主兼容。
- [x] HTTP 集成测试验证鉴权、边界、202、轮询、恢复和关闭；写扩展协议、D3 报告。

## 阶段 D4：受控 Gateway 执行

- [x] 新增 host/verification.py、tests/test_host_verification.py；实现 VerificationPort。
- [x] 核对 C++ grant/Job 参数与计划摘要；验证授权不匹配、失败停止、build 依赖、回应丢失和幂等持久账本。
- [x] 将可信结果交给 U2 OutcomeChecker；不得构造成功事实或修改 RTL。产出 D4 报告。

## 阶段 D5：进程与真实联调入口

- [x] 子进程 bootstrap 测试、运行手册、可信锚点样例和真实 Gateway 探针。
- [x] 执行核心全量、宿主单元与 fixture HTTP/进程集成；检查文档命令、链接和来源描述。
- [x] 报告真实 EDA/DeepSeek/Linux/完整 L4 等尚未验收项，更新 task.md 并据结果再规划。

## 最终检查与再规划

2026-10-15：D1–D5 的本地实现及分阶段文档完成；核心与新宿主 328 项中 322 项通过、6 项历史实验跳过；host 相关 109 项通过；旧 agent 独立回归 124 项通过；开发评测 40 项通过。独立审查发现的取消竞态、旧审批污染和存储边界问题均已修复并复审。无提交或推送。

下一阶段按 D5 手册对接 UI 教学扩展、真实 Gateway/报告/锚点、受控 Job、DeepSeek 与 Linux。真实验收未被本地测试替代；UA-01～04 的其他团队职责、U3 全量学习记录与完整 L4 不在本批完成声明内。
