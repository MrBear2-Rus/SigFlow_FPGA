# Agent 模型与 API 仪表盘设计

> 版本：v1.0 · 2026-11-17。状态：已实施并验收。
> 基线：2026-11-17 工作区（改动未提交）。
> 实施与验收：[agent-model-dashboard-verification.md](agent-model-dashboard-verification.md)
> 执行清单：[agent-model-dashboard-task.md](agent-model-dashboard-task.md)
> 关联规格：[SigFlow-Edu-Agent-GUI-Spec.md](SigFlow-Edu-Agent-GUI-Spec.md)

范围：SigFlow 主程序 Agent UI（`main/agent`）与 `agent/` sidecar

## 1. 背景与目标

Agent UI 目前没有任何模型凭据的界面配置入口，也没有任何用量展示：

- 模型 API Key 只有两种来源：编译期嵌入（`SIGFLOW_EMBED_LOCAL_MODEL`，开发用）或环境变量；发行版用户无法在界面里配置。
- sidecar 只在启动时通过 stdin bootstrap 拿到一次 `model_config`，换 Key 必须重启 sidecar。
- 两个 Python 模型客户端都没有保留响应里的 `usage`，C++ 侧更看不到 token 数量；仓库内没有 usage 日志或统计。
- DeepSeek 官方只提供余额查询接口，没有公开的历史 token 用量 API，因此“用量”必须由本机采集。

本设计在 Agent 页面操作行增加一个“模型仪表盘”入口，打开宿主侧对话框，提供：

1. API Key 的本机安全存储、清除与 sidecar 重启生效（保存后由状态与余额查询验证连接）；
2. 模型可用性/降级原因/Key 来源展示；
3. DeepSeek 账户余额查询；
4. 本机 token 用量累计（总量 + 按日），从本版本上线后开始记录，不做历史回填。

## 2. 非目标

- 不管理 `Plugin_DeepSeek` 插件的 Key（`SIGFLOW_DEEPSEEK_API_KEY` / `deepseek.key` 是另一子系统）。
- 不读取或复用 `sigflowAgent/` 的 DPAPI 配置（那是另一条 teaching 服务）。
- 不做历史用量回填、调用明细列表、费用估算、预算告警、CSV 导出。
- 不实现 Linux 密钥库；非 Windows 只读环境变量。
- 不支持自定义 OpenAI 兼容端点；bootstrap 的 `model_config` 仍只接受 DeepSeek 端点。
- 不把仪表盘做成页内可停靠面板；本期是宿主侧对话框。

## 3. 现状依据

| 事实 | 位置 |
|---|---|
| AgentWorkspace 是纯视图，不写配置、不依赖 MainFrame | `main/agent/gui/AgentWorkspace.h:32-35` |
| 操作行现有“选择报告”“返回工作台”两个 Ghost 按钮 | `main/agent/gui/AgentWorkspace.cpp:149-164` |
| rail 注册 12 个动作（本期不新增 rail 动作） | `main/agent/gui/AgentNavigation.h:50-65` |
| 编译期嵌入 Key | `CMakeLists.txt:419-445`、`main/MainFrame.cpp:1000-1004` |
| sidecar 只通过 stdin bootstrap 收一次模型配置 | `main/agent/AgentServiceController.h:39-54,87-100`、`agent/src/sigflow_edu_agent/bootstrap.py:110-121` |
| 环境变量与本地配置文件来源优先级 | `agent/src/sigflow_edu_agent/local_model_config.py:6-49` |
| 模型状态与降级原因 | `agent/src/sigflow_edu_agent/model_client.py:118-142` |
| 现有路由风格与 UI token 鉴权 | `agent/src/sigflow_edu_agent/http_server.py:80-87` |
| 契约先行、新增动作同步 OpenAPI/fixture | `docs/agent/SigFlow-Edu-Agent-GUI-Spec.md:24` |
| sidecar 启停与 generation 失效链路 | `main/agent/AgentServiceController.cpp:209-239`、`main/MainFrame.cpp:1056-1101` |

