> 负责人：cpp工
> 类型：交接
> 更新：2026-11-01
> 状态：有待确认项
> 阶段：P3 → P4

# cpp工 · 交接

> 由原 `IMPLEMENTATION.md`、`P2_HANDOFF.md`、`P3_HANDOFF.md`（已删除，历史见 git）压缩合并；协议细节以
> `../backend/cpp/contracts/run-v1/` 为准，本文件不复述字段。

## ① 待对方配合

| ID | 事项 | 期望 |
|---|---|---|
| H-CPP-001 | Node 建 Job 目录 | 每次提交前建**空目录** `<project>/.sigflow/agent/jobs/<jobType>/<jobId>`；不可复用，重试必须换新 jobId |
| H-CPP-002 | 进程与取消 | `detached` spawn；参数写完后关闭 stdin；取消先 `kill(-pid, SIGTERM)`，超时再 `SIGKILL` |
| H-CPP-003 | 解析约定 | 只解析有界 JSONL；`artifact` 事件是候选；无 `result` 或残行按失败处理 |
| H-CPP-004 | wave 对外字段 | 由 js 侧给出 UI 需求，P7 前一起冻结 |
| H-CPP-005 | flash 确认 | 沿用 `require_confirm` 语义；具体传递形式（参数 / 一次性 token）待定 |
| H-CPP-006 | 真实工具环境 | 需要一台有 Linux 原生 Yosys 的环境完成 P3 验收 |

## ② 待定接口

| 编号 | 事项 | 冻结时机 |
|---|---|---|
| D10 | `wave serve` 字段、数值编码、EOF/损坏 JSON 处理 | P7 |
| D11 | structure / lint / validate / target / devices / capture 的结果 payload | 各命令实现时 |
| D12 | 限额默认值、日志截断行为、波形性能阈值 | 联调前 |

## ③ 已知风险

- `SIGKILL` 后没有最终报告，Node 必须扫描残留 `Running` 目录。
- `.runtime-claim` 常驻不删，Job 目录不可复用；重试只能换新 jobId。
- 真实 Yosys、板卡、Node 联调均未验收；假工具通过 ≠ 业务可用。
- `jsonschema` 缺失时契约测试被跳过（只打印 STATUS），CI 应强制安装。
- 外层工具的进程组与 CLI 进程组不同，`kill(-cliPid)` 不能证明后代已回收。

## ④ 最近变更

- 2026-11-01 P3：Yosys provider；假工具 17 个场景；Windows Yosys 经测试适配器验证。
- 2026-11-01 P2：JobRuntime / RunCommand / JobPaths；51 个场景，4/4 CTest。
- 2026-10-31 P1：`contracts/run-v1` 四份 schema + 五组 fixture。
- 2026-10-31 P0：平台层、Toolchain、Logger、nlohmann 离线依赖。
