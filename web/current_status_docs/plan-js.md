> 负责人：js工
> 类型：规划
> 更新：2026-11-01
> 状态：进行中
> 阶段：J1 待启动

# js工 · 规划

## ① 当前目标

让 Node 主进程跑通 `synth` 端到端：建 Job 目录 → spawn `sigflow-tool` → 解析 JSONL →
读取 `reports/report.json` 与产物；先脚本验证，再接 HTTP/SSE。

## ② 阶段与验收

| 阶段 | 内容 | 验收标准 | 状态 |
|---|---|---|---|
| J0 | 目录骨架、单文件 CMake、`config.txt`、CLI/Node 骨架 | `cmake` 构建通过，Node 能读配置 | ✅ |
| J1 | Job 调度与 C++ 联调（`jobs/` + `util/`） | synth 的成功 / 失败 / 取消三条路径跑通，能读到报告与产物 | ⬜ 待启动 |
| J2 | HTTP/SSE、会话、静态文件 | 浏览器能提交任务并看到实时日志 | ⬜ |
| J3 | 工程管理、文件树、读写 | 建 / 开 / 存工程，用户与工程目录隔离 | ⬜ |
| J4 | 前端编辑器与画布 | 编辑、结构树、诊断展示；给出 wave UI 需求 | ⬜ |
| J5 | 波形、Agent 托管、插件 | 波形查询代理；Python agent 零改动接入；插件注册表 | ⬜ |

## ③ 下一步

1. `jobs/`：jobId 生成、空目录、`spawn`、JSONL 有界解析、状态合成、取消。
2. `util/`：`config.txt` 解析与日志。
3. 联调脚本：`synth` 成功与取消两条路径，断言 `reports/report.json` 与 `artifacts/`。

## ④ 阻塞与依赖

- 真实 Yosys 环境（handoff-cpp H-CPP-006）；没有时只能跑假工具。
- `wave serve` 字段需要 js 侧先给 UI 需求（H-CPP-004 ↔ H-JS-002）。

## ⑤ 相关决策

跨角色决策见 `../../README.md`「决策记录」；C++ 契约细节见
`../backend/cpp/contracts/run-v1/`。
