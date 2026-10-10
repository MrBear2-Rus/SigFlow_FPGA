# SigFlow Edu Agent

自研的 SigFlow 教育版教学 Agent sidecar。它是一个本机 loopback 服务，
被 SigFlow 桌面端以子进程方式拉起，负责**教学流程、提示策略、证据解读与计划建议**。

它**没有独立 EDA 执行权限**：不运行 Yosys/Verilator（或任何其它工具）、
不读也不写工程文件、不执行命令、不接受客户端传入的路径、脚本、可执行文件名或工作目录。
综合与仿真只能由 SigFlow 签发的 grant 触发；基础 v1 runs 不自动执行。

- 100% 自研 Python，**只使用标准库**（`http.server` + `socketserver` + `urllib.request`），
  没有任何第三方运行时依赖（没有 requests/aiohttp/flask）。
- 不依赖 UCAgent 源码、运行时、`UCTool`/`Checker` 类、默认验证流程或上游镜像；
  仅参考其“分阶段工作流、受限工具、阶段检查、运行记录”的架构思想。
- 支持 Python >= 3.9；Windows 与 Linux 使用同一份代码，只更换启动器。

## 安装与运行

> 发布、升级、回滚、版本/许可证与"未验证项"见 [`RELEASE.md`](RELEASE.md)；
> 安装自检用 `python agent/scripts/verify_install.py`（只读、不联网，退出码 0/1）。

### 方式一：pip 安装（推荐发布路径）

```sh
python -m pip install ./agent
```

安装后得到入口点 `sigflow-edu-agent`（`[project.scripts]`），以及两个随包安装的启动器
`sigflow-edu-agent.cmd`（Windows）与 `sigflow-edu-agent`（POSIX sh）。

### 方式二：不安装，直接用启动器或 PYTHONPATH

```sh
# Windows
set SIGFLOW_EDU_AGENT_EXECUTABLE=<repo>\agent\bin\sigflow-edu-agent.cmd
# POSIX
export SIGFLOW_EDU_AGENT_EXECUTABLE=<repo>/agent/bin/sigflow-edu-agent
```

宿主只需设置这一个环境变量，然后以 `<executable> --bootstrap-stdin` 启动。启动器会：

1. 用 `%SIGFLOW_EDU_AGENT_PYTHON%`（Windows）或 `${SIGFLOW_EDU_AGENT_PYTHON:-python3}`（POSIX）
   选择解释器，默认分别是 `python` 与 `python3`；
2. 把包目录 `<launcher dir>/../src` 前置到 `PYTHONPATH`；
3. `exec` 到 `python -m sigflow_edu_agent <args>` 并透传退出码。

手工调试时也可以直接跑：

```sh
cd agent/src && python -m sigflow_edu_agent --bootstrap-stdin
```

## 命令行参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--bootstrap-stdin` | 关闭 | **正常启动必须提供**。从 stdin 读恰好一行 UTF-8 JSON bootstrap。 |
| `--port N` | `0` | 绑定的 loopback 端口；0 表示由系统分配。 |
| `--agent-version X` | 包版本 | 覆盖 ready 记录与 `/health` 上报的版本（供版本失配测试使用）。 |
| `--selftest-fail MODE` | `none` | **仅测试**的故障注入，见下文。 |
| `--selftest-ready-delay-ms N` | `0` | **仅测试**：发出 ready 前延迟 N 毫秒。 |

## bootstrap / ready 协议

严格按 `docs/edu-agent/sigflow/sidecar-bootstrap.md` 实现：

1. 从 stdin 读一行 JSON；要求 `type == "sigflow-bootstrap"`、`protocol == "edu.api.v1"`，
   且 `instance_id`/`nonce`/`gateway_url`/`gateway_token`/`ui_token`/`data_directory` 均为非空字符串。
   任何问题 → **写一句原因到 stderr 并以退出码 2 结束，stdout 保持完全为空**。
