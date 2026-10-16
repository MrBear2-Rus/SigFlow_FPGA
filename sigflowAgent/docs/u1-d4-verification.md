# D4：受控 Gateway VerificationPort

日期：2026-10-15。实现位于 `sigflow_edu_agent/host/verification.py`，只使用 Python 标准库；测试位于 `tests/test_host_verification.py`。

## 接口与可信输入

```python
GatewayVerification(
    config: GatewayConfig,
    journal_path,
    report_resolver,
    *, approval_resolver=None, expected_resolver=None,
    wait_timeout=5.0, poll_interval=0.05,
)
```

- `register_approval(plan, package: ApprovalPackage, gateway_plan_hash: str)`：可信宿主注册完整 UI 交接材料。重新使用 `PlanChecker` 与 `build_plan_card` 构建预期材料，核对完整计划、卡片、版本和格式。注册只保存在内存，不签发授权。
- `bind(plan, gateway_plan_hash)`：为会话固定不可变的完整审批材料。工厂将绑定后的端口交给 Session；后续相同计划的新审批注册不会重定向旧会话的执行、取消或迟到回执处理。
- `approval_resolver(plan) -> ApprovedPlan(package, gateway_plan_hash)`：可选可信宿主审批来源，返回材料仍经过上述核对。
- `authorized(plan, grant_ref) -> bool`：核对真实 active grant、有效期、剩余额度、项目、revision、snapshot、独立 Gateway 审批 hash 与完整步骤。
- `execute(plan, step_index, grant_ref, idempotency_key) -> VerificationResult`：提交受限 Job 或观察已有 Job。幂等键由宿主提供。
- `observe(plan, step_index, idempotency_key)`：D7 新增的专用只读观察路径，不使用旧 grant，不提交 Job；详见 [D7](u1-d7-observation.md)。
- `report_resolver(job_id, *, project_id, revision, snapshot_id) -> Evidence | None`：可信宿主从真实报告和可信锚点获得证据，可构造 `GatewayReportSource` 后调用 `collect`。该回调不能由模型或普通 HTTP 请求提供。
- `expected_resolver(plan, step_index, evidence) -> bool | None`：可选可信观察比较器。未配置时 `matches_expected=None`；工具返回成功不推断预期观察已满足。
- `pending() -> tuple[dict, ...]`：仅当前 Gateway 实例的待核对元数据，包括请求摘要、计划作用域摘要与可能已知的 Job ID，不包含凭据或 grant。
- `cancel_owned(plan) -> tuple[dict, ...]`：仅取消当前计划账本中的已知 Job，返回 `{job_id, status, cancel_requested}`。不接受调用方 Job ID；可由宿主取消 worker 调用。
- `request_cancel(plan)`：仅写磁盘的同步门禁，通过独立 SQLite 连接记录实例/完整计划/UI hash 的取消摘要；不等待执行线程的网络锁，不发 HTTP。取消后相同审批材料不能新建步骤；不自动清除该门禁。
- D7 将本地审批包 digest 纳入记录与取消门禁的精确范围，保留旧门禁并提供受限迁移；相同 UI hash 的不同审批包不能接管既有任务。
- `close()`：关闭 SQLite 连接。

## 已核对的 C++ 边界

核对对象为 `core/src/eda-agent-gateway/GatewayServer.cpp`、`GatewayDto.cpp`、`IdempotencyStore.cpp` 和 `GrantStore.cpp`。

Gateway 接收 UI 提供的非空 `plan_hash` 并原样保存在 grant；当前代码没有统一的计划哈希算法。因此适配器要求独立、明确的 `gateway_plan_hash`，不会用内部 `fingerprint` 或 `ApprovalPackage.digest` 自动代替。前者标识内部计划，后者校验本地完整交接材料，均不产生执行权限。

`GET /api/v1/health` 首先验证实例、`edu.api.v1` 和 edu 版本；随后凭据只能发送到固定 loopback Gateway 的 capabilities、grant、Job 与 report 路由。拒绝重定向，校验响应大小、JSON 重复键、信封版本与类型，不返回服务端原始错误文本。

仅映射：

| 内部能力 | Gateway Job 类型 | provider / plugin_id | 参数映射 |
|---|---|---|---|
| `eda.synth` | `synth` | `yosys` / `eda-synth-yosys` | `top` → `top_module` |
| `eda.sim.build` | `sim.build` | `verilator` / `eda-sim-verilator` | `top` → `top_module`，`testbench` 保留 |
| `eda.sim.run` | `sim.run` | `verilator` / `eda-sim-verilator` | `build_step_id` → 此审批作用域内已成功 build 的 `build_job_id` |