> 上表行号为实现前快照；实施时以符号搜索为准，不按行号修改。

DeepSeek 接口事实（实现时以官方文档复核）：

- `GET https://api.deepseek.com/user/balance`：Bearer 认证，返回 `is_available` 与 `balance_infos[]`（`currency`、`total_balance`、`granted_balance`、`topped_up_balance`）。
- 没有公开的历史 token 用量/账单查询 API；每次 Chat Completions 响应的 `usage` 对象是本机唯一可靠来源。

## 4. 架构

```
┌──────────────────────────── SigFlow 主程序 (C++) ────────────────────────────┐
│ MainFrame                                                                     │
│  ├─ AgentModelSettings       DPAPI 存储 + 优先级 + 字段校验                   │
│  ├─ AgentServiceController   新增 SetModelSettings()；Stop()→Start() 生效     │
│  ├─ AgentDashboardController 后台查询 usage/balance，回主线程；丢弃过期结果   │
│  └─ AgentWorkspace                                                           │
│       └─ 操作行 [模型仪表盘] ──host callback──► AgentDashboardDialog          │
└───────────────┬───────────────────────────────────────────────┬──────────────┘
                │ bootstrap stdin（唯一 Key 通路）              │ UI token HTTP
                ▼                                               ▼
┌──────────────────────────── agent/ sidecar (Python) ─────────────────────────┐
│ ModelClient ── 成功/失败记录 ──► usage.py（本地 JSONL 累计）                  │
│ GET /api/v1/usage     本地统计，不联网                                        │
│ GET /api/v1/balance   转发 DeepSeek /user/balance，不回显 Key/原始响应体      │
└───────────────────────────────────────────────────────────────────────────────┘
```

### 4.1 组件职责

| 组件 | 职责 | 明确不做 |
|---|---|---|
| `main/agent/AgentModelSettings.{h,cpp}`（新增） | 校验、DPAPI 加密、原子写入、加载优先级、清除 | 不联网、不碰 UI |
| `AgentServiceController`（修改） | 新增 `SetModelSettings()`；由 MainFrame 调 `Stop()`→`Start()` | 不读配置文件 |
| `agent/src/sigflow_edu_agent/usage.py`（新增） | 记录、聚合、裁剪、损坏行容错 | 不记录提问内容、路径、Key |
| `model_client.py`（修改） | 提取 `usage` 并上报；新增 `fetch_balance()` | 不写 Key 日志、不回显原始响应 |
| `http_server.py` + OpenAPI（修改） | 两个只读 GET：usage、balance | 不新增写接口 |
| `AgentApiClient`（修改） | `GetUsage()` / `GetBalance()`，严格校验信封 | 不处理 Key |
| `AgentDashboardController`（新增） | 后台查询 + `CallAfter` 回主线程 + 请求序号/generation 防护 | 不轮询、不写配置 |
| `AgentDashboardDialog`（新增） | 状态、Key 表单、余额、用量四区块 | 不直接读写配置、不直接联网 |
| `AgentWorkspace`（修改） | 操作行按钮 + `SetDashboardHandler()` | 不写配置、不依赖 MainFrame |

### 4.2 边界

- Key 永不进入日志、tooltip、状态文案或错误消息；界面只显示“已配置”与来源。
- 保存 Key 后立即重启 sidecar；generation 变化沿用现有 `onStatus` 失效链路。
- 余额查询需要 sidecar Ready 且已配置 Key；未就绪时显示明确原因，不显示假数据。
- 仪表盘是宿主能力；`AgentWorkspace` 只增加一个回调入口，维持现有视图边界。

## 5. Key 存储

### 5.1 文件与格式

- 路径：`AppDataRoot()/sigflow-edu-agent/model.credentials.json`
- 外层信封（仅密文）：

```json
{"schema": "sigflow.agent.model.dpapi.v1", "encrypted": "<base64 DPAPI blob>"}
```

- 解密后的明文结构（仅在内存中使用）：