2. 绑定 `127.0.0.1:PORT`，**先绑定再报 ready**；绑定失败 → 退出码 3。
3. 向 stdout 写**恰好一行** JSON 并 flush：

   ```json
   {"type":"ready","protocol":"edu.api.v1","nonce":"<与 bootstrap 相同>","port":18423,"agent_version":"0.1.0"}
   ```

   之后**永不再写 stdout**；所有诊断只走 stderr。
4. 服务直到收到 SIGTERM/SIGINT，或一个通过鉴权的 `POST /api/v1/shutdown`；两者都在 3 秒内优雅停止并以 0 退出。

token 处理约定：`gateway_token` / `ui_token` **只存在于内存**，不回显、不落盘、不写日志、
不放进 URL 或错误消息；启动时安装的日志过滤器会对每条日志记录做替换。
`data_directory` 只在内部使用，**不会**通过任何接口回传。

## HTTP 接口

全部位于 `/api/v1`，只监听 loopback（Host 头非 loopback → 403 `POLICY_DENIED`）。
除 `GET /api/v1/health` 外，所有路由都要求 `Authorization: Bearer <ui_token>`
（`hmac.compare_digest` 比较）；缺失或错误 → 401 `UNAUTHENTICATED`。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/health` | 协议、版本、实例 ID、`model.available` |
| GET | `/api/v1/capabilities` | L1–L4 模式、能力就绪状态、`execution.can_run_eda=false` |
| POST | `/api/v1/sessions` | 创建会话，201 |
| GET | `/api/v1/sessions/{sid}` | 会话摘要 |
| POST | `/api/v1/sessions/{sid}/runs` | 请求一次教学运行，201 |
| GET | `/api/v1/runs/{rid}` | 读取运行结果 |
| GET | `/api/v1/sessions/{sid}/events?after=<seq>` | 会话事件；未知游标 → 410 `CURSOR_EXPIRED` |
| POST | `/api/v1/shutdown` | 202 后优雅停止 |

信封与 EDA Gateway 完全同构：

```json
{"schema_version":"edu.api.v1","request_id":"req-1","trace_id":"trace-1","data":{...}}
{"schema_version":"edu.api.v1","request_id":"req-2","trace_id":"trace-2",
 "error":{"code":"POLICY_DENIED","message":"...","retryable":false,"details":null}}
