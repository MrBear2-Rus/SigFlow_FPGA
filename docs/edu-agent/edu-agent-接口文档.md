# SigFlow 教育版 Agent 接口文档（`edu.api.v1`）

> 本文档由 `contracts/edu-agent/v1/`（OpenAPI + JSON Schema，即「spec」）与
> `sigflowAgent/docs/`（实施计划，即「task」）以及 `agent/src/sigflow_edu_agent/`（交付 sidecar）、
> `sigflowAgent/sigflow_edu_agent/`（U2 教学主控设计）、`core/src/eda-agent-gateway/`（C++ Gateway）
> 的源码综合整理而成，供 UI / S1 / U1 / U2 各侧对接口作统一理解。
>
> 真源划分（以真源为准，本文档为索引与导读）：
> - **路由 / 参数 / 状态码 / 错误码 / 信封**：`contracts/edu-agent/v1/agent.openapi.yaml`、`sigflow.openapi.yaml`。
> - **字段与枚举真源**：`contracts/edu-agent/v1/schemas/*.schema.json`。
> - **Agent sidecar 内部 Python 接口（交付真源）**：`agent/src/sigflow_edu_agent/*.py`。
> - **U2 教学主控（设计参考，不进 CMake 构建）**：`sigflowAgent/sigflow_edu_agent/*.py`。
> - **sidecar 启动/就绪握手**：`docs/edu-agent/sigflow/sidecar-bootstrap.md`（本仓随附）。
> - **语义真源**：`docs/edu-agent/sigflow/spec.md`（本仓随附）。

---

## 1. 系统架构与接口总览

```
┌──────────┐   接口 A（ui_token）   ┌──────────────────────┐   接口 B（agent_token）   ┌─────────────────────┐
│   UI     │ ─────────────────────▶ │  教学 Agent sidecar   │ ────────────────────────▶ │   EDA Gateway (C++)  │
│（可信宿主）│ ◀───────────────────── │  （自研 Python，无EDA权限）│ ◀──────────────────────── │  core/eda-agent-gateway │
└──────────┘                        └──────────────────────┘                           └──────────┬──────────┘
      │  签发 grant / receipt（ui_token，走接口 B 的 UI 专用路由）                                        │
      └───────────────────────────────────────────────────────────────────────────────────────────────┘
                                                                                                        ▼
                                                                                              ┌─────────────────────┐
                                                                                              │   EDA Core / 插件   │
                                                                                              │  core/eda-core      │
                                                                                              └─────────────────────┘
```

三条接口层：

| 层 | 双方 | 载体 | 契约真源 |
|---|---|---|---|
| **接口 A** | UI ↔ 教学 Agent | HTTP（loopback） | `agent.openapi.yaml` |
| **接口 B** | Agent ↔ EDA Gateway | HTTP（loopback） | `sigflow.openapi.yaml` |
| **接口 C** | Agent sidecar 内部各模块 | Python（进程内，标准库） | `agent/src/sigflow_edu_agent/*.py` |

核心安全边界（贯穿三个接口）：

- 教学 Agent **不持有 EDA 执行权限**：不运行 EDA 工具、不读写工程文件、不接受客户端提供的路径/脚本/可执行文件/工作目录。综合/仿真只能由 UI 签发的 grant 触发，Agent 只生成待批准的 `PlanCard`。
- 只监听 `127.0.0.1`，Host/Origin 非 loopback → `403 POLICY_DENIED`。
- **任何响应都不得包含本机绝对路径**；证据一律用 opaque ID、行号、能力名与工具报告的原始字段。
- token 只经继承管道/环境变量传入，禁止出现在 URL、日志、卡片、错误消息、工程文件与模型上下文。

---

## 2. 通用约定（接口 A、B 共用）

### 2.1 信封（Envelope）

成功：

```json
{ "schema_version": "edu.api.v1", "request_id": "req-1", "trace_id": "trace-1", "data": { } }
```

失败：

```json
{ "schema_version": "edu.api.v1", "request_id": "req-2", "trace_id": "trace-2",
  "error": { "code": "INVALID_ARGUMENT", "message": "可读短句", "retryable": false, "details": null } }
```

- `schema_version` 恒为 `edu.api.v1`；`request_id`/`trace_id` 为实例内单调 ID（`req-<n>`、`trace-<n>`）。
- 时间戳一律 UTC ISO 8601；所有 64 位数值（`state_version`、`sequence`、`size`、tick 等）用**十进制字符串**承载。
- `details` 当前恒为 `null`；`retryable` 仅在 `503` 时为 `true`。