内部 `iverilog` provider 没有已核实的当前 Gateway 插件映射，因此拒绝。`timeout_ms` 没有 `duration_ticks` 转换，当前 Gateway 本身拒绝 `duration_ticks`，故拒绝该计划。模型不能传 URL、文件路径、任意命令、工作目录、环境或自定义 provider。

提交带 `snapshot_id/expected_revision/capability/grant_id/plugin_id/params/plan_hash/step_id`，并原样使用宿主指定的 `Idempotency-Key`。当前 Gateway 消耗 grant、确认工程保存及同步状态、确认 snapshot 内容和实际执行产物；这些检查仍由 Gateway 执行。适配器要求 grant 步骤与完整已审批卡片一致，拒绝 legacy 空步骤 grant。

`GET /jobs/{id}` 不含完整授权 provenance；成功报告再核对 Job、项目、revision、snapshot、能力、插件和状态。报告 complete 且 Evidence 结构、作用域、snapshot、artifact ID/hash 均一致时才能返回证据。报告本身不提供可信源码锚点目录，因此仍需宿主提供可信 resolver。观察不匹配且计划声明停止时，账本阻止后续步骤。

## 持久幂等与失败

SQLite `verification_requests` 单独保存：幂等键摘要、完整请求摘要、审批作用域摘要、内部计划摘要、实例、项目、revision、snapshot、审批 hash、步骤、能力、插件、Job ID、观察状态与取消请求标记。不保存 token、grant ID、完整审批材料、参数、学生问题、源码或证据正文。`synchronous=FULL`，在 POST 前提交唯一预留；同审批作用域和步骤只能预留一次。

- 同键同请求且 Job 已知：只查询原 Job；不会再次 POST。
- 同键请求变化或同一步改用另一键：拒绝冲突。
- POST 连接失败、响应丢失或回应无法验证：保留 `Unknown` 且无已知 Job，不自动重交。
- 预留写入失败：不提交 Job；Job 已接受后的账本写入失败：原 Unknown 预留仍保留，重启不重交。
- Failed、Cancelled、TimedOut、Rejected 终态保持失败；后续步骤拒绝。已知 Job 的新查询或报告无法核实，会降为 Unknown，禁止它继续作为成功 build 依赖。
- 轮询受 `wait_timeout` 约束，单次查询还受 Gateway 网络超时约束。到时返回真实最后状态，例如 Running，并返回空证据和未知观察匹配。不会把该状态伪装成 Job 已超时。报告读取及可信回调耗时不包含在 Job 轮询窗口内。

只读恢复不恢复执行授权，新进程需重新提供可信审批与仍有效的 grant；未知提交的只读核对允许在旧 grant 已失效时完成。`reconcile` 仅接入活跃或未知记录，不允许清除之前已观察到的失败终态。

## 计划范围取消

`cancel_owned(plan)` 选择精确匹配当前 instance、project、revision、snapshot、内部计划摘要、审批 hash、步骤与插件的账本记录，最多处理计划上限的三个 Job。只选择真实 POST 成功后登记或显式核对后登记的已知 Job ID，不从项目列表猜测 Job，不取消手动 Job 或其他计划。旧账本自动增加计划摘要和取消标记列；无法核实计划摘要的旧记录不参与取消。

先完成 instance/protocol/edu 握手，再 `GET /jobs/{id}` 验证实际 project/job_type 与已知记录；Gateway 该公开接口缺少完整 snapshot/plan/grant 绑定，因此原始精确作用域来自本地提交账本，不能声称 GET 本身证明了这些额外字段。已终态 Job 只返回实际状态，不发送取消请求。活跃 Job 才请求 `POST /jobs/{id}/cancel`，严格接受 `job_id` 一致且 `accepted=true` 的响应。返回 `status="CancelRequested", cancel_requested=true` 表示已收到确认，不表示 Job 已终止。

取消 POST 前通过 SQLite 条件更新原子争用未知标记，只有唯一获胜者提交 POST；确认后保存已确认标记。回应丢失或形状无法核实返回 `Unknown/false`，跨适配器并发、重启及重复取消不再次发送 POST，只查询同一已知 Job。`false` 表示没有可信确认，不能推断服务端从未接到请求。明确 `POLICY_DENIED/NOT_FOUND/UNAUTHENTICATED` 返回稳定拒绝码。

