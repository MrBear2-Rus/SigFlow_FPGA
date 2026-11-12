> 负责人：js工
> 类型：实况
> 更新：2026-11-08
> 状态：进行中
> 阶段：V12

# js工 · 实况

> 本文件描述"当前代码实际是什么样"，随模块变化**原地改写**，不按计划、不按日期追加。
> 计划看 [plan-js.md](plan-js.md)，待办与风险看 [handoff-js.md](handoff-js.md)，架构看 [README](../README.md)。

## ① 进程与入口

- `backend/nodejs/src/index.js`：唯一常驻进程。读 `config.txt` → 装配 `createApp` → 监听 → 按需启动 Python sidecar；收到 `SIGINT`/`SIGTERM` 先取消在跑的任务、停止 Agent 再退出。
- 每次 Job 或分析调用 spawn 一次 `sigflow-tool`。路径按 `SIGFLOW_TOOL_BIN` → `config.tools.sigflow_tool` → `web/build/bin` 解析。
- Agent 由 `[agent] enabled` 或 `SIGFLOW_AGENT_ENABLED=1` 开启，每用户一个 sidecar（D-17）。

## ② Node 模块

| 路径 | 职责 | 当前行为 |
|---|---|---|
| `src/http/server.js` | 路由、SSE、静态文件、鉴权 | 公开：health / 注册 / 登录；`/api/*` 未登录 401；`/api/v1/*` Agent 网关；`/api/agent/*` 代理；跨站 Origin 拒绝、登录 5 次锁定 5 分钟 |
| `src/users/` | 用户与会话 | `database/users.json`（scrypt + 原子写）；会话 token 哈希存 `database/sessions.json`，HttpOnly Cookie，默认 24h |
| `src/projects/project-store.js` | 工程与文件 | `<uid>/<pid>` 增删；`updatedAt` 随写入刷新；模板 `blank` / `sample`；文件树 ≤2000 项/8 层；文本扩展名、≤2 MiB；画布 `.sigflow/canvas.json` |
| `src/jobs/job-manager.js` | 队列与并发 | 上限取自配置；`Queued → Running → 终态`；取消用 `AbortSignal` |
| `src/jobs/tool-runner.js` | 单次 run 调用 | detached spawn、stdin 写 JSON 后关闭、有界 JSONL、SIGTERM→宽限→SIGKILL、读 `reports/report.json` |
| `src/jobs/job-dir.js` | Job 目录 | 预建空目录并拒绝复用；报告/产物路径解析 |
| `src/jobs/job-id.js` | ID | UUID；字符集与 C++ 侧一致 |
| `src/projects/target-store.js` | 目标板数据 | `static/target-profiles/*.json` 列表与 profile+pins；id 白名单 |
| `src/analysis/tool-command.js` | 通用 CLI | 参数由调用方给，JSONL、超时、输出上限 |
| `src/analysis/analysis-runner.js` | structure / lint | 一次性 CLI，自动带 `--file` 与 `--project-root`；退出码 3 → 501（仅旧构建） |
| `src/agent/manager.js` | 每用户 sidecar | 惰性创建、空闲回收、上限淘汰、gateway_token → user |
| `src/agent/supervisor.js` | sidecar 托管 | bootstrap 一行、ready 握手、健康轮询、退避重启、优雅停止 |
| `src/wave/worker.js` | 每用户 `wave serve` | JSONL 按 id 关联、15s 超时、64 KiB 请求 / 1 MiB 行上限、崩溃重启 |
| `src/wave/manager.js` | wave worker 池 | 惰性创建、空闲回收、上限淘汰 |
| `src/agent/gateway.js` | Gateway 兼容路由 | `/api/v1/health`、`projects/{id}/context`、`jobs/{id}/report`；Bearer + edu.api.v1 信封 + 2 MiB |
| `src/util/` | 基础设施 | `config.txt` 解析、JSONL 有界解析、日志 |

## ③ 前端页面