### 2.2 鉴权

| 身份 | 用于 | 说明 |
|---|---|---|
| `ui_token` | UI → Agent（接口 A 全路由） | 随 sidecar bootstrap 一次性下发，`GET /health` 除外 |
| `agent_token` | Agent → EDA（接口 B 常规路由） | 随受限继承管道下发 |
| UI 专用 token（接口 B） | grant/receipt 签发与撤销 | 独立 token；用 agent_token 或未配置 UI token → `403 POLICY_DENIED`（失败关闭） |

- `GET /health` 是两条 API 唯一无需鉴权的路由。
- Host 必须为 loopback（`127.0.0.1`/`localhost`/`[::1]`，可带端口）；带 Origin 时必须是 loopback 来源。

### 2.3 错误码总表

| 错误码 | 语义 | 典型来源 |
|---|---|---|
| `INVALID_ARGUMENT` | 参数/请求体非法 | 400/415/500 兜底 |
| `UNAUTHENTICATED` | 缺失或无效 Bearer token | 401 |
| `POLICY_DENIED` | 边界/身份/授权策略拒绝 | 403、409（receipt 冲突） |
| `NOT_FOUND` | 资源不存在 | 404 |
| `STALE_REVISION` | revision 不匹配/工程 dirty | 409 |
| `IDEMPOTENCY_CONFLICT` | 同幂等键不同内容 | 409 |
| `RECOVERY_REQUIRED` | 同幂等键同内容仍在飞行 | 409（先查询再重试） |
| `CURSOR_EXPIRED` | 事件游标超出保留窗口 | 410 |
| `ARTIFACT_EXPIRED` | artifact/源文件消失或 hash 不符 | 410 |
| `SOURCE_CHANGED` | 快照构建期间源被改写 | 422 |
| `UNSUPPORTED_MAPPING` | 危险/未知字段或不允许内联 | 422 |
| `CAPABILITY_UNAVAILABLE` | 能力不在白名单/插件未 ready | 422 |
| `RESOURCE_EXHAUSTED` | 请求体/响应/配额超限 | 413/429/403 |
| `MODEL_UNAVAILABLE` | 模型不可用 | Agent 内部 |
| `GATEWAY_UNAVAILABLE` | Gateway 不可达/拒绝 | 503 |
| `SERVICE_UNAVAILABLE` | 服务/存储/Job 服务不可用 | 503（唯一 retryable=true） |

> 实现回传但**不在** `envelope.schema.json` 枚举内的三个码（不依赖）：`PATH_OUTSIDE_PROJECT`、`SOURCE_UNAVAILABLE`、`PLUGIN_UNAVAILABLE`。

### 2.4 ID 约定

| 实体 | 形态 | 作用域 |
|---|---|---|
| project | `project-<24 hex>` | 工程根规范化 ID |
| source | `source-<24 hex>`（= sha256(相对路径) 前 24 位） | **非工程作用域**，不可当全局唯一键 |
| session | `sess-<n>` | Agent 实例内 |
| run | `run-<n>` | Agent 实例内 |
| event | `ev-<n>` | Agent 实例内 |
| grant | `grant-<n>` | Gateway |
| receipt | `ui-receipt-<n>` | Gateway |
| job / artifact | opaque（Job 服务 / 宿主登记） | Gateway |

### 2.5 统一限额

| 项 | 值 |
|---|---|
| 请求体上限 | 1 MiB（超限 413 → `RESOURCE_EXHAUSTED`） |
| `selection.text` | 64 KiB |
| 集合分页 | 默认 100 / 最多 500（jobs、events）；波形信号默认 256 / 最多 4096 |
| 事件保留 | 最新 10,000 条且 ≤ 24h |
| 长轮询 `wait_ms` | 默认 20000 / 上限 25000 ms |
| 响应护栏 | 普通响应 2 MiB（超出 429，不截断 JSON） |
| grant TTL / max_jobs | 600 s / 3 |
| receipt TTL | 900 s（一次性核销） |
| 单次运行卡片 | 16 张（超出记 `omitted`） |
| 会话 | 32 个（LRU 淘汰） |

---

## 3. sidecar 启动/就绪握手（bootstrap）