`cancel_owned` 即使尚无 Job 行也先持久记录计划取消门禁。新执行预留使用同一 SQLite 原子 INSERT 条件核对该门禁；已经完成预留的请求属于在途请求，迟到的已知 Job 回执会触发取消。回应丢失且 Job ID 未知时继续等待可信 Core provenance，不猜测或重交。该机制阻止 E6 内部阻塞取证返回后新建 Job；不能撤回 Gateway 已接收的请求，也不声明教学取消等同于 Job 已终止。相同计划及 UI hash 的门禁跨重启保留，重新执行需要新的显式审批材料。

取消使用同一锁与整体等待窗口；单次 Job GET/取消 POST 使用窗口剩余时间与网络超时的较小值，初始 health 握手另受 Gateway 配置网络超时约束。宿主应在后台调度取消，可能先等待当前有界执行轮询退出。`close()` 不主动取消；关闭后取消返回空记录且不访问 Gateway，宿主应等待执行/取消 worker 完成后关闭账本。

## 显式核对未知提交

```python
reconcile(plan, step_index, grant_ref, idempotency_key, job_binding)
```

`job_binding` 必须由可信宿主获得实际 Core Job manifest 的 metadata，并补充该真实 record 的 Job ID；不能猜测 ID 或把普通请求正文当 provenance。要求：

- `schema="sigflow.edu-agent-job-binding.v1"`；
- `job_id/instance_id/project_id/revision/snapshot_id/capability/plan_hash/grant_id/step_id`；
- `idempotency_scope="project_id|capability"`、原始 `idempotency_key` 和实际 `request_hash`。

适配器重算当前 Gateway 的受限请求 hash，核对全部绑定，查询实际 Job，并在成功时读取报告后接入账本。Gateway 请求 hash 是当前 C++ `StableDump` 对 `snapshot_id/capability/plugin_id/params` 的 SHA-256；这里所有映射字段是受限 ASCII 标识符，因此与排序键、紧凑 JSON 的序列化一致。它同样不是审批 `plan_hash`。Gateway 请求 hash 没有完整 grant/plan/revision 语义，本地完整请求摘要补齐该绑定。

当前公开 GET Job/项目 Job 列表无法提供以上完整 provenance，因此适配器不实现自动发现或自动猜测核对。已有调用方必须注入可信 Core provenance 来源。

## 验证记录

命令（在 `sigflowAgent` 目录）：

```text
python -m unittest discover -s tests -p test_host_verification.py
```

49 项 loopback HTTP fixture 测试通过。先运行缺失适配器的测试并观察到 15 项明确失败，再实现；后续新增拒绝布尔步骤配额、报告不完整、观察不匹配停止、成功 build 失去核实后拒绝依赖、终态失败保持、核对不得清除失败、取消回应丢失不重发、取消停止后续执行等测试先失败后修复。新增取消接口前，六个取消场景因缺少该接口失败，再实现并验证。独立审查新增回归覆盖空日志路径拒绝、跨重启缺证据/错误报告不得清除 ObservationMismatch、两个适配器并发取消只提交一次、无 Job 时取消门禁持久化、在途 POST 的迟到回执取消，以及旧审批延迟取消不得影响相同计划的新审批会话；均观察到明确失败后修复。

日志仅接受明确文件路径，拒绝空路径、`:memory:` 与 `file:` URI，避免 SQLite 临时或内存数据库破坏防重语义。ObservationMismatch 作为持久策略停止状态，不因刷新 Job 成功、缺锚点或网络错误而清除；成功 Job 不抹除此前已核实的观察不匹配。

覆盖真实 HTTP 请求路径与 payload、合成/构建/运行映射、grant/步骤/参数/provider/snapshot/revision/hash 不匹配、legacy 计划拒绝、握手先于凭据、cross-plan build、失败停止、有界等待、同键重放与冲突、未知回应持久化、显式核对、提交前后真实 SQLite 拒写、重启已知 Job、跨实例拒绝以及证据锚点约束。取消测试涵盖精确计划范围、终态/未知 Job 排除、明确拒绝、错误 Job/非布尔确认、回应丢失持久化、有界 GET、关闭生命周期、旧记录排除，以及停止后续提交。

这些测试明确使用 fixture Gateway 与 fixture Evidence，不运行真实 EDA 工具、不调用 DeepSeek，也不证明真实 UI 审批显示或共享接口联合验收。真实 Gateway、真实 Core manifest、可信锚点与模型验证仍由后续独立联调入口完成。
