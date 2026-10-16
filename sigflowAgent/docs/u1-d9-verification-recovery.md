# U1 D9：验证会话重启后的只读恢复

日期：2026-10-15。范围：Python trusted host；不修改 `readonly-checkpoint.v1`。

## 持久边界

独立 `verification-checkpoint.v1` 上限为 64 KiB，严格检查字段、版本、预算、计划、审批包摘要和作用域。内容包含固定会话绑定、原始请求摘要、计划及 Gateway 审批 hash、审批包标识与摘要、运行 ID、当前执行索引、预算消耗、基线/已完成结果的证据摘要以及最后一次已知 Job 状态。检查点不包含原始 question、模型输出、报告正文、grant 或 token。

状态与检查点使用同一个 SQLite 事务提交。进入每个 E6 步骤前，宿主先原子保存运行位置，并保持动作处于 pending；成功提交动作时继续使用状态版本比较。这样，在 Gateway 收到提交之后、最终宿主状态提交之前退出进程，重启仍会使用原 run ID 和执行索引查询持久验证账本。未登记的执行不会被恢复代码补发。

## 恢复与取消

重启保持 `RecoveryRequired` 和卡片隔离。现有 recover HTTP 动作仍要求原始 question，宿主只在内存中重建请求并对比摘要。恢复使用相同的不可变审批包绑定，重新收集并校验基线，通过确切的 run/plan/index 幂等键 GET 每个已完成步骤，再只读观察当前已登记步骤。恢复不调用 authorized、execute 或模型。

当前 Job 尚未终结、回执未知或成功证据迟到时保持恢复状态；未知回执不猜测 Job ID，也不重提。证据不匹配保留原检查点并允许显式重试。结果通过校验后，后继步骤回到 `WaitingApproval`；只有新的显式 execute_plan 动作及有效 grant 才能继续。没有发出的计划直接恢复到 `WaitingApproval`。已完成步骤的观察不匹配停止事实继续保留。

重启取消不需要原始 question。宿主仅从检查点重建审批绑定与原始 run ID，再使用既有持久账本的 cancel_owned 限定到本运行拥有的已知 Job；不补收未知回执。运行时绑定把取消屏障限定为审批包及 run ID，取消请求只选择精确 `run:plan:index` 键对应的已登记 Job。同一审批包、同一 Gateway hash 被多个运行复用时，其他运行的 Job、后继步骤与新运行均不会继承该取消屏障。重新恢复后会将工厂新建的随机运行标识重新绑定为持久原始 run ID；迟到回执也使用相同运行边界。独立调用 GatewayVerification 的既有计划级取消 API 保持兼容。教学取消与 EDA 终态保持各自语义。

## 验证

运行环境：Windows，项目自身 `sigflowAgent/.venv/Scripts/python.exe`；`PYTHONPATH=sigflowAgent`。

- `-m unittest discover -s sigflowAgent/tests -p test_verification_restart.py`：12 项通过，含真实子进程在回执后直接退出、重新打开两个 SQLite store、只读恢复、后继审批、篡改拒绝、证据迟到、未知回执、取消与版本/索引单调性，以及复用审批包下的运行取消隔离。
- `-m unittest discover -s sigflowAgent/tests -p test_host_verification.py`：49 项通过，保留既有计划级 API、幂等及取消行为。
- `-m unittest discover -s sigflowAgent/tests -p test_verification_observation.py`：19 项通过。

取消隔离修复后以上 80 项通过。修复前另有 host runtime 20 项、host store 25 项回归通过；最终综合测试由主任务汇总。以上均为本地/loopback fixture 测试，不代表真实 EDA 工具或 DeepSeek 闭环验收。真实联调由后续任务覆盖。

## 限制

摘要用于检测绑定和观察变化，不是磁盘攻击者认证机制；宿主数据库及 Gateway 账本属于可信存储。恢复不迁移旧的、没有检查点的验证会话；这类会话仍不能自动恢复执行。审批包必须与原始不可变绑定一致，不兼容的宿主审批格式明确拒绝。新的 grant 不会静默重绑定旧的 build/run 依赖作用域；调用方须提供该账本作用域接受的有效授权，不能把不兼容授权当作成功。