实现真源：`agent/src/sigflow_edu_agent/__main__.py`、`bootstrap.py`；语义真源 `docs/edu-agent/sigflow/sidecar-bootstrap.md`（本仓随附）。宿主控制器（`AgentServiceController`）与 sidecar 经 stdin/stdout 各一行 JSON 握手，再验证 health 才开放业务：

1. 宿主向 sidecar **stdin 写一行 UTF-8 JSON**（≤ 64 KiB），`type`、`protocol` 与 6 个文本字段全部必填非空：

   ```json
   { "type": "sigflow-bootstrap", "protocol": "edu.api.v1",
     "instance_id": "...", "nonce": "...",
     "gateway_url": "http://127.0.0.1:<port>",
     "gateway_token": "...", "ui_token": "...", "data_directory": "..." }
   ```

   - `type` 必须为 `sigflow-bootstrap`，`protocol` 必须为 `edu.api.v1`，`gateway_url` 必须为 http(s)。
   - `gateway_token` / `ui_token` / `data_directory` 为秘密值：sidecar 只在进程内存保存，`__repr__` 不打印，并立即挂 `RedactionFilter` 对后续所有日志脱敏。

2. sidecar **先绑定 `127.0.0.1`，再宣布就绪**：bootstrap 行非法 → 退出码 `2`；绑定失败 → 退出码 `3`。两种情况 **stdout 均保持为空**（宿主把 ready 之前的任何 stdout 字节视为协议违例）。

3. sidecar 在 **stdout 写恰好一行 JSON ready 并 flush**，此后 stdout 不再写任何内容（诊断一律走 stderr）：

   ```json
   { "type": "ready", "protocol": "edu.api.v1", "nonce": "...", "port": 54321, "agent_version": "0.1.0" }
   ```

   - `nonce` 回显 bootstrap 的 nonce，宿主据此配对；`port` 为实际绑定端口（`--port 0` 时为系统分配）。

4. sidecar 服务直到收到 SIGTERM / SIGINT 或鉴权通过的 `POST /api/v1/shutdown`；正常退出码 `0`。

退出码速查：`0` 正常、`2` bootstrap 错误、`3` 绑定错误、`7`/`9` 仅测试故障注入。

CLI 参数：`--bootstrap-stdin`（必需）、`--port`（默认 `0`，系统分配）、`--agent-version`（覆盖版本号）、`--selftest-fail`、`--selftest-ready-delay-ms`。测试专用故障注入（不属于冻结契约，生产禁用）：`--selftest-fail` ∈ `exit-before-ready`(退 7) / `exit-after-ready`(退 9) / `wrong-nonce` / `wrong-protocol` / `hang` / `no-ready`，供宿主压测自身的重启/配对/超时逻辑。

---

## 4. 接口 A：UI ↔ 教学 Agent（自研 sidecar）

实现真源：`agent/src/sigflow_edu_agent/`（交付 sidecar）。协议 `edu.api.v1`。

> 注意：`sigflowAgent/sigflow_edu_agent/` 是 U2 教学主控的**设计参考代码**（含 E0–E7 状态机、`TeachingLoop`、DeepSeek 适配），**不是**本接口的实现真源，两者模块名与类型不互通。

### 4.1 端点一览

| 方法 | 路径 | 鉴权 | 摘要 |
|---|---|---|---|
| GET | `/health` | 无 | 存活、协议、包版本、模型可用性 |
| GET | `/capabilities` | ui | 模式 L1–L4 与能力就绪状态 |
| POST | `/sessions` | ui | 创建教学会话（绑定 project + revision） |
| GET | `/sessions/{sessionId}` | ui | 会话摘要 |
| POST | `/sessions/{sessionId}/runs` | ui | 发起教学运行并取回卡片 |
| GET | `/sessions/{sessionId}/events` | ui | 会话事件流（立即返回，无长轮询） |
| GET | `/runs/{runId}` | ui | 读取一次运行结果 |
| POST | `/shutdown` | ui | 请求安全关闭 sidecar |

### 4.2 关键语义