```json
{"api_key": "...", "model": "deepseek-chat", "base_url": "https://api.deepseek.com"}
```

- Windows：`CryptProtectData`，当前用户范围，不设 `CRYPTPROTECT_LOCAL_MACHINE`，使用 `CRYPTPROTECT_UI_FORBIDDEN`。
- 原子写入：同目录临时文件写入 → 替换目标；失败时保留旧文件。
- 非 Windows：`Save`/`Clear` 返回 `unsupported_platform`，界面禁用保存并提示环境变量方式。

### 5.2 校验规则（与 Python `valid_model_config` 一致）

- `api_key`：1–512 字符，全部 33–126 可打印 ASCII。
- `model`：1–128 字符，全部 33–126 可打印 ASCII。
- `base_url`：仅 `https://api.deepseek.com` 或 `https://api.deepseek.com/v1`。

### 5.3 加载优先级

1. 显式环境变量：存在任一 `SIGFLOW_EDU_MODEL_PROVIDER/API_KEY/BASE_URL/NAME` 或 `DEEPSEEK_API_KEY/MODEL/BASE_URL` → 整套使用环境配置。
2. 本机 DPAPI 存储。
3. 编译期嵌入（`SIGFLOW_EMBED_LOCAL_MODEL`，仅开发构建）。
4. 未配置。

该顺序与 sidecar 的 `load_model_config` 语义一致：显式环境变量存在时，bootstrap 里的 `model_config` 会被 sidecar 忽略。界面显示的“Key 来源”必须按同一优先级计算。

### 5.4 外部来源的边界情况

若 `/capabilities` 报告 `model.available=true`，但宿主按上述优先级得到“未配置”，说明 Key 由外部启动器/包装脚本提供（例如 `SIGFLOW_EDU_LOCAL_MODEL_CONFIG`）。界面显示来源为“外部/未知”，不显示“未配置”，也不尝试读取该文件。

## 6. Sidecar 用量采集与接口

### 6.1 usage.py

- 存储：`<data_directory>/usage/usage.jsonl`，每行一条记录。
- 记录字段：`ts`（UTC ISO 8601）、`model`、`ok`、`prompt_tokens`、`completion_tokens`、`total_tokens`、`tokens_unavailable`。
- 不记录提问、证据、路径、Key、错误原文。
- 写入使用追加模式并加锁；单行长度有上限。
- 读取时跳过损坏行并计数；启动时裁剪 90 天前的记录；文件异常不影响服务启动。
- 按 UTC 日期聚合；接口返回 `since`（最早记录日期）与 `skipped` 计数。

### 6.2 model_client.py

- 成功解析响应后，严格读取 `usage` 的整数非负字段；缺失或非法 → `tokens_unavailable=true`，调用仍记为成功。
- 失败路径记录 `ok=false`，token 记 0；不记录错误原文。
- 新增 `fetch_balance()`：`GET https://api.deepseek.com/user/balance`，Bearer 认证，超时 5–10 秒，不跟随重定向，不读代理；响应做严格 JSON 校验。
- 余额错误映射固定码：`model_unconfigured`、`balance_unsupported_provider`、`balance_unauthorized`、`balance_unreachable`、`balance_timeout`、`balance_unavailable`、`balance_invalid_response`。任何响应都不包含 Key 或上游原始正文。

### 6.3 HTTP 接口

两个接口都沿用现有 UI token 鉴权（`/api/v1/health` 除外），均为只读 GET。

`GET /api/v1/usage?days=30`

- `days` 默认 30，限制 1–90。
- 200 响应：

```json
{
  "schema": "edu.usage.v1",
  "generated_at": "2026-11-17T08:00:00Z",
  "since": "2026-10-18",
  "days": 30,
  "skipped": 0,
  "totals": {
    "calls": 12,
    "failed_calls": 1,
    "prompt_tokens": 1234,
    "completion_tokens": 567,
    "total_tokens": 1801,
    "tokens_unavailable_calls": 0
  },
  "daily": [
    {"date": "2026-11-17", "calls": 3, "failed_calls": 0,
     "prompt_tokens": 400, "completion_tokens": 120, "total_tokens": 520}
  ]
}
```

