# edu-agent v1 契约变更记录

> 2026-10-07 架构说明：教学 Agent 改为自行实现，只参考 UCAgent 架构；本目录仍是 SigFlow ↔ Agent 的机器契约，不绑定任何 Agent 框架。下文较早日期的名称保留为当时记录，不代表当前依赖。参见 [自研决策](../../../docs/edu-agent/DECISION-2026-10-07.md)。

> 字段与枚举真源：本目录 `schemas/*.schema.json`（W1 冻结后）。
> 语义与边界真源：`docs/edu-agent/sigflow/spec.md §5`。
> 协作与变更流程真源：`docs/edu-agent/Cooperate.md §8`。

## v1（草稿，W1 冻结）

初始字段（来自 `spec.md §5.4` 共同 DTO 与 §5.1 通用约定）：

- `SourceRef`、`Diagnostic`、`JobReportView`、`envelope`（成功/失败）、`capabilities`。
- 能力白名单：`eda.sim.build`、`eda.sim.run`、`eda.synth`；`eda.pnr/pack/flash/debug` = disabled。
- 错误码枚举见 `schemas/envelope.schema.json`。
- 64 位 tick/uid 用十进制字符串；路径为工程相对 POSIX 或 opaque ID。

### 未落定（W1 决策）

- `TeachingCard` / `PlanCard` / `Event` / `LearningOutcome` schema（Agent 侧字段，S1/U1 联合冻结）。
- `projects/{p}/context`、`snapshots`、`waves/*` 的 data 载荷 schema。
- HTTP 库选型（MinGW/Linux 最小样例验证后固定）。→ **已定：cpp-httplib v0.18.3（单头 MIT，随仓库 `3rd/httplib/httplib.h` 分发）**。

### 已落地

- `sigflow.openapi.yaml`：已冻结 `GET /health`、`GET /capabilities`、`GET /projects/{p}/context`、`POST /projects/{p}/snapshots`。
- `schemas/`：新增 `project-context` 与 `snapshot`，固定 opaque project/source ID、十进制字符串大小、相对 POSIX 路径及 dirty/synchronized 语义。
- `fixtures/golden.json`：新增已保存 context 和 snapshot golden；由 `tests/agent/edu_contract_smoke` 校验。

### 2026-09-26：SF-05 revision/snapshot

- 问题：Gateway 已能生成工程上下文和快照，但 wire schema 尚未冻结，UCAgent 无法据此生成客户端模型。
- 新语义：`revision=rev-<单调序号>-<设计指纹前缀>`；同一工程输入重复观察不增长，已保存设计输入变化时增长。`dirty=true` 或 `synchronized=false` 的 context 不可创建快照。
- 一致性：快照提交会复核 DTO 构建时的每个 source hash 与 `sigflow.project` hash；IDE 外部修改尚未触发 UI revision 刷新时返回 `422 SOURCE_CHANGED`。
- 路径：快照只暴露工程/快照相对路径；工程外 include 复制到 `external/<source_id>/<filename>`，不泄露绝对路径。
- 兼容性：新增路由与 schema，无已有字段删除；`size`、`created_at_epoch` 使用十进制字符串，避免跨语言 64 位精度问题。

### 2026-09-28：SF-03 受控 Job 提交

- 新增 `POST /projects/{p}/jobs`、`GET /jobs/{j}`、`GET /jobs/{j}/report`、`POST /jobs/{j}/cancel`。
- Job 提交要求 `Idempotency-Key`、同 revision 的不可变 snapshot 和 grant；重放不会重复消耗 `max_jobs`。
- 当前报告是 SF-06 前的过渡视图；grant/receipt 路由 schema 仍待联合冻结。

### 2026-10-01：SF-01 契约补全 + SF-02 限额/保留 + SF-05 快照提交

**新增 wire schema（此前路由已实装但无 schema）**

- `schemas/event.schema.json`：项目事件。`sequence` 为十进制字符串；`type` 枚举
  （project/opened|closed|dirty|saved、revision/advanced、snapshot/created、job/created|finished、
  artifact/produced、selection/changed、agent/state）；`trace_id` 可空。