- **会话**：只存内存，最多 32 个（LRU）。创建时 `state_version="1"`。
- **运行 `kind`**：`explain` / `hint` / `plan` / `report_review`。`level` 缺省 `L1`，取值 `L1–L4`。
  - `explain`/`hint`：在 `selection.text` 上跑确定性规则引擎；命中锁存器/敏感列表/位宽/结构规则时返回 `source:"rule"` 的 TeachingCard。
  - `plan`：返回 `PlanCard`；只用 `eda.synth`/`eda.sim.build`/`eda.sim.run`，`requires_approval=true`，不含路径/可执行文件/脚本/工作目录。
  - `report_review`：**必须**带 `job_id`；Agent 用 gateway_token 调 Gateway 的 `GET /api/v1/jobs/{job_id}/report`，把 `diagnostics` 映射为 `diagnostic_explanation` 卡片。`diagnostics` 为空时，仅在 `completeness∈{complete,exact}` 且 `origin≠legacy` 才解释为“无诊断”，否则返回证据不足卡片，绝不宣称“电路没问题”。
- **L4 可信门控**：基础 `/runs` 的 L4 恒 403，`more=true` 不构成授权。协商教学扩展先逐级到 L3，再 reference_request + 原生 UI 独立签发 receipt + reference_confirm 全绑定核销；第一步不含参考内容。
- **模型降级**：未配置模型 Key 时 `model_used=false`、`state=degraded`，被跳过的模型步骤记入 `omitted`，绝不伪造模型回答。
- **事件**：至少一次投递，消费者按 `event_id` 去重；`after` 为会话内 sequence 游标；每会话保留最新 1000 条，游标过期 → `410 CURSOR_EXPIRED`。

### 4.3 运行模型（sidecar 无 E0–E7 状态机）

sidecar 是**无状态、按 run 一次性调度**：每次 `POST /runs` 由 `RunDispatcher` 按 `kind` 直接产出卡片，`run.state ∈ {succeeded, degraded, failed}`。没有 E0–E7 阶段、没有 WaitingStudent / WaitingApproval 挂起态、没有 `hint_next` / `execute_plan` 之类的宿主动作；L4 的“两步放行”由请求里的 `more` 布尔位实现（见 §4.2），而非会话阶段机。

> E0–E7 状态机（`Status` / `TeachingLoop.act()` / `host_action` / `grant_ref`）属于 `sigflowAgent/sigflow_edu_agent/`（U2 教学主控）的设计参考，sidecar 未实现该运行时。

### 4.4 主要响应结构（数据模型索引见 §7）

- `HealthData`：`{protocol, agent_version, instance_id, ready, model:{available, provider, reason}}`
- `CapabilitiesData`：`{protocol, agent_version, modes[], capabilities[], model, course, execution}`
- `RunData`：`{run_id, session_id, state, state_version, cards[], omitted[], model_used}`
- `Selection`：`{node_id, source_id, start_line, end_line, text}`（`text ≤ 64 KiB`，不得携带路径）

---

## 5. 接口 B：Agent ↔ EDA Gateway（C++，交付项 SF-01）

实现真源：`core/src/eda-agent-gateway/`。协议 `edu.api.v1`，只监听 `127.0.0.1`，端口由系统分配并经受限继承管道告知 Python sidecar。

### 5.1 端点一览

| 方法 | 路径 | 鉴权 | 摘要 |
|---|---|---|---|
| GET | `/health` | 无 | 实例存活、协议主版本、edition、build |
| GET | `/capabilities` | agent | 教育能力白名单就绪状态 |
| GET | `/projects/{projectId}/context` | agent | 已保存工程上下文 DTO（revision/dirty/sources） |
| GET | `/projects/{projectId}/state` | agent | 一致性快照（事件水位 + active_jobs + capabilities） |
| GET | `/projects/{projectId}/events` | agent | 工程事件流（分页 + 长轮询 + 游标） |
| POST | `/projects/{projectId}/snapshots` | agent | 提交不可变 EDA 输入快照 |
| GET | `/projects/{projectId}/sources/{sourceId}` | agent | 读源文件有界行区间 + SourceRef |
| GET | `/projects/{projectId}/design/nodes/{nodeId}` | agent | 只读局部设计证据（图元 → 源码映射） |
| POST | `/projects/{projectId}/context/query` | agent | 按需裁剪的局部证据包（`max_bytes` 预算） |
| POST | `/projects/{projectId}/grants` | **UI** | 签发执行授权 grant |
| GET | `/grants/{grantId}` | agent | 查询 grant 状态 |
| POST | `/grants/{grantId}/revoke` | **UI** | 撤销 grant（幂等） |
| POST | `/projects/{projectId}/ui-receipts` | **UI** | 签发 UI 凭据 receipt |
| GET | `/ui-receipts/{receiptId}` | agent | 读 receipt 状态（不返回签发摘要） |
| POST | `/ui-receipts/{receiptId}/consume` | agent | 原子核销 receipt（一次性） |
| POST | `/projects/{projectId}/jobs` | agent | 在快照上提交受 grant 约束的 Job（需 `Idempotency-Key`） |
| GET | `/projects/{projectId}/jobs` | agent | 列出本 Gateway 提交的 Agent Job |
| GET | `/jobs/{jobId}` | agent | 查询 Job 状态 |
| GET | `/jobs/{jobId}/report` | agent | 规范报告视图 `edu.jobreport.v1` |
| POST | `/jobs/{jobId}/cancel` | agent | 取消 Job |
| GET | `/projects/{projectId}/legacy/jobs` | agent | 旧 main/jobs 历史（只读桥） |
| GET | `/projects/{projectId}/legacy/jobs/{jobId}/report` | agent | 旧 Job 报告归一化视图 |
| GET | `/waves/{artifactId}/signals` | agent | 波形信号列表（分页） |
| POST | `/waves/{artifactId}/query` | agent | 精确区间跳变查询 |
| GET | `/artifacts/{artifactId}` | agent | artifact 元数据（不含本机路径） |
| GET | `/artifacts/{artifactId}/content` | agent | artifact 有界字节内容（256 KiB 窗口） |