- 无记录：`since=null`、totals 全 0、`daily=[]`，仍为 200。
- `calls` 包含失败调用；`tokens_unavailable` 的调用 token 记 0。

`GET /api/v1/balance`

- 200 成功：

```json
{
  "schema": "edu.model.balance.v1",
  "available": true,
  "checked_at": "2026-11-17T08:00:00Z",
  "currency": "CNY",
  "total_balance": "12.34",
  "granted_balance": "0.00",
  "topped_up_balance": "12.34"
}
```

- 失败仍返回 200，`available=false`，附固定 `reason` 码；本地用量不受影响。
- 余额接口不写入用量记录，也不消耗模型额度。

### 6.4 契约

同步更新 `contracts/edu-agent/v1/agent.openapi.yaml`、golden fixture 与契约测试；新增路由必须出现在 OpenAPI 中，且错误码为枚举。

## 7. UI 设计

### 7.1 入口

- 位置：`AgentWorkspace` 操作行，“选择报告”与“返回工作台”之间。
- 控件：`controls::AgentButton`，`Variant::Ghost`，文案“模型仪表盘”。
- 接入：`AgentWorkspace::SetDashboardHandler(std::function<void()>)`；按钮点击只调用回调。
- 始终可用：即使 sidecar 处于 Disabled，也允许预先配置 Key；对话框状态区显示服务状态。
- 窄宽度：沿用操作行现有折行/缩短策略；`AgentLayoutPolicy` 增加对应断点单测，保证三个按钮不重叠。

### 7.2 对话框

标题“模型与 API 仪表盘”，宿主侧 `wxDialog`，四个数据区块放在可滚动内容区中，底部固定状态行与关闭按钮：

1. **服务状态**：sidecar 状态（disabled/starting/ready/backing-off/failed）、`model.available`、provider、模型名、limits、Key 来源、最近错误（去敏）。
2. **API Key**：掩码输入框（不预填、保存后清空）、模型名输入、只读端点、按钮“保存并重启”“清除本机 Key”“刷新”；按钮使用 Agent 自绘 `AgentButton`。
3. **账户余额**：币种、总额、赠金、充值额、查询时间；失败显示固定原因码。
4. **本机用量**：累计 / 近 30 天调用次数、失败次数、prompt/completion/total tokens；显示“统计起点”和“从本版本开始记录，无历史回填”；`tokens_unavailable_calls` 提示。
5. **布局与主题**：四个区块使用 `AgentCardHost` 卡片；背景取 Agent `pageBg`，输入框取 Agent surface/input 色；字体取 `AgentTypography`；`UiTheme::Subscription` 跟随浅/深色切换。窗口变矮时内容区滚动，不裁剪余额/用量卡片。

### 7.3 交互流程

1. 打开：显示缓存状态；Ready 时在同一个后台任务里顺序查询 usage、balance，非 Ready 不发请求。
2. 保存：本地校验 → DPAPI 写入 → `Stop()` → `SetModelSettings()` → `Start()`。重启期间 Key 说明行显示“已保存，正在重启 Agent 服务…”；Ready 后改为“已保存，Agent 服务已重启。”，失败/超时/未启用分别显示对应终态并停止轮询。
3. 清除：确认后删除密文 → 重启 sidecar → 状态显示未配置/规则模式。
4. 刷新：重新查询 usage/balance；请求带序号，旧响应丢弃；完成后底部状态行显示“用量与余额已更新。”或具体失败原因。
5. 关闭：后台请求结果在对话框销毁后丢弃（generation + 指针防护）。

## 8. 错误处理