- `schemas/project-state.schema.json`：`/state` 载荷。新增 `oldest_sequence`（游标窗口下界）与
  `event_retention{max_events,max_age_seconds}`；`active_jobs` 由"预留空数组"改为真实进行中 Job 摘要
  （job_id/job_type/capability/state，不含 params）。
- `schemas/grant.schema.json`：`status` 枚举 active|revoked|expired|exhausted；含 `steps`/`max_jobs`/`used_jobs`。
- `schemas/ui-receipt.schema.json`：`level` 枚举 hint|l4|teaching；`state_version` 为十进制字符串；
  核销结果含 `consumed`/`replayed`；**不含 issue 内容字段**（凭据不进模型上下文）。
- `schemas/wave-signals.schema.json`、`schemas/wave-query.schema.json`：`completeness` 枚举
  exact|complete|partial；tick 与区间为十进制字符串；新增 `initial_known` 区分"无前值"与"前值为空"。

**收紧既有 schema**

- `job-report-view.schema.json` 的 `state` 由任意字符串收紧为 9 态枚举
  （Queued/Running/Cancelling/Cancelled/Succeeded/Failed/TimedOut/Rejected/Unknown）。
  实现早已按 9 态映射，此前 schema 未表达，属 schema 滞后修正。

**行为与限额变更**

- 事件保留：由"仅 10,000 条"改为 **10,000 条且不超过 24 小时**；读路径同样裁剪，
  静默期后的过期游标稳定返回 `410 CURSOR_EXPIRED`（此前可能返回空页）。
- 普通响应上限 **2 MiB**：超限不再可能返回被截断的 JSON，改为
  `429 RESOURCE_EXHAUSTED` + `error.details{limit_bytes,actual_bytes}`。**新增 429 语义，客户端需处理。**
- 新增 `GatewayConfig.maxResponseBodyBytes`；`port>0` 时改为显式 `bind_to_port`（原恒系统分配）。
- 快照 `snapshot_id` 改为**内容寻址**（`SHA-256(input_fingerprint)` 前 24 位）：
  **迁移窗口**——旧实现同输入会得到不同 id，现在同输入返回同一快照，客户端不得依赖
  "每次提交都产生新 id"；同时修掉并发/重复提交时 `rename` 到已存在目录的偶发 `503`。

**校验器**

- `SimpleSchemaRegistry` 支持 `enum`、嵌套 `properties`/`items` 与 `integer ⊂ number`。
  未知可选字段仍被忽略（向前兼容），未知枚举值显式失败。

**fixtures**

- `fixtures/golden.json` 新增 event（含空 data）、project-state（含 context 为 null）、grant（active/revoked）、
  ui-receipt（active/已核销重放）、wave-signals、wave-query（exact / partial 含 x/z 与 64 位边界 tick）。
- `edu_contract_smoke` 改为按 fixture 名字取值，不再依赖数组下标。

**OpenAPI**

- `sigflow.openapi.yaml` 覆盖全部 22 条实装路由，标注两条身份边界（grant/receipt 签发仅 UI）、
  危险参数白名单、游标与分页、限额与兜底错误映射；并更正了"无 24 小时淘汰""无 2 MiB 护栏"的旧描述。

### 2026-10-01（第二轮）：SF-03 Job 存储根目录、SF-06 旧报告映射、SF-02/SF-05 边界

**新增字段（向后兼容，均为新增）**

- `Diagnostic.location` 明确子字段：`file`/`line`/`column`（新格式）与 `log_line`/`ir_coordinate`
  （旧 `main/jobs` reports 格式）。此前 `location` 只声明为 `object|null`，旧报告的 `log_line`
  在归一化时被**丢弃**；现在逐字段保留，全部缺失时才为 `null`（不伪造行号）。
- `Diagnostic.code` 语义明确：无原始码时置 `UNCLASSIFIED`，`raw_code` 保留原码或 `null`。
  约束 C++ 侧不得替教学层假定根因。

**行为变更**

- `POST /projects/{p}/jobs` 引用快照时区分两种失败：
  **格式合法且目录已不存在**（保留策略清理过）→ `410 ARTIFACT_EXPIRED`；
  其余（id 非法/从未存在/recording 损坏）→ `404 NOT_FOUND`。此前一律 404，客户端无法区分
  "旧引用已过期"与"id 打错"。**调用方需处理该 410。**
