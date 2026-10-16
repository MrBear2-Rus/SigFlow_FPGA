# D2：教学核心宿主

日期：2026-10-15。依赖 D1 DurableStore；实现位于 host/runtime.py。

## 行为

Runtime 创建固定实例/项目/学习者/版本/Job 的会话，复用 TeachingSession 驱动 E0–E7。请求由受信 UI 发起，模型没有动作接口。每个项目最多一个非终态教学会话；最多四个后台 worker。创建键在实例内生成稳定 session_id，同键不同请求拒绝。

创建会话和初始动作预留在同一事务中提交；其他动作执行前持久保留 action_id/指纹/expected_version。候选状态只在事务提交后通过 snapshot 或重放返回。hint_next 每次最多升一级，L4、shell 和未知动作拒绝。HTTP 适配见 D3，Runtime 不实现监听。

取消直接提交教学 Cancelled、清空卡片并中断 pending，独立 `/cancel` 不受动作容量或客户端键冲突影响；普通 cancel 动作保留版本与动作预留检查，随后复用同一取消路径。重复动作在恢复/取消期间只能返回隔离后的持久状态，不能重新暴露历史卡片。阻塞读取/模型调用可能继续，但不能发布迟到结果。

验证入口接受严格 DetailedPlan、独立 UI 审批 hash 与 execute_plan 动作中的 grant_ref。共享能力描述不构成执行授权。验证端口在每次 authorized/execute 前核对会话取消标记，避免 E6 阶段内部阻塞取证返回后继续提交。取消先同步调用适配器的磁盘 request_cancel 门禁，再提交教学取消，并后台调用 cancel_owned 处理本计划已知 Job；取消确认不表示 Job 已终止。已在途提交的迟到回执由 D4 继续取消，未知 Job ID 不猜测。非终态/Unknown 结果进入 RecoveryRequired，禁止自动执行下一步骤或重交。

只读暂停可生成既有检查点。重启后卡片隔离为 RecoveryRequired，恢复需再次提供原问题并匹配仅存储的摘要，然后重新读取证据；不会重新调用旧模型请求。恢复失败保留原检查点，可使用新动作键显式重试。可信持久版本作为 restore_readonly_session 的下界，避免恢复失败后再重启产生版本错位。恢复动作与提示动作均按键重放。处理中崩溃、缺检查点或真实执行授权不自动恢复。

## 验证

在 sigflowAgent 执行：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_host_runtime.py -v
```

首轮 6 项因 Runtime 缺失而失败；实现及独立审查修复后 20 项通过。覆盖真实 TeachingLoop 的 L1/L2/L3、动作重放、同项目限制、范围/版本拒绝、恢复卡片隔离、失败恢复重试、创建事务故障回滚、持久模型预算、取消竞态、动作容量冲突、验证状态挂起及 E6 取证期间取消后不得提交 Job。所有证据是明确 fixture，不代表真实 EDA 或真实模型验收。

## 再规划

D3 使用 create/action/recover/cancel/snapshot/events 作为单一服务边界，不直接读取后台 Session 的候选状态。D4 将独立核对 C++ grant/Job 契约后接入验证意图；只读检查点不扩大为自动执行恢复。磁盘错误与 pending 不确定性继续保守处理。