| 场景 | 表现 |
|---|---|
| Key/模型名/端点校验失败 | 字段内联错误，不保存、不重启 |
| DPAPI 加密/写入失败 | `key_store_unavailable` / `key_store_write_failed`；旧配置保留；不回退明文 |
| 非 Windows 保存 | 按钮禁用，提示使用 `SIGFLOW_EDU_MODEL_API_KEY` |
| sidecar 未就绪/重启失败 | 状态区显示 controller state + message；用量/余额显示“服务未就绪” |
| 用量文件损坏 | 跳过坏行并显示 `skipped`；其余统计正常 |
| 余额 401/网络/超时/非 JSON | 对应固定原因码；本地用量照常显示 |
| 过期查询返回 | 按请求序号 + generation 丢弃 |
| capabilities 与宿主来源不一致 | 显示“外部/未知”，以 capabilities 的可用性为准 |

## 9. 安全

- Key 只在内存与 DPAPI 密文中；不进入日志、tooltip、状态文案、错误消息。
- 界面不显示 Key 的任何片段；只显示“已配置/未配置”和来源。
- 两个新接口沿用 loopback + UI token；不新增写接口。
- 余额响应不含 Key、原始上游正文、绝对路径。
- 保存后清空输入框；不做剪贴板日志。
- 非 Windows 不提供明文持久化回退。

## 10. 测试计划

### C++（非 GUI）

- `AgentModelSettings`：字段校验；优先级（环境变量 > 存储 > 嵌入）；Windows DPAPI 假 Key 往返（磁盘无明文）；原子写入失败保留旧配置；清除；非 Windows 返回 `unsupported_platform`。
- `AgentApiClient`：usage/balance 响应信封解析；错误码；401；空统计。
- `AgentDashboardController`：过期结果丢弃；generation 变化失效；回调线程安全。

### Python

- `usage.py`：记录/聚合/按日/裁剪/坏行/超大行；并发追加。
- `model_client.py`：`usage` 提取成功、缺失、非法；余额成功/401/超时/非 JSON；输出不含 Key。
- `http_server.py`：两个新路由鉴权、参数边界（days）、空数据、损坏存储。
- 契约：OpenAPI 与 fixture 校验。

### GUI/集成

- 夹具：按钮存在、可聚焦、tooltip/可访问名；窄宽度三按钮不重叠；点击触发回调。
- 对话框：主题、Tab 顺序、Esc、无 Key 明文；保存/清除流程（假实现）。
- 联调：真实 sidecar Ready → 保存假 Key → 重启 → generation 变化 → 状态刷新；用现有 mock 模型服务验证一次调用后 usage 增长；余额接口用 mock HTTP 服务验证成功与失败分支。

## 11. 实施顺序

1. Python：usage 采集 + 两个接口 + OpenAPI/fixture + 测试。
2. C++：`AgentModelSettings` + `AgentServiceController::SetModelSettings` + 非 GUI 测试。
3. C++：`AgentApiClient::GetUsage/GetBalance` + `AgentDashboardController` + 测试。
4. UI：`AgentDashboardDialog` + 操作行按钮 + MainFrame 接线 + GUI 测试。
5. 联调、文档补充、全量构建与测试。

每一步独立可验证；第 1 步不依赖 GUI，第 2 步不依赖 sidecar 新接口。

## 12. 风险与缓解

| 风险 | 缓解 |
|---|---|
| DPAPI 在受限账户不可用（历史出现 0x2） | 固定错误码、保留旧配置、不退化明文；在正常用户上下文验证 |
| sidecar 重启竞态 | 使用现有有界 `Stop()`；重启后由 generation 防护丢弃旧请求 |
| usage 文件无限增长 | 90 天裁剪 + 单行/文件上限 |
| 余额接口变化 | 只依赖官方文档端点；失败固定降级，不影响本地用量 |
| 多实例共享数据目录 | 追加写 + 坏行容错；聚合只读，不修改历史 |
| 操作行窄宽度拥挤 | `AgentLayoutPolicy` 断点单测 + 截图验收 |

## 13. 待确认

无阻塞项。实现时已按官方文档复核 `GET /user/balance` 的字段命名（`is_available`、`balance_infos[]`）；若上游字段后续变化，由 sidecar 归一化层吸收，C++ 契约不变。