### 5.2 核心概念

**Snapshot（快照）** —— 不可变 EDA 输入，只接受已保存（`dirty=false`）且图码同步（`synchronized=true`）的工程；`expected_revision` 必须等于当前 revision。构建期间变化 → 409/422。快照内路径为快照相对 POSIX 路径。

**Grant（授权）** —— UI 签发的执行授权，绑定 `plan_hash`（必填）+ revision + snapshot。`ttl_seconds`（默认 600）、`max_jobs`（默认 3）。Agent 只能读自身 grant，签发/撤销仅 UI 身份。状态 `active|revoked|expired`。

**Receipt（凭据）** —— UI 签发的教学凭据，`level ∈ {hint, l4, teaching}`，TTL 900 s。签发时 `state_version=0`，首次核销必须传 0。核销一次性：同 action 重放幂等（`replayed=true`），不同 action → 409。重启后 active 恢复为 invalid。

**Job（任务）** —— 在快照上提交、受 grant 约束。请求体字段白名单固定 `snapshot_id/expected_revision/capability/grant_id/plugin_id/params`。参数策略：

| capability | 允许的 params 键 |
|---|---|
| `eda.sim.build` / `eda.synth` | `top_module`, `strategy`, `opts`（字符串数组） |
| `eda.sim.run` | `top_module`, `testbench`, `duration_ticks`（十进制字符串）, `build_job_id` |

危险/未知字段（`executable`/`command`/`args`/`script`/`work_dir`/`cwd`/`output`/`env` 等）→ 422 `UNSUPPORTED_MAPPING`。9 态：`Created`/`Validating`/`Queued`/`Running`/`ValidatingArtifact`/`Succeeded`/`Failed`/`Cancelled`/`TimedOut`。

**幂等** —— `Idempotency-Key` 头仅对 `POST /jobs` 必填，作用域 project+capability，请求 hash 覆盖 snapshot/capability/plugin/params（**不含** expected_revision 与 grant_id）。

**Report（报告）** —— 归一化 `edu.jobreport.v1`：9 态映射、`diagnostics[]`、`metrics`（缺失用 null+reason，不填 0）、`artifacts[]`（只给 `artifact_id`+`sha256`+`role`）、`completeness`。结构化本机路径被替换为文件名并标记 `path_redacted`/`file_redacted`。

**Legacy（旧历史）** —— 只读桥，恒 `origin="legacy"`、`completeness="legacy_unverified"`，旧 `parameters`（含本机绝对路径）永不外泄。

**能力白名单**：`eda.synth`、`eda.sim.build`、`eda.sim.run`（ready 依插件真实状态）；`eda.pnr`/`eda.pack`/`eda.flash`/`eda.debug` 恒 `disabled=true`。

### 5.3 边界约束

- `POST /jobs/{jobId}/cancel` 不带工程参数，跨工程取消在协议上不可表达。
- 波形查询入参 `start_tick`/`end_tick` 为 JSON 无符号整数，响应 tick 为十进制字符串。
- `GET /artifacts/{id}`、`/content` 为全局（非工程作用域）路由，只认 opaque artifact ID。

---

## 6. 接口 C：Agent sidecar 内部 Python 接口（进程内，非 HTTP）