- Job 数据根目录不再默认落在系统临时目录：`CoreJobService` 支持
  `<project>/.sigflow/agent/jobs/<projectKey>` > 应用数据目录 > 临时目录（**带 warning**）的解析顺序，
  并新增 `IsControlledRoot()` 表明是否为受控目录。此项只改目录选择，不改 `eda.job.v1` manifest 字段。

**测试**

- `tests/contract/job_service_smoke.cpp`：受控根解析 12 项 + 重启恢复加固 6 项。
- `tests/agent/edu_report_normalizer_smoke.cpp`：旧 report 映射 10 项（含 log_line 变异验证）。
- `tests/agent/edu_gateway_fault_smoke.cpp`：1 MiB 请求边界 4 项（不泄露路径/token）。
- `tests/agent/edu_gateway_smoke.cpp`：已清理快照 410 语义 3 项（含变异验证）。

### 2026-10-01（第三轮）：SF-02 Job 源事件订阅

**新增事件语义（不新增字段，复用既有 `event` schema）**

- `type` 取值新增两个已实现值，`schemas/event.schema.json` 的枚举对应更新：
  `job/state-changed`（Job 状态真正变化）与 `job/finished`（终态，每个 Job 至多一次）。
  `data` 携带 `job_id`/`state`/`exit_code`，并在可用时带 `capability`/`snapshot_id`/`revision`。
- 投递规则：同一 `(job_id, state)` **不重复投递**，宿主可幂等轮询；
  服务已不持有的 Job **跳过**，不伪造终态事件。

**实现**

- 新增 `core/src/eda-agent-gateway/JobEventSource.{h,cpp}`（wx 无关）与
  `GatewayServer::PollJobEvents()`；`POST /projects/{p}/jobs` 提交路径同步登记归属。

**测试**

- 新增 `tests/agent/edu_job_event_source_smoke.cpp`（33 项断言），含经
  `POST /jobs` → `PollJobEvents` → `GET /events` 的端到端链路与变异验证。

### 2026-10-07：NG-05 局部设计证据 / NG-07 Agent 侧契约 / NG-09 旧历史只读桥

**NG-05 局部设计证据与未保存缓冲边界（SigFlow 侧新增路由与 schema）**

- 新增路由 `GET /projects/{projectId}/design/nodes/{nodeId}`（只读）。
  `data.mapping_status` 明确取 `available` / `ambiguous` / `stale` / `unavailable`，
  并用 `reason` 给出机器可读原因（`node_not_registered`、`no_source_mapping`、
  `unmapped_path`、`revision_changed`、`source_changed`、`source_missing`、
  `unsaved_only`、`ambiguous_node`、`declared_unavailable`）。无法可靠映射时**不猜**
  候选对象、不伪造行号；显式 `revision` 与当前不一致返回 409 `STALE_REVISION`。
- 新增 `schemas/design-node.schema.json`、`schemas/context-query.schema.json`；
  `/context/query` 的 `needs` 由仅 `sources` 扩展为
  `sources` / `selection` / `design` / `buffer`，被裁剪项一律进 `omitted[]` 并带 reason。
- **硬边界（不可放宽）**：设计上下文恒 `usable_for_execution=false`；未保存编辑缓冲
  （`buffer_excerpts[]`）只在工程 dirty 时返回，带 `origin="unsaved_buffer"`、
  `usable_for_execution=false`，且**没有 `source_id`**，因此不可能成为 snapshot 或工具
  执行输入；单条上限 64 KiB（超出截断并置 `truncated=true`）；响应不含本机绝对路径。
- 兼容性：仅新增路由与新增可选字段/枚举值；已有路由与字段语义不变。
- 测试：`tests/agent/edu_design_context_smoke.cpp`（48 项断言），含四态映射、
  revision 竞态、同 revision 内源码改写、路径越界、缓冲截断与“缓冲不可执行”。

**NG-07 自研 Agent 服务侧契约冻结（`agent.openapi.yaml`）**

- 新增 `agent.openapi.yaml` 与 `schemas/`：`teaching-card`、`plan-card`、`agent-session`、
  `agent-run`、`agent-event`、`agent-health`、`agent-capabilities`。
  协议同为 `edu.api.v1`，信封与错误码风格与 SigFlow 侧一致。
