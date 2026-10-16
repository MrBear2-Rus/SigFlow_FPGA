# U1 D3/D5：U2 教学 sidecar

日期：2026-10-15。宿主位于 `sigflow_edu_agent.host`，Python >=3.11，仅使用标准库。`agent/` 既有 v1 服务不受修改。基础 bootstrap、ready、health 使用 `edu.api.v1`；教学路由属于独立扩展 `u2.host.v1`，尚非双方签收的共享教学接口。

## 启动与关闭

桌面宿主可将 `SIGFLOW_EDU_AGENT_EXECUTABLE` 指向绝对路径 `sigflowAgent/bin/sigflow-u2-host.cmd`。解释器可通过 `SIGFLOW_EDU_AGENT_PYTHON` 指定；未配置时 Windows 使用 `python`，POSIX 脚本使用 `python3`。启动器固定自身包目录，并使用 Python 3.11 的 `-P` 防止工作目录中的同名包遮蔽教学宿主；环境修改仅作用于子进程。现有 C++ 配置只传入 bootstrap 参数，可信锚点可通过启动 IDE 前配置 `SIGFLOW_U2_ANCHORS` 提供；显式 `--anchors` 优先于环境变量。目录内容必须来自可信宿主。

开发入口（从 `sigflowAgent/` 运行）：

```text
python -m sigflow_edu_agent.host --bootstrap-stdin --anchors trusted-anchors.json
```

stdin 第一行严格采用 [sidecar-bootstrap](../../docs/edu-agent/sigflow/sidecar-bootstrap.md) 的八个字段；不得增加字段。记录为 <=64 KiB、单行、UTF-8 JSON，必须以换行结束。nonce 为 64 个十六进制字符；两个 token 为非空 ASCII 可见字符，最大 4096 字符；instance_id 为 1–128 个字母、数字、下划线或连字符；data_directory 必须为绝对路径。

Gateway 地址仅接受 `http://127.0.0.1:<1–65535端口>`。用户信息、路径（包括尾斜杠）、查询、片段、其他主机、HTTPS 均拒绝。初始化完成并绑定 127.0.0.1 随机端口后，stdout 输出且仅输出一行 ready，包含 type、protocol、原 nonce、port、agent_version。初始化失败退出 2，不输出 ready；stderr 仅输出固定诊断代码。

状态和验证账本分别存放于 `<data_directory>/u2-host/<instance摘要>.sqlite3` 和同名前缀的 `-verification.sqlite3`。token、模型 Key 不写入账本。学生原请求以摘要保存；教学卡中的问题属于教学输出。重启不继承 grant 或旧执行授权。认证的 `POST /api/v1/shutdown` 返回 202，停止接收请求；运行时最多等待 2 秒，验证适配器清理最多额外等待 0.1 秒，未完成线程为 daemon。进程退出结束剩余工作，持久 pending 不自动重交。

## HTTP 边界与路由

所有响应均为 `edu.api.v1` envelope，包含 request_id、trace_id 与 data 或稳定 error。Host 必须为 `127.0.0.1:<本服务端口>`；Origin 缺省允许，出现时必须等于该服务的 HTTP origin。health 允许无凭据，其他路由要求 `Authorization: Bearer <ui_token>`。教学会话路由还要求 `X-Learner-ID`，创建正文 learner_id 必须一致；该 token 代表可信 UI，模型输出没有调用权限。

POST 要求唯一 Content-Length 和 JSON Content-Type，正文最大 64 KiB。重复长度、Transfer-Encoding、Content-Encoding、非 UTF-8、重复 JSON 键、NaN/Infinity/数值溢出、孤立 surrogate、未知字段均拒绝。请求线程默认最多 8 个，教学 worker 默认最多 4 个；每个连接最多等待输入 3 秒并在响应后关闭。HTTP 不接收源码路径、Evidence、checkpoint、provider 配置或执行结果。

| 方法/路由 | 输入与结果 |
| --- | --- |
| GET `/api/v1/health` | 标准 protocol、agent_version、instance_id、ready、model；model.available 表示有效配置，未配置则使用规则回退。 |
| GET `/api/v1/capabilities` | L1–L3 可用、L4 拒绝，extensions 含 `u2.host.v1`。verification.configured 表示适配器已配置，requires_gateway_grant 恒 true。共享 execution.can_run_eda 恒 false，不能据 capability 视为执行授权。 |
| POST `/api/v1/teaching/sessions` | 需要 Idempotency-Key。必需 learner_id/project_id/revision/snapshot_id/job_id/diagnostic_id/issue_id/question；可选 mode=`rule`/`deepseek`、intent=`diagnose`/`explain`。202 返回 session_id 与 replay。验证入口另接受 intent=`verify`、宿主严格解析的 DetailedPlan `plan` 与 UI 提供的 `gateway_plan_hash`。 |
| GET `/api/v1/teaching/sessions/{sid}/state` | 200 返回已提交状态及同事务读取的字符串 event_high_watermark，包括 state_version、cards、evidence_origin、有界 trace 与验证进度。绝不返回原 checkpoint。 |
| GET `/api/v1/teaching/sessions/{sid}/events?after=0&limit=100` | 单值十进制游标，limit 为 1–100。返回 events 与 high_watermark；未来游标或早于保留范围的旧游标返回 410，随后读取 state 的高水位重同步。 |
| POST `/api/v1/teaching/sessions/{sid}/actions` | kind/action_id/expected_version；版本必须为无前导零十进制字符串，范围 0–2^63−1。hint_next/finish/refresh_evidence/cancel/observe_verification；recover 额外且必需 question；execute_plan 可额外提交 grant_ref。其他动作不能包含 question/grant_ref。202 后轮询 state/events；只读观察语义见 D7。 |
| POST `/api/v1/teaching/sessions/{sid}/cancel` | 空 JSON 对象，202；提交教学 Cancelled、隔离卡片，中断 pending。验证会话先建立持久取消门禁，再后台请求取消已知 Job；Job 终态仍由 Gateway 确认。 |
| POST `/api/v1/shutdown` | 空 JSON 对象，202 确认有界关闭。 |