实现真源：`agent/src/sigflow_edu_agent/`（仅标准库；包声明 Python >=3.9，本轮实测 3.13.7，其他版本待矩阵验证）。sidecar 自包含，不 import `sigflowAgent/`。模型输出不能实现接口或取得工具权限。

### 6.1 启动配置（`bootstrap.py`）

```python
@dataclass(frozen=True)
class Bootstrap:        # instance_id, nonce, gateway_url,
                        # gateway_token/ui_token/data_directory（repr=False 秘密值）
    def secrets(self) -> tuple[str, ...]: ...   # → (gateway_token, ui_token)

def read_from_stream(stream, limit=64*1024) -> Bootstrap: ...   # 读 stdin 一行并校验
class BootstrapError(Exception): ...            # .reason
```

- 校验：`type=="sigflow-bootstrap"`、`protocol=="edu.api.v1"`、6 个文本字段非空字符串、`gateway_url` 必须 http(s)。行 > 64 KiB 或非法 UTF-8 / JSON → `BootstrapError`。

### 6.2 HTTP 服务（`http_server.py`）

```python
@dataclass
class ServerConfig:     # instance_id, ui_token, agent_version, host="127.0.0.1",
                        # port=0, gateway, model, secrets, stop_event
class EduAgentState:    # SessionStore + RunDispatcher + 请求/追踪计数器
    def next_ids(self) -> tuple[str, str]: ...   # → ("req-<n>", "trace-<n>")
class EduAgentServer(ThreadingHTTPServer): ...
def create_server(state) -> EduAgentServer: ...  # 绑定 loopback
```

- 硬限制：请求体 1 MiB → 413；`selection.text` 64 KiB → 400；每 run 16 卡；会话 32 LRU。
- 鉴权：除 `/health` 外一律要求 `Authorization: Bearer <ui_token>`（`hmac.compare_digest` 常量时间比较）；Host 非 loopback → 403 `POLICY_DENIED`。

### 6.3 调度（`dispatch.py`）—— 策略层

```python
@dataclass(frozen=True)
class RunRequest:       # kind, level, selection, job_id=None, more=False, top, revision
@dataclass
class Outcome:          # state, cards, omitted, model_used, error_code, error_message
class RunDispatcher:
    def dispatch(self, request, state_version) -> Outcome: ...   # 按 kind 分派
    def model_status(self) -> ModelStatus: ...
```

- `kind` 分派：`explain` / `hint` / `plan` / `report_review`；`run.state ∈ {succeeded, degraded, failed}`。
- `safe_teaching_answer(answer, level)`：模型文本失败关闭——空 / 超长(>12 KiB) / 含本机路径 / 含代码标记（``` 、`endmodule`、`always @`/`always_comb`/`always_ff`）→ 整体降级为规则讲解，不就地改写。
- `sufficient_report(report) -> (trustworthy, reasons)`：空 `diagnostics` 仅当 `completeness∈{complete,exact}`、`origin∈{core,gateway}`（`legacy` 拒绝）、`revision` 非空时才可解释为“无诊断”。

### 6.4 卡片构建（`cards.py`）

卡片 kind：`rule` / `teaching` / `reference_solution` / `diagnostic_explanation`。

```python
@dataclass
class RenderedCards:    # cards, omitted, model_used
def cap_cards(cards, omitted=()) -> RenderedCards: ...   # 截断到 16 张并记 omitted
def build_rule_card(...) / build_teaching_card(...) / build_diagnostic_card(...) /
    build_insufficient_evidence_card(...) / build_reference_solution_card(...)   # 仅 L4
