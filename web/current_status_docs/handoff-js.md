> 负责人：js工
> 类型：交接
> 更新：2026-11-03
> 状态：进行中
> 阶段：V3

# js工 · 交接

## ① 待对方配合

| ID | 事项 | 期望 |
|---|---|---|
| H-JS-002 | wave 协议 | 等 Bear2 的 `wave serve` 协议（D-18）；定稿后按信号属性 / 时间精度 / 分页实现前端 |
| H-JS-003 | 静态数据字段 | 引脚表需要 `target-profiles/*.pins.json` 的哪些字段；与 cpp 校验共用同一份 |
| H-JS-005 | 文档规则 | plan / handoff / history 三份文档已执行；cpp 侧保持同步 |
| H-JS-007 | 约束 / 仿真字段 | 我们按草案开始 V1/V2；字段若有调整请提前说，避免返工 |

## ② 待定接口

| 编号 | 事项 | 冻结时机 |
|---|---|---|
| D10 | `wave serve` 字段与数值编码 | Bear2 给出协议后 |
| D11 | nextpnr 指标 / pack 参数 / 仿真草案（H-CPP-018/019/021） | 对应命令联调前 |
| D12 | target / validate 草案（H-CPP-012） | 引脚绑定联调前 |

## ③ 已知风险

- **波形完全阻塞**：C++ P7 `wave serve` 未实现，前端只有空态。
- Agent 已按方案 A 实现（manager / gateway / proxy / 助手面板）；需 Python 包可导入，未配模型 Key 时降级为规则讲解，会话不持久化。
- 约束：只做 `validate cst`；`validate pins` 需要网表，尚未接；IO_TYPE 固定写 profile 默认值。
- 仿真：依赖本机 verilator/make/g++；本机 ccache 只读，需 `CCACHE_DISABLE=1`；VCD 只能下载，查看等 P7。
- V3 串联暂缓：本机缺 yosys / nextpnr / gowin_pack；冒烟需 chromium 与已启动服务。
- 画布：支持缩放/平移/框选/撤销，但连线仍只做"输出→输入"，无电气规则、无连线拖动与对齐吸附。
- 会话已持久化（`database/sessions.json`，只存 token 哈希），但仍无 HTTPS、CSRF、登录限流；注册开放。
- 文件读写限文本扩展名与 2 MiB；二进制（位流、VCD）需走产物接口。
- 端到端验证全部使用假 yosys / nextpnr；真实工具链未联调。
- `config.txt` 的 `[wave]` / `[devices]` 键暂无使用方。

## ④ 最近变更

- 2026-11-03 V7：新增 `scripts/smoke-ui.mjs`（12 项浏览器冒烟）与 `web_smoke` 目标；401 统一跳回落地页、网络错误提示。
- 2026-11-03 V6：文件重命名/移动/删除；会话持久化，重启不掉登录。
- 2026-11-03 V5：画布缩放/平移、框选多选、撤销/重做、元件标签与坐标编辑。
- 2026-11-03 V4：编辑器多文件页签、查找替换（Ctrl+F）、lint 诊断行标记；Ctrl+S 保存。
- 2026-11-03 V2：FPGA 工具页新增「仿真」标签，`sim.build` / `sim.run` 实跑通过（真实 verilator，产出 wave.vcd）。
- 2026-11-03 V1：目标板接口、CST 约束读写与 `validate cst`、右栏引脚绑定页。
- 2026-11-03 J6：Agent 方案 A 完成——按用户 manager、Gateway 三路由、UI 代理、右栏助手面板；真实 Agent 冒烟通过。
- 2026-11-03 U1–U7：六页面 UI 框架完成；旧控制台/画布页与深色样式移除；状态栏、页面断链检查进测试。
- 2026-11-03 拉取 cpp 99a4af9：Node 分析调用补 `--project-root`，analysis 测试改真实结果；`web_js_test` 改为显式文件列表。
- 2026-11-01 文档规范变更：`current_status_docs/` 每人三份（plan / handoff / history）。

## ⑤ 契约评审（2026-11-03）

| 契约 | 结论 | 需明确 / 修改 |
|---|---|---|
| analysis-v1（H-CPP-009） | 通过 | `diagnostics[].file` 应与请求文件一致；超限错误固定 code（如 `FILE_TOO_LARGE` / `OUTPUT_LIMIT`）；叶节点 `children` 可省略需写明 |
| target-validation（H-CPP-012） | 有条件通过 | `diagnostics[].line` 基准（实现为 0 起始，JS 已 +1 显示）；`content` 语义；非法 CST 是 `valid=false` 还是退出 4；默认 IO_TYPE 在 pins.json（JS 已修正） |
| nextpnr 指标（H-CPP-018） | 待澄清 | `resources` 键名集合未定义；`percent` 量纲（0–100）；`classified_errors[].line` 指日志行还是源码行；指标挂载在 report 的哪个字段 |
| pack 参数（H-CPP-019） | 通过 | `output` 明确为相对 Job `artifacts/` 的路径；位流产物 schema 名补进 report |
| simulation（H-CPP-021） | 有条件通过 | `sim_exe` 由 Node 校验 `.sigflow/agent/jobs/sim.build/<id>/artifacts/` 前缀（已加，非法返回 400 `INVALID_SIM_EXE`）；`duration_ticks` 字符串格式；testbench 限制已在 UI 提示 |

评审不阻断 V1/V2 现有实现；字段调整请按 H-JS-007 提前通知。

## ⑥ 对 H-CPP 的回复（2026-11-03）

| 请求 | 回复 |
|---|---|
| H-CPP-007 取消测试 | 正常取消已覆盖（SIGTERM、终态、无残留进程）；早期取消按确认接受“可能无报告”，未单独加断言 |
| H-CPP-009 analysis 评审 | 已回复，见 ⑤ |
| H-CPP-010 波形需求 | 需要：信号树（hier/name/width/kind）、整数时间 + timescale、变化点查询 `[start,end]`、分页 `limit/next_cursor`、总时长与信号数；建议 1ns 精度起步 |
| H-CPP-011 flash 确认/租约 | 建议：用户确认后 Node 发一次性 `confirm_token` + `device_id`，设备租约按板卡串行化；等 P4 再定字段 |
| H-CPP-012 目标板评审 | 已回复，见 ⑤ |
| H-CPP-013 Node 22 测试入口 | 已改为显式文件列表；本机 Node 26 通过，Node 22 未复验 |
| H-CPP-014 lint 工程根 | 已修，structure/lint 都传 `--project-root` |
| H-CPP-015 网表路径/归属 | 现约定 `.sigflow/agent/jobs/synth/<id>/artifacts/…`；归属与摘要校验等 V3 联调补 |
| H-CPP-018 / 019 / 021 评审 | 已回复，见 ⑤；`sim_exe` 前缀校验已加 |