| 文件 | 当前内容 |
|---|---|
| `pages/index.html` + `scripts/landing.js` | 落地页：产品介绍、登录/注册、已登录态 |
| `pages/projects.html` + `scripts/projects.js` | 工程管理：列表、新建（空白/示例）、打开、删除、导出/导入 JSON 包 |
| `pages/workspace.html` + `scripts/workspace.js` | 工作区：六区；文件树、元件库、画布、CodeMirror、结构树/诊断、属性、引脚绑定、波形、终端、助手面板 |
| `pages/fpga-tool.html` + `scripts/fpga-tool.js` | FPGA 工具：Yosys / nextpnr / 打包 / 仿真 任务控制台（提交、SSE、取消、产物、网表与仿真程序联动） |
| `pages/tracebridge.html` + `scripts/tracebridge.js` | TraceBridge：设备枚举（串口/FTDI）+ 契约/运行时/双轨波形骨架 |
| `pages/debug-contract.html` + `scripts/debug-contract.js` | 调试契约骨架：表单预览，保存待 P8 |
| `pages/components.html` + `scripts/components-demo.js` | 组件演示页（验收用） |
| `scripts/lib/` | `api` / `tabs` / `splitter` / `dialog` / `toast` / `icons` |
| `scripts/workspace/` | `canvas-editor` / `editor` / `file-tree` / `analysis` |
| `scripts/window-common.js` | 独立窗口的登录校验、工程标题、关闭按钮 |
| `backend/nodejs/scripts/smoke-ui.mjs` | 浏览器冒烟：登录→工程→工作区→独立窗口，收集 JS 异常 |
| `backend/cpp/contracts/` | C++ 侧契约（analysis / run / target / simulation），JS 已评审并记录意见 |
| `styles/` | `theme` / `components` / `landing` / `projects` / `workspace` / `window` |
| `vendor/codemirror/` | CodeMirror 5.65.16（MIT），离线可用 |
| `assets/` | `canvas_elements.json`、`component-icons.json`、`icons/`（ui 13 + signal 10 + components 111 + manifest） |

## ④ 数据落点

| 数据 | 位置 | 说明 |
|---|---|---|
| 用户表 | `backend/database/users.json` | gitignore；仅存 salt/hash |
| 会话 | `backend/database/sessions.json` | gitignore；SHA-256(token) → userId/expiresAt |
| Agent 数据 | `backend/database/agent/<uid>/` | 每用户 sidecar 的 `data_directory` |
| 工程 | `backend/userprojects/<uid>/<pid>/` | gitignore；元数据 `sigflow.project` |
| 画布 | `<project>/.sigflow/canvas.json` | Node 校验后原子写 |
| 约束 | `<project>/constraints/<target>.cst` | 引脚绑定页写入 |
| 任务 | `<project>/.sigflow/agent/jobs/<type>/<id>/` | C++ 写 request/manifest/artifacts/logs/reports；仿真产物 `sim_main` / `waveform/wave.vcd` |

## ⑤ 明确未实现

- 波形：worker 与面板已接，支持游标与滚轮缩放；错误码按新契约映射（`NOT_OPEN`→409 等）；多 VCD 对比未做。
- TraceBridge：只有 devices list；capture/硬件按 D-20 暂缓，转虚拟板卡方向。
- 工程导入只接受 `sigflow.export.v1` JSON，不支持 zip/二进制产物。
- FPGA 串联（synth→pnr→pack 自动传参）暂缓（缺工具链）；约束只做 CST，未接 `validate pins`。
- 画布：无电气规则校验、连线拖动、对齐吸附；撤销只在当前页面会话内，刷新即清空。
- 编辑器：无代码折叠、无自动补全；诊断只做行标记，未做逐字符下划线。
- Agent：会话/事件不持久化；无模型 Key 时规则降级；助手面板只做"解释当前文件"。
- 安全：无 HTTPS、CSRF、登录限流；注册开放。
- 没有浏览器级自动化测试；验证到 HTTP 层 + 人工截图。

## ⑥ 验证方式

| 范围 | 命令 | 当前结果 |
|---|---|---|
| C++ | `ctest --test-dir build` | 全绿（数量随用例增加） |
| Node | `cmake --build build --target web_js_test` | 全绿（Node 26 与 Node 22.23.4 均为 28/28） |
| 冒烟 | `cmake --build build --target web_smoke` | 12/12，需 chromium 与服务已启动 |
| 手动 | `cmake --build build --target run` → `http://127.0.0.1:8080` | 可用；synth 需假 yosys 或真实工具 |

## ⑦ 阶段交付

| 阶段 | 交付 |
|---|---|
| J0–J5 | 任务链路、HTTP/SSE、用户/工程/文件、编辑器、画布 |
| U1–U7 | 六页面 UI、浅色主题、伪停靠、三个独立窗口、断链与文档检查 |
| J6b | Agent 方案 A：每用户 sidecar、Gateway、代理、助手面板 |
| V1 | 引脚绑定、`.cst` 读写、`validate cst` |
| V2 | 仿真页：`sim.build` / `sim.run` |
| V3 | ⏸ 等 yosys / nextpnr / gowin_pack |
| V4 | 编辑器页签、查找替换、诊断行标记 |
| V5 | 画布缩放/框选/撤销、元件属性 |
| V6 | 文件移动/删除、会话持久化 |
| V7 | 浏览器冒烟 12 项、401 与网络错误处理 |
| V8 | wave worker、`/api/wave/*`、波形面板（信号/查询/绘制） |
| V9 | `/api/devices`、TraceBridge 设备列表 |
| V10 | 波形游标/滚轮缩放、CSRF 同源检查、登录限流 |
| V11 | 工程 export/import、Node 22 复验（28/28） |