- `fixtures/golden.json` 新增 `agent-*` 9 项与 `agent_cards.json`（含 3 个负例）。
- 语义：卡片必须带 `limitations` 与证据引用；无模型 Key 时降级为确定性规则卡
  （`source="rule"`、`model_used=false`）；L4 需 `more=true` 的两步策略；
  Agent 服务不持有任何 EDA 执行权限（`execution.can_run_eda=false`）。
- 兼容性：新增文件，不影响既有 SigFlow 路由。

**NG-09 旧 `main/jobs` 历史只读桥**

- 新增路由 `GET /projects/{projectId}/legacy/jobs` 与
  `GET /projects/{projectId}/legacy/jobs/{jobId}/report`，新增
  `schemas/legacy-job.schema.json`；报告响应复用 `schemas/job-report-view.schema.json`。
- **不修改旧格式**：桥只读 `<project>/.sigflow/jobs/<type>/<jobId>/`；旧 manifest 的
  `parameters`（含本机绝对路径/可执行文件）永不外泄；旧 `artifacts[].path` 只转成
  opaque `artifact_id` + `sha256`。
- 旧记录无指纹/版本 → `origin="legacy"`、`completeness="legacy_unverified"`、
  `revision`/`snapshot_id`/`input_fingerprint` 为 null，绝不冒充当前 revision 证据；
  未知状态串不当作 `Succeeded`；缺报告/损坏报告是显式 `problem` 与 404，不等于“无问题”。
- 测试：`tests/agent/edu_legacy_bridge_smoke.cpp`（38 项断言），含“桥运行前后旧历史
  字节不变”的只读证明。

**NG-07 宿主侧真实进程联调**

- `main/agent/AgentServiceController` 暴露 `ValidateReadyLine()`，使非 GUI 测试能用
  控制器**自身**的握手规则判定真实 sidecar 的 ready 行（避免测试复述规则）。
- 测试：`tests/agent/edu_agent_sidecar_smoke.cpp`（33 项断言）：真实启动
  `python -m sigflow_edu_agent --bootstrap-stdin`，覆盖 bootstrap → ready（nonce/协议/
  端口/版本）→ `/health` → 无 Key 规则卡 → Gateway 不可达时显式失败 → `/shutdown`
  正常退出，以及 nonce/协议失配与 ready 前退出必须被拒绝。缺 Python 时显式 SKIP。

**报告与路径边界修复（不影响既有字段语义）**

- 报告里的**结构化**本机路径不再外泄：`metrics.*.value` 中的路径替换为文件名并标记
  `path_redacted=true`（附 `path_redacted_reason`），`diagnostics[].location.file` 同理标记
  `file_redacted=true`。**残留风险**（已知）：工具自由文本 `diagnostics[].summary` 与日志片段
  仍可能由工具自己打印路径。
- 非 ASCII（中文）相对路径此前完全不可用，已修：工程 `source_files` 与 Gateway 的
  `/sources/{id}`、`/context/query` 改用 `platform::PathFromUtf8` 构造路径；新增
  `platform::RelativePathToUtf8()` 统一"UTF-8 + POSIX 分隔符"的契约路径表示，替换
  `PathToUtf8(path.generic_string())` 的双重编码写法（`GatewayServer` 源摘要、
  `SnapshotService` 清单）。ASCII 路径的清单表示不变（本来就是正斜杠）。

**NG-10 契约运行期校验与边界矩阵**

- 新增 `tests/agent/edu_schema_runtime_smoke.cpp`（119 项断言）：用真实 loopback HTTP 驱动
  各路由，把**真实响应 data** 交给 `SimpleSchemaRegistry` 对照 `schemas/*.schema.json`
  校验；并记录 1 MiB 请求 / 2 MiB 响应 / 分页上限（events 100-500、waves 256-4096、
  legacy 100-500）/ 64 位 tick 与 `high_watermark` 十进制字符串 / 中文+空格相对路径 /
  8 线程并发 / 游标过期 410 / 未知路径 404 的实测结果。
- 已知观察（未改实现）：2 MiB 护栏返回的 429 `retryable=false`，而本文档
  `sigflow.openapi.yaml` 的限额段落写作 retryable=true，两处需统一。