```

错误码只允许这 12 个：`INVALID_ARGUMENT`、`UNAUTHENTICATED`、`POLICY_DENIED`、`NOT_FOUND`、
`STALE_REVISION`、`IDEMPOTENCY_CONFLICT`、`CURSOR_EXPIRED`、`RESOURCE_EXHAUSTED`、
`SERVICE_UNAVAILABLE`、`MODEL_UNAVAILABLE`、`GATEWAY_UNAVAILABLE`、`CAPABILITY_UNAVAILABLE`。

硬限制（超出即显式报错或计入 `omitted`，绝不静默截断）：

- 请求体 1 MiB → 413 `RESOURCE_EXHAUSTED`；
- `selection.text` 64 KiB → 400 `INVALID_ARGUMENT`；
- 单次运行卡片 16 张 → 多余项记入 `omitted`；
- 会话 32 个 → LRU 淘汰最旧（随后其 `session_id` 返回 404）；
- 每会话事件保留最新 1000 条 → 更早的 `after` 返回 410；
- 未知路径或不支持的方法 → 404 `NOT_FOUND`；
- **任何响应都不含本机绝对路径**（卡片、证据引用、错误消息、日志一致适用）。

契约真源：

- Agent 侧：[agent.openapi.yaml](../../contracts/edu-agent/v1/agent.openapi.yaml)
- DTO：[teaching-card](../../contracts/edu-agent/v1/schemas/teaching-card.schema.json)、
  [plan-card](../../contracts/edu-agent/v1/schemas/plan-card.schema.json)、
  [agent-session](../../contracts/edu-agent/v1/schemas/agent-session.schema.json)、
  [agent-run](../../contracts/edu-agent/v1/schemas/agent-run.schema.json)、
  [agent-event](../../contracts/edu-agent/v1/schemas/agent-event.schema.json)
- fixtures：[golden.json](../../contracts/edu-agent/v1/fixtures/golden.json)、
  [agent_cards.json](../../contracts/edu-agent/v1/fixtures/agent_cards.json)

## 教学策略要点

- **`kind=explain` / `hint`**：在 `selection.text` 上运行确定性规则引擎
  （`RTL_LATCH_INCOMPLETE_BRANCH`、`RTL_SENSITIVITY_INCOMPLETE`、`RTL_WIDTH_MISMATCH`、
  `RTL_BLOCK_UNBALANCED`）。命中时返回 `source:"rule"`、`level` 为请求难度的卡片，
  `evidence[0].kind == "rule_match"`，正文只说明推理过程，**不给出可直接替换的完整代码**。
- **`level=L4`**：基础 runs 始终 403，`more=true` 不是授权。扩展先 observe/hint_next 到 L3，
  再 reference_request 取得 challenge，原生 UI 独立确认签发 receipt 后 reference_confirm 原子核销。
  只给参考检查要点，不代写工程；challenge/receipt 与 issue/session/版本/策略/动作严格绑定。
- **`kind=report_review`**：必须带 `job_id`。用 `gateway_token` 调用
  `GET {gateway_url}/api/v1/jobs/{job_id}/report`，把 `diagnostics` 映射为
  `diagnostic_explanation` 卡片；`code`/`raw_code`/`severity`/`location` 原样进入证据，
  报告的 `revision`/`snapshot_id`/`input_fingerprint`/`completeness`/`origin` 一并入证据，
  正文不补造报告未给出的根因。
- **空 `diagnostics`**：只有报告归属/版本相符、state=Succeeded、completeness=complete、origin=core，
  且存在 snapshot_id/input_fingerprint 时，才解释为“工具本次没有报告诊断”；
  否则返回 `issue_id: null` 的 `source:"rule"` 卡片说明**证据不足**，
  绝不宣称“电路没有问题”。
- **`kind=plan`**：只使用 `eda.synth` / `eda.sim.build` / `eda.sim.run`，
  `params` 仅允许受限标量 key，`snapshot_required=true`、`requires_approval=true`。
  卡片中不会出现路径、可执行文件名、脚本或工作目录。

## 降级矩阵

| 场景 | 宿主观察到的现象 | 服务行为 | IDE 影响 |
|---|---|---|---|
| **未安装**（`SIGFLOW_EDU_AGENT_EXECUTABLE` 未设置或文件不存在） | 控制器 `Disabled`/`Failed` | 进程不存在 | 编辑、手动仿真/综合照常 |
| **启动失败**（bootstrap 非法，退出码 2） | 15 秒内无有效 ready → 重启（1s/2s/4s） | stdout 为空，原因只写 stderr | 达到重启上限后只禁用 Agent |
| **端口绑定失败** | 同上（退出码 3） | 绑定先于 ready | 同上 |
| **协议不兼容**（ready 的 `protocol` ≠ `edu.api.v1`） | 控制器判定 “protocol is incompatible”，不进入 Ready | 进程照常服务 | 只停 Agent |
| **nonce 失配** | 控制器判定 “ready nonce did not match” | —— | 只停 Agent |
| **无模型 Key**（`SIGFLOW_EDU_MODEL_API_KEY` 未设置） | `/health`、`/capabilities` 的 `model.available=false` | 教学请求走规则路径，`state=degraded`、`model_used=false`，被跳过的模型步骤进 `omitted` | 无影响，规则卡片可用 |
| **网络故障**（Gateway 不可达/拒绝 token/报告服务不可用） | `report_review` 返回 503 `GATEWAY_UNAVAILABLE` | `state=failed`，**不返回任何假装读过报告的卡片** | 无影响 |
| **Gateway 报告该 job 不存在** | 404 `NOT_FOUND` | 同上，`state=failed` | 无影响 |
| **Agent 崩溃** | 控制器按 1s/2s/4s 最多重启 3 次 | —— | 达到上限后只停 Agent，编辑/手动 EDA 不受影响 |
| **健康检查连续两次失败** | 控制器重启 sidecar | —— | 同上 |

## 仅测试用的开关（不属于冻结契约）

用于验证宿主的启动/失败/重启路径，**生产启动不得使用**：

| 模式 | 行为 |
|---|---|
| `none` | 正常启动 |
| `exit-before-ready` | 绑定前以退出码 7 结束（模拟“启动即崩”） |
| `exit-after-ready` | 发出 ready 后以退出码 9 结束（模拟“ready 后崩溃”） |
| `wrong-nonce` | ready 行使用错误的 `nonce` |
| `wrong-protocol` | ready 行使用 `edu.api.v0` |
| `hang` | 永不绑定也永不写 ready（模拟挂死） |
| `no-ready` | 绑定但永不写 ready |

`--selftest-ready-delay-ms N` 用于把 ready 推迟到宿主的 15 秒超时之内/之外。

## 运行测试

```sh
# 从仓库根目录，任选其一
python agent/tests/run_all.py
python -m unittest discover -s agent/tests -t agent
```

两个入口等价，都会在失败时以非零码退出。测试只使用标准库
（`unittest` + 一个进程内的 stub EDA Gateway + 真实 loopback socket）。

## 模块布局

```
agent/
  bin/sigflow-edu-agent.cmd   Windows 启动器（PYTHONPATH 前置 ../src）
  bin/sigflow-edu-agent       POSIX sh 启动器（LF、#!/bin/sh）
  src/sigflow_edu_agent/
    __main__.py        argparse 入口、日志、ready 输出、信号处理
    bootstrap.py       stdin bootstrap 解析与校验
    gateway_client.py  唯一工程/EDA 通道（受限 Gateway HTTP，不签发 grant/receipt）
    rules.py           确定性、无模型的 RTL 规则引擎
    cards.py           TeachingCard / PlanCard 构造与校验
    dispatch.py        运行调度与教学策略（L4 两步放行、报告可信度判定）
    model_client.py    可选模型端点（无 Key 即降级，绝不伪造回答）
    session.py         内存 session/run/event 存储与容量上限
    http_server.py     loopback HTTP 服务、路由、鉴权、限额、信封
    errors.py          错误码与信封辅助
    util.py            ID、时间戳、脱敏、日志过滤器
    version.py         版本与协议常量
  tests/               unittest 套件
  .tmp/                开发期一次性脚本与输出（不参与发行）
