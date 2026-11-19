# U2 耦合依赖补全计划 D6–D8

> 执行：subagent-driven-development 分工实现独立模块；测试先失败再实现，逐阶段报告及审查。沿用已批准的 U1 依赖设计和 D5 再规划，用户要求继续实现；不新增提交或推送。

目标：补齐 U2 真实教学闭环直接依赖的动态证据、长 Job 只读观察和状态/事件重同步。独立 U1 打包、安装、桌面 UI 开发不在本批实现范围。

架构：保留 `sigflowAgent/sigflow_edu_agent/host`。文件目录仅由可信宿主配置，普通 HTTP 不注入证据或路径。观察动作与执行动作分离，观察不会创建 Job，不携带 grant，也不能解锁后继步骤。运行时继续使用 SQLite 事务和已有状态机。

## 全局约束

- Python >=3.11、仅标准库；不依赖 UCAgent、不新增真实模型额度消耗。
- 保留旧 agent 服务和已有未提交改动；只在用户指定现有工作区开发，不删除外部文件。
- 新教学路由仍为 u2.host.v1 扩展，不自行修改冻结共享 v1。
- 不自动恢复执行授权、不猜未知 Job、不通过工具成功推断功能正确；不解锁 L4。
- 对真实 Gateway/EDA/DeepSeek/Linux 的验收与 fixture 测试分别记录。

## D6：可信锚点的动态供应

文件：新增 `host/catalog.py`、`tests/test_host_catalog.py`、`docs/u1-d6-catalog.md`；修改 `host/factory.py`、`host/__main__.py`。

接口：`AnchorCatalog(path).snapshot() -> tuple[EvidenceAnchor, ...]` 每次严格重读；`load_anchors(path)` 保持既有导入可用；`make_factory`/`make_report_resolver` 同时接受静态序列与 AnchorCatalog。每次 collect 使用同一完整快照，不因损坏或缺失回退旧目录。CLI 校验启动目录后传递活目录，显式 CLI 参数优先。

- [x] 先写文件从空→有锚点、已存在 source 重新取证、结果 resolver 更新、损坏/撤回后失效、重复/超限/错误范围拒绝的测试；先观察新增接口缺失失败。
- [x] 将目录验证拆为独立模块，工厂使用可更新 source；不把报告内容当可信锚点。
- [x] 定向运行 catalog、factory/HTTP、process 测试；记录兼容静态序列结果并产出阶段文档。

## D7：已提交验证步骤的只读续查

文件：修改 `domain.py`、`ports.py`、`loop.py`、`host/verification.py`、`host/runtime.py`、`host/http.py`；新增 `tests/test_verification_observation.py`、`docs/u1-d7-observation.md`；更新相关原有宿主测试。

接口：`GatewayVerification.observe(plan, step_index, idempotency_key) -> VerificationResult` 和对应不可变绑定端口。仅查本地精确审批/实例/计划/步骤账本的已知 Job，不接收 Job ID，不要求或保留旧 grant。未知提交返回 Unknown；无对应账本拒绝且不 POST。

核心动作 `observe_verification`：只允许当前进程 live 的验证 RecoveryRequired，reason 为 verification_status_pending/verification_evidence_pending。调用专用 observe，补充可信证据后进入 E7；仍等待则保持隔离状态。若当前步骤成功且尚有后继，暂停 WaitingApproval，重新显式 execute_plan 后才可继续。重启后的执行不自动恢复。

VerificationResult 增加可选 job_id；runtime 状态提供有界 trace 与 verification 进展，区分 Job 状态、结果证据和后继审批。原问题、grant、幂等执行键、凭据不进入进展 DTO。

- [x] 先验证 Running→Succeeded 单次 POST、证据稍晚到达续查、Unknown 不重交、失效 grant 不妨碍只读观察、跨审批/实例/步骤拒绝。
- [x] 核心测试验证观察不调用 execute、幂等/旧版本、非终态保持、后继重新审批、取消与重启拒绝，观察新增动作缺失失败。
- [x] 实现端口、核心与宿主动作，更新 HTTP 语义变更对应测试；组合动态目录跑新 Job 闭环。
- [x] 产出阶段文档，审查只读续查不会自动提交后继 Job。

## D8：状态与事件重同步、最终兼容验证

文件：修改 `host/store.py`、`tests/test_host_store.py`；新增 `docs/u1-d8-compatibility.md`；由主控修改 `host/runtime.py`/HTTP 组合测试、README 和 task.md。

接口：`DurableStore.get` 的结果增加 `event_high_watermark`，与 state 在同一事务读取；`events` 对裁剪前游标返回 StoreError('cursor_expired')，未来游标返回 cursor_stale。runtime 状态将 event_high_watermark 转为十进制字符串供 UI 读 state 后续订。事件只保留已提交状态变更，不新增自由文本/卡片泄漏。

- [x] 先写裁剪后旧游标拒绝、边界游标接受、状态高水位一致、同会话隔离、未来游标拒绝测试，再实现。
- [x] HTTP 测试验证 410→读取 state/high_watermark→续订无丢失；verify 进展、动态证据与取消组合回归。
- [x] 核心/宿主全量、旧 agent 独立回归、40 项开发评测、文档链接/JSON/命令检查。
- [x] 更新 D6–D8 报告和 task 再规划，明确真实工具、跨进程验证恢复、动态锚点可信生产方、完整功能观察比较和 UI 签收的外部边界。

## 完成记录与后续阶段

2026-10-15：D6–D8 本地交付完成。全量 368 项（362 通过、6 项历史实验跳过），宿主/续查相关 149 项通过，旧 agent 124 项通过，开发评测 40 项通过。独立审查发现的取消 gate、审批包隔离、健康检查及基线瞬断、终态保护问题均已复现、修复和复审。文档检查覆盖 13 个文件、72 个本地链接、3 个 JSON 示例及 10 个 PowerShell 语法块，均通过；源码 compileall 和 diff whitespace 检查通过。

跨进程验证恢复、可信动态目录生产方、完整观察比较器及真实 Gateway/EDA/DeepSeek/Linux 联调按 D8 后续边界继续验收。独立 U1 打包、教学 UI 与发布不包含在本批完成结论。未提交或推送。