@dataclass(frozen=True)
class PlanStep:         # step_id, capability, description, params, depends_on
def build_plan_card(plan_id, goal, steps, revision, state_version, stop_conditions, max_jobs): ...
def validate_plan_card(card): ...
```

- 构建器为纯函数（不改输入）；卡片 `state_version` 恒为十进制字符串；不含本机绝对路径 / 可执行文件名 / 工作目录。
- `validate_plan_card` 强制 `snapshot_required=true`、`requires_approval=true`、能力 ∈ `eda.synth`/`eda.sim.build`/`eda.sim.run`、params 无危险键（`path/executable/command/args/script/work_dir/cwd/env/shell/output...`）。

### 6.5 规则引擎（`rules.py`）

```python
@dataclass(frozen=True)
class Selection:        # text, node_id, source_id, start_line, end_line
@dataclass(frozen=True)
class RuleFinding:      # issue_id, rule, title, body, signal, line_hint, detail
def analyse(selection) -> list[RuleFinding]: ...   # 纯函数、无模型、无 IO
```

- 规则：`RTL_LATCH_INCOMPLETE_BRANCH`（组合块某信号未覆盖所有分支 → 推断锁存器）、`RTL_SENSITIVITY_INCOMPLETE`（敏感列表缺读取信号）、`RTL_WIDTH_MISMATCH`（常量位宽 > 目标位宽）、`RTL_BLOCK_UNBALANCED`（`begin`/`end` 不匹配）。

### 6.6 会话存储（`session.py`）

```python
@dataclass
class SessionRecord:    # session_id, project_id, revision, top, target, locale,
                        # state_version, created_at, runs, events, sequence
@dataclass
class RunRecord:        # run_id, session_id, kind, level, state, state_version,
                        # cards, omitted, model_used, error_code, error_message
class SessionStore:     # RLock 保护
    def create_session(...) / get_session(...) / create_run(...) / get_run(...)
    def append_event(...) / read_events(after, limit=100): ...
```

- 32 会话 LRU；每会话最多 64 个 run、1000 条事件（超限裁最旧，游标越界 → `410 CURSOR_EXPIRED`）；`create_run` 使 `state_version+1`。

### 6.7 Gateway 客户端（`gateway_client.py`）

```python
@dataclass(frozen=True)
class GatewayOutcome:   # ok, report=None, code=None, message
class GatewayClient:
    def __init__(self, base_url, token, timeout=8.0, secrets=()): ...
    def fetch_job_report(self, job_id) -> GatewayOutcome: ...   # GET /api/v1/jobs/{id}/report
```

- sidecar **唯一工程/EDA 通道**；支持受限读取、已审批 Job 提交/查询/取消及 receipt 核销，不签发 grant/receipt。可选模型是另一出站客户端。禁代理/重定向、无自动 POST 重试、token 只进 header；opaque ID 严格校验，响应 2 MiB 上限；冻结领域错误保留，其余归为 GATEWAY_UNAVAILABLE。

### 6.8 模型客户端（`model_client.py`）

```python
@dataclass(frozen=True)
class ModelStatus:      # available, provider, reason, model
@dataclass(frozen=True)
class ModelAnswer:      # ok, text, reason
class ModelClient:
    def complete(self, instruction, evidence) -> ModelAnswer: ...
```

- OpenAI-compatible `/chat/completions`，非流式，30 s 超时，响应 ≤ 256 KiB。
- 环境变量：`SIGFLOW_EDU_MODEL_API_KEY` / `SIGFLOW_EDU_MODEL_BASE_URL`（默认 `https://api.openai.com/v1`）/ `SIGFLOW_EDU_MODEL_NAME`（默认 `gpt-4o-mini`）。
- 无 Key → `available=false` 降级规则讲解；返回文本含本机路径或敏感凭据 → 整体拒绝。

### 6.9 错误与信封（`errors.py`）

```python
ERROR_CODES = (INVALID_ARGUMENT, UNAUTHENTICATED, POLICY_DENIED, NOT_FOUND,
               STALE_REVISION, IDEMPOTENCY_CONFLICT, CURSOR_EXPIRED, RESOURCE_EXHAUSTED,
               SERVICE_UNAVAILABLE, MODEL_UNAVAILABLE, GATEWAY_UNAVAILABLE, CAPABILITY_UNAVAILABLE)
class AgentError(Exception):    # code, message, retryable, http_status
def success_envelope(request_id, trace_id, data) / failure_envelope(...): ...
```

- sidecar 只会回传这 12 个错误码；`failure_envelope` 的 `details` 恒为 `null`。

### 6.10 共享工具与常量（`util.py` / `version.py`）

- `util.py`：`decimal`（64 位计数 → 十进制字符串）、`iso_utc`、`contains_absolute_path`、`redact`、`sanitize_message`、`RedactionFilter`（bootstrap 后立即挂到 logging 防泄漏）、`json_dumps`、`env_flag`；`REDACTED="***redacted***"`。
- `version.py`：`AGENT_VERSION="0.1.0"`、`PROTOCOL="edu.api.v1"`、`BOOTSTRAP_TYPE="sigflow-bootstrap"`、`READY_TYPE="ready"`、`PLAN_CAPABILITIES`(3 能力)、`LEVELS`(L1–L4)、`RUN_KINDS`(4)、`CARD_KINDS`(4)、`MAX_CARD_BYTES=64 KiB`、`MAX_CARDS_PER_RUN=16`、`MAX_SESSIONS=32`、`MAX_EVENTS_PER_SESSION=1000`。