```

## 故障排查

| 现象 | 排查方向 |
|---|---|
| 宿主一直停在 `Starting` | stdout 是否为空？ready 行是否**恰好一行**且带换行？`nonce`/`protocol` 是否与 bootstrap 一致？ |
| 进程立刻退出，码 2 | stderr 的第一行会写明确原因（缺字段、类型不符、协议不符、非 http(s) 的 `gateway_url`）；stdout 必须为空才算符合契约 |
| 进程立刻退出，码 3 | loopback 端口被占用或权限不足；`--port 0` 让系统分配 |
| `/health` 返回 403 | 客户端把请求发给了非 loopback 的 Host（例如用具名地址或反向代理） |
| 除 `/health` 外全部 401 | `Authorization: Bearer <ui_token>` 缺失或 token 不是本次 bootstrap 下发的那个 |
| `report_review` 返回 503 | Gateway 未启动、token 失效或 Job 服务未接入；本次运行 `state=failed` 是预期行为 |
| 教学请求总是 `degraded` | 未配置 `SIGFLOW_EDU_MODEL_API_KEY`；这是正常的无 Key 规则模式 |
| 想确认没有路径泄露 | 跑 `agent/tests/run_all.py` 中的 `test_no_card_ever_leaks_a_path_or_a_token` |
