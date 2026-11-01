> 负责人：js工
> 类型：实况
> 更新：2026-11-01
> 状态：进行中
> 阶段：J5

# js工 · 实况

> 本文件描述"当前代码实际是什么样"，随模块变化**原地改写**，不按计划、不按日期追加。
> 计划看 [plan-js.md](plan-js.md)，待办与风险看 [handoff-js.md](handoff-js.md)，架构看 [README](../README.md)。

## ① 进程与入口

- `backend/nodejs/src/index.js`：唯一常驻进程。读 `config.txt` → 装配 `createApp` → 监听 `server.listen_host:listen_port`；收到 `SIGINT`/`SIGTERM` 先取消在跑的任务再退出。
- 每次 Job 或分析调用 spawn 一次 `sigflow-tool`。可执行文件按 `SIGFLOW_TOOL_BIN`（CMake 的 `run` / `web_js_test` 会注入）→ `config.tools.sigflow_tool` → `web/build/bin` 的顺序解析。

## ② Node 模块

| 路径 | 职责 | 当前行为 |
|---|---|---|
| `src/http/server.js` | 路由、SSE、静态文件、鉴权 | 公开：health / 注册 / 登录；其余 `/api/*` 未登录一律 401；其余见 README 7.3 |
| `src/users/` | 用户与会话 | `database/users.json`（scrypt 加盐、原子写）；内存 token + HttpOnly Cookie，默认 24h |
| `src/projects/project-store.js` | 工程与文件 | `<uid>/<pid>` 增删；文件树 ≤2000 项/8 层；文本扩展名、单文件 ≤2 MiB；画布存 `.sigflow/canvas.json` |
| `src/jobs/job-manager.js` | 队列与并发 | 上限取自配置；`Queued → Running → 终态`；取消通过 `AbortSignal` |
| `src/jobs/tool-runner.js` | 单次 run 调用 | `detached` spawn、stdin 写 JSON 后关闭、有界 JSONL、SIGTERM→宽限→SIGKILL、退出后读 `reports/report.json` |
| `src/jobs/job-dir.js` | Job 目录 | 预建空目录并拒绝复用；报告与产物路径解析 |
| `src/jobs/job-id.js` | ID | UUID；字符集与 C++ 侧一致 |
| `src/analysis/analysis-runner.js` | structure / lint | 一次性 CLI；退出码 3 → HTTP 501；成功时返回 `result.data` |
| `src/util/` | 基础设施 | `config.txt` 解析、JSONL 有界解析、日志 |

## ③ 前端页面

| 文件 | 当前内容 |
|---|---|
| `pages/index.html` + `scripts/console.js` | 登录/注册、工程选择与新建、文件树、CodeMirror 编辑器、结构树/诊断面板、任务提交与 SSE 日志 |
| `pages/canvas.html` + `scripts/canvas.js` | 元件库（6 种门）、拖拽、引脚连线、删除、按工程保存 |
| `styles/app.css` | 暗色主题、三栏布局、CodeMirror 暗色适配 |
| `vendor/codemirror/` | CodeMirror 5.65.16（MIT，含 LICENSE），离线可用 |

## ④ 数据落点

| 数据 | 位置 | 说明 |
|---|---|---|
| 用户表 | `backend/database/users.json` | gitignore；仅存 salt/hash |
| 工程 | `backend/userprojects/<uid>/<pid>/` | gitignore；元数据 `sigflow.project` |
| 画布 | `<project>/.sigflow/canvas.json` | 由 Node 校验后原子写 |
| 任务 | `<project>/.sigflow/agent/jobs/<type>/<id>/` | `request.json` / `manifest.json` / `artifacts/` / `logs/` / `reports/` 由 C++ 写 |

## ⑤ 明确未实现

- 波形（`src/wave/`）、Agent 托管（`src/agent/`）、设备租约（`src/devices/`）、插件（`src/plugins/`）：只有目录。
- 画布：无缩放/平移、多选/框选、撤销、属性编辑。
- 编辑器：无搜索替换、折叠、诊断内联（结构树/诊断数据还依赖 C++ P6）。
- 没有浏览器级自动化测试；验证只到 HTTP 层。

## ⑥ 验证方式

| 范围 | 命令 | 当前结果 |
|---|---|---|
| C++ | `ctest --test-dir build` | 4/4 |
| Node | `cmake --build build --target web_js_test` | 11/11 |
| 手动 | `cmake --build build --target run` → `http://127.0.0.1:8080` | 可用（synth 需假 yosys 或真实工具） |