> **与 U2 教学主控（`sigflowAgent/sigflow_edu_agent/`）的关系**：后者是设计参考（`TeachingLoop` / `TeachingSession` / E0–E7 / `ports.py` 的 `EvidenceSource`/`CardProvider`/`VerificationPort` / DeepSeek `deepseek.py` / Gateway `clients/gateway.py` / 上下文 `context/*`），不进 CMake 构建；sidecar 把同一套教学目标重写为“按 run 一次性调度”的精简实现，二者类型不互通。

---

## 7. 数据模型索引（`contracts/edu-agent/v1/schemas/`）

| Schema 文件 | 用途 |
|---|---|
| `envelope.schema.json` | 成功/失败信封 + 错误码枚举（真源） |
| `agent-health.schema.json` | Agent `/health` 响应 |
| `agent-capabilities.schema.json` | Agent `/capabilities` 响应 |
| `agent-session.schema.json` | Agent 会话摘要 |
| `agent-run.schema.json` | Agent 运行结果（cards/omitted/model_used） |
| `agent-event.schema.json` | Agent 会话事件 |
| `capabilities.schema.json` | Gateway 能力清单 |
| `project-context.schema.json` | 工程上下文 DTO |
| `project-state.schema.json` | 工程一致性快照 |
| `snapshot.schema.json` | 不可变输入快照 |
| `source-ref.schema.json` | 源码引用（行号/文件 hash） |
| `design-node.schema.json` | 局部设计图元证据 |
| `context-query.schema.json` | 按需裁剪证据包 |
| `event.schema.json` | Gateway 工程事件 |
| `grant.schema.json` | 执行授权 grant |
| `ui-receipt.schema.json` | UI 教学凭据 receipt |
| `job-report-view.schema.json` | 规范报告视图（`edu.jobreport.v1`） |
| `diagnostic.schema.json` | 诊断项 |
| `legacy-job.schema.json` | 旧 Job 历史 |
| `teaching-card.schema.json` | 教学卡片（Agent 侧） |
| `plan-card.schema.json` | 计划卡片（Agent 侧） |
| `wave-signals.schema.json` | 波形信号列表 |
| `wave-query.schema.json` | 波形跳变查询 |

---

## 8. 安全与边界约定（汇总）

1. **token 管理**：只经继承管道/环境变量；禁止进入 URL、日志、卡片、错误消息、工程文件、模型上下文。
2. **路径脱敏**：任何响应不含本机绝对路径；结构化路径替换为文件名并标记 `path_redacted`/`file_redacted`（已知残留风险：工具自由文本 `diagnostics[].summary` 可能自带路径，需另立任务做文本级裁剪）。
3. **危险字段白名单**：Job `params` 白名单见 §5.2，多出/危险字段 → 422，不静默剥离。
4. **身份分离**：UI 专用 token 仅用于 grant/receipt 签发撤销；Agent token 无法执行这些路由（失败关闭）。
5. **幂等**：`Idempotency-Key` + 请求 hash；同键不同内容 → 409，同键同内容飞行中 → `RECOVERY_REQUIRED`。
6. **模型输出边界**：模型只生成候选卡片，不能控制阶段、授权、提示级或执行工具；确定性结构检查不构成对提示注入/答案泄露的完整语义保障。

---

## 9. 已知偏差与限制（v1）

- 事件无长轮询（接口 A）；会话/运行/事件在内存，重启失效。扩展有 SQLite 最小学习元数据/恢复检查点，默认不收集学习数据，不自动恢复会话或续跑。
- 无课程库：`course_refs` 恒为空数组，`/capabilities.course.available=false`。
- 无波形内联读取：证据只引用 opaque artifact/report 字段。
- L4 只返回“参考解要点”清单，不代写工程文件。
- `--selftest-fail` / `--selftest-ready-delay-ms` 为仅供测试的故障注入开关，不属于冻结契约。
- 实现与 schema 的若干不一致（如 `source-ref.end_line` 空区间返回 0、波形入参 tick 不接受字符串等）详见 `sigflow.openapi.yaml` 的 info.description「已知实现与 schema/决策的偏差」。