过期状态、幂等冲突、项目/会话忙等为 409；跨 learner、L4、授权拒绝为 403；无认证为 401；无效游标为 400、已失效游标为 410；过大正文为 413、缺 Content-Length 为 411；存储不可用为 503。原始异常、token、模型 Key、绝对路径不通过响应或日志传播。

## 可信锚点与模型

`--anchors` 只能来自宿主管理的本地目录。文件上限 64 KiB，根对象仅含 anchors，最多 256 项；字段对应 EvidenceAnchor，source_ref 对应 SourceRef。示例模板如下，artifact_hash 必须替换为真实 artifact 的 SHA-256，全部范围必须与真实 report 对齐：

```json
{"anchors":[{"evidence_id":"ev-1","project_id":"prj-1","revision":"rev-12","job_id":"job-21","snapshot_id":"snap-12","artifact_id":"artifact-1","artifact_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]}
```

上述仅为字段示例，不是有效工程证据。未配置锚点或匹配失败会进入 WaitingEvidence，不生成虚构证据。默认 evidence_origin=`tool`；fixture 联调必须显式 `--evidence-origin fixture`，卡片保留示例来源。HTTP 客户端不能提交或替换锚点。

配置目录路径时，CLI 和工厂每次取证重新读取严格快照，支持可信生产方补入新 Job 锚点而不重启；目录损坏或撤回不回退旧数据，见 [D6](u1-d6-catalog.md)。更新文件本身不会驱动教学或执行。

DeepSeekProvider 仅从进程环境读取现有 DEEPSEEK_API_KEY/DEEPSEEK_MODEL 配置；缺少配置时使用规则生成。每次 generate（包括修复重试）先调用持久预算预留，失败返回 model_budget_exceeded，网络调用不发生。普通测试不使用真实 Key 或额度。

验证结果解析器只选择与既有可信锚点的 project/revision/job/snapshot 完全相符的单个诊断，再通过 GatewayReportSource 重新读取与校验。新 Job 没有宿主锚点、没有可证实诊断、报告缺失或不完整时，返回证据不足。进程成功不是观察成功，当前没有 expected_resolver 自动证明计划预期。

## 已执行验证与剩余联调

定向命令（工作目录 `sigflowAgent/`）：

```text
python -m unittest discover -s tests -p test_host_http.py
python -m unittest discover -s tests -p test_host_process.py
```

HTTP/工厂/探针定向测试 16 项通过，使用真实 loopback Gateway fixture：创建、L1/L2、幂等重放、版本冲突、作用域、事件、取消、重启只读恢复、缺锚点、严格输入边界、验证 Session 工厂、预算预留、模型文本敏感内容过滤、探针实例核对与脱敏摘要。验证 HTTP 入口提交单个 fixture Job；成功 Job 缺结果锚点时进入可显式续查的 RecoveryRequired，重放不重交。进程定向测试 4 项通过，覆盖正常 ready/health/认证 shutdown、拒绝非法 bootstrap、环境锚点校验、两个 token 不落盘、stdout 单行、Windows 启动器抵御工作目录包遮蔽。D6–D8 新增动态目录、长 Job 观察和组合 HTTP 验证，见 [最新兼容记录](u1-d8-compatibility.md)。

当前 C++ `AgentServiceController` 的启动、ready、health、shutdown 字段与新宿主兼容；源码中未发现调用 teaching 扩展的 AgentPanel/AgentApiClient。因此配置启动器只接通进程管理，不会自动生成教学 UI。旧 `agent/` 的 session/run 教学 DTO 与本扩展不同，调用方须显式选择，不能直接替换业务路由。扩展错误码为小写稳定码，不属于冻结 v1 的大写业务错误枚举。

未执行真实 Gateway/EDA/DeepSeek 请求；Linux/POSIX 启动器尚未在 Linux 执行。完整 L4、真实波形路径、预期观察判定、提交未知后的 Core provenance 对账仍需外部输入和联合验收。动态目录只消费可信生产方的输入，不能为未知新 Job 自动赋予证据。
