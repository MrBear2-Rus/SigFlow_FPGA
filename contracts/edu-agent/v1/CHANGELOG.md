# edu-agent v1 契约变更记录

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

- `sigflow.openapi.yaml`：本期路由子集 `GET /health`、`GET /capabilities`（loopback + Bearer）。
- `schemas/`：`capabilities`、`diagnostic`、`job-report-view`、`source-ref`、`envelope`。
- `fixtures/golden.json`：4 组 golden；由 `tests/agent/edu_contract_smoke` 校验。

### 变更规则

1. 提变更者在本文件记录：问题、旧/新字段与语义、影响路由、兼容性、迁移窗口、双方代码/测试清单。
2. S1/U1 同审 wire/鉴权/错误/恢复；S2/U2 审数据事实/教学语义；S3 审 UI 状态。
3. 先改 schema + 成功/失败 fixtures，再改实现；非兼容变更升主版本或明确版本化路径。
