> 负责人：js工
> 类型：交接
> 更新：2026-11-09
> 状态：进行中
> 阶段：V11

# js工 · 交接

> 只写 **cpp 需要配合/确认的事**。我们已完成的能力见 [history-js.md](history-js.md)，
> 计划见 [plan-js.md](plan-js.md)。

## ① 待对方配合

| ID | 事项 | 期望 |
|---|---|---|
| H-JS-002 | wave 草案 3 个细节 | 回复后冻结 D10：① `open` 响应字段补进草案；② `initial_value` 与 begin 处跳变重复，改为“严格早于 begin”或写明去重；③ 错误码拆分 `FILE_NOT_FOUND`/`NOT_OPEN`/`RESPONSE_TOO_LARGE` 并定义超限/越界行为 |
| H-JS-003 | 引脚字段 | `target-profiles/*.pins.json` 供 UI 与校验共用；请确认还需哪些字段 |
| H-JS-005 | 文档规则 | plan / handoff / history 三份已执行，cpp 侧同步 |
| H-JS-007 | 约束/仿真字段 | 字段若调整请提前通知，避免 V1/V2 返工 |
| H-JS-008 | capture CLI/JSONL | 补请求字段（device_id/depth/trigger/target_count/timeout）、事件（window/complete/cancel）、取消命令与错误码；`device_id` 建议用 `usb_serial`（无则 `path`），Node 按此做租约 |

## ② 待定接口

| 编号 | 事项 | 冻结时机 |
|---|---|---|
| D10 | `wave serve` / `wave signals|query` 字段 | H-JS-002 回复后 |
| D11 | `capture` 的 CLI/JSONL 与设备租约 | C++ 接线后 |
| D12 | target / validate 字段 | 引脚联调前 |

## ③ 已知风险

- capture 只有持久化文件格式，没有 CLI/JSONL；Node 目前只能做 `devices list`。
- 波形 3 个细节未冻结；实现已按“去重 + 错误码兼容映射”处理。
- V3 串联暂缓：本机缺 yosys / nextpnr / gowin_pack；真实工具链未跨端验收。
- Agent 需 Python 包可导入；未配模型 Key 时只做规则讲解。
- 无 HTTPS；已有 CSRF 同源检查与登录限流。

## ④ 最近变更（只记影响接口的）

- 2026-11-08 V8：新增 `/api/projects/{id}/wave/artifacts|open|signals|query|close` 与每用户 `wave serve` worker。
- 2026-11-08 V9：新增 `GET /api/devices`，透传 `devices list`。
- 2026-11-08 V11：新增工程 `export` / `import`（与 C++ 无关）。

## ⑤ 契约评审结论

| 契约 | 结论 | 需明确 / 修改 |
|---|---|---|
| analysis-v1（H-CPP-009） | 通过 | `diagnostics[].file` 应与请求一致；超限固定 code；叶节点 `children` 可省略需写明 |
| target-validation（H-CPP-012） | 有条件通过 | `line` 基准（实现 0 起始，JS 已 +1）；`content` 语义；非法 CST 的退出码 |
| nextpnr 指标（H-CPP-018） | 待澄清 | `resources` 键名；`percent` 量纲；`classified_errors.line` 指向；指标挂载位置 |
| pack 参数（H-CPP-019） | 通过 | `output` 明确相对 `artifacts/`；位流产物 schema 名 |
| simulation（H-CPP-021） | 有条件通过 | `sim_exe` 前缀校验已加；`duration_ticks` 字符串格式；testbench 限制 |
| wave（H-CPP-010） | 有条件通过 | 见 H-JS-002 的 3 点 |
| devices/capture（H-CPP-026） | 部分通过 | `devices list` 可用；capture 缺 CLI/JSONL、租约键、错误码（见 H-JS-008） |

## ⑥ 对 H-CPP 的回复

| 请求 | 回复 |
|---|---|
| H-CPP-007 取消测试 | 正常取消已覆盖；早期取消按确认接受“可能无报告”，未单独断言 |
| H-CPP-009/012/018/019/021 评审 | 已回复，见 ⑤ |
| H-CPP-010 波形评审 | 已回复，见 ⑤ / H-JS-002 |
| H-CPP-011 flash 确认/租约 | 建议一次性 `confirm_token` + `device_id`，按板卡串行化；等 P4 |
| H-CPP-013 Node 22 | 已完成：v22.23.4 下 `node --test` 28/28 |
| H-CPP-014 lint 工程根 | 已修，structure/lint 都传 `--project-root` |
| H-CPP-015 网表路径/归属 | 现约定 `.sigflow/agent/jobs/synth/<id>/artifacts/…`；归属校验等 V3 |
| H-CPP-026 capture 评审 | 已回复，见 ⑤ / H-JS-008 |