### 2026-10-07（第二轮）：契约运行期严格校验、路径边界与故障矩阵

**契约对齐（向后兼容，仅新增可选字段）**

- `schemas/diagnostic.schema.json` 新增可选字段 `summary_paths_redacted`（boolean）：
  工具自由文本 `summary` 里的本机绝对路径已被替换为文件名时置 true。
- `GET /waves/{artifactId}/signals` 的 `time_range` 由 `{begin,end,valid}`（JSON 数字）
  对齐为契约声明的 `{start_tick,end_tick}`（十进制字符串），时间范围不可信时为 `null`。
  这是一处**实现纠正**（此前实现与冻结 schema 不一致），字段名与类型均按 schema 收敛。
- `SimpleSchemaRegistry` 现在解析 `$ref`（文件引用 + 内部 JSON Pointer，带深度上限与循环保护），
  且 `required`/`enum`/`const` 在**每一层**对象上生效。`additionalProperties` 仍不强制——
  契约的演化规则是"未知可选字段被忽略"，两者此前互相矛盾，此处以演化规则为准。

**Agent 路径边界（约束：Agent 不得看到本机绝对路径）**

- 新增 `core/src/eda-agent-gateway/PathRedaction.{h,cpp}`：识别 Windows 盘符路径、UNC、
  以及以常见根前缀开头的 POSIX 路径；把每一处替换为**文件名**，并把 URL 形态的
  `/api/v1/...` 排除在外。
- 应用范围：报告 `metrics.*.value`（标记 `path_redacted`）、`diagnostics[].location.file`
  （标记 `file_redacted`）、`diagnostics[].summary`（标记 `summary_paths_redacted`），
  以及 `GET /artifacts/{a}/content` 的文本类内容（标记 `path_redacted` + `redacted_paths`）。
- 裁剪一律**显式标记**，不静默改写工具事实。

**故障矩阵与恢复（NG-06→NG-10 的非 GUI 部分）**

- 新增 `tests/agent/edu_fault_matrix_smoke.cpp`：跨工程隔离（grant/snapshot/artifact/job）、
  产物失效（文件删除 → 410，绝不返回缓存 200）、取消语义（只影响本 run、重复取消无副作用）、
  幂等冲突、以及双工程并发只读压力。
- `tests/agent/edu_event_retention_smoke.cpp` 增补重启恢复：事件 JSONL 重放后
  `/state` 可重建、`project_id` 跨重启稳定、重启前的游标仍然有效且不重放已投递事件。
  （注意：`/events` 默认长轮询 20 s；确认"无新事件"必须显式 `wait_ms=0`。）

**sidecar 发布路径（NG-07 非 GUI 部分）**
- `tests/agent/edu_agent_sidecar_smoke.cpp` 增补双实例隔离（不同端口/不同 token/交叉 token
  被拒/不同数据目录/互不影响关闭）与**安装后的启动器入口**验证
  （`agent/bin/sigflow-edu-agent.cmd`，Windows 经 `cmd.exe /c`）。
- 新增 `agent/RELEASE.md`（安装/升级/回滚/版本与许可证/未验证项）与
  `agent/scripts/verify_install.py`（只读安装自检，退出码 0/1）。

**快照提交的偶发抖动（实现修复，契约不变）**

- `POST /projects/{p}/snapshots` 约 5% 的运行会在**首次**提交时返回
  `503 SERVICE_UNAVAILABLE / unable to commit snapshot directory`。根因：Windows 上刚写完文件的
  目录 rename 会被防病毒/搜索索引器短暂持有（sharing violation / access denied），而提交路径
  只尝试一次。`SnapshotService` 现在做有界退避重试（约 150 ms），失败原因仍如实上报。
  行为与响应契约不变，只是不再把可恢复的瞬时错误报成 503。

### 变更规则

1. 提变更者在本文件记录：问题、旧/新字段与语义、影响路由、兼容性、迁移窗口、双方代码/测试清单。
2. S1/U1 同审 wire/鉴权/错误/恢复；S2/U2 审数据事实/教学语义；S3 审 UI 状态。
3. 先改 schema + 成功/失败 fixtures，再改实现；非兼容变更升主版本或明确版本化路径。
