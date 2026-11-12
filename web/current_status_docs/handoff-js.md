> 负责人：js工
> 类型：交接
> 更新：2026-11-09
> 状态：进行中
> 阶段：V12

# js工 · 交接

> 只写 **cpp 需要配合/确认的事**。我们已完成的能力见 [history-js.md](history-js.md)，
> 计划见 [plan-js.md](plan-js.md)。

## ① 待对方配合

| ID | 事项 | 期望 |
|---|---|---|
| H-JS-002 | wave 契约 | 已确认：`initial_value` 严格早于 begin、`open` 响应与错误码已写入草案；Node 已映射 `NOT_OPEN→409`、`FILE_NOT_FOUND→404`、`RESPONSE_TOO_LARGE→413`。请主 contributor 冻结 D-18 |
| H-JS-003 | 引脚字段 | `target-profiles/*.pins.json` 供 UI 与校验共用；请确认还需哪些字段 |
| H-JS-005 | 文档规则 | plan / handoff / history 三份已执行，cpp 侧同步 |
| H-JS-007 | 约束/仿真字段 | 字段若调整请提前通知，避免 V1/V2 返工 |
| H-JS-008 | capture | 按 D-19/D-20 暂缓硬件采集；P8 启动时再评审 CLI/JSONL 与租约（建议 `usb_serial` 作 `device_id`） |

## ② 待定接口

| 编号 | 事项 | 冻结时机 |
|---|---|---|
| D10 | `wave serve` / `wave signals|query` 字段 | 已冻结（D-18） |
| D11 | `capture` 的 CLI/JSONL 与设备租约 | 按 D-20 暂缓，P8 启动时 |
| D12 | target / validate 字段 | 引脚联调前 |

## ③ 已知风险

- capture/硬件采集按 D-19/D-20 暂缓；Node 目前只有 `devices list`。
- 波形 3 个细节未冻结；实现已按“去重 + 错误码兼容映射”处理。
- V3 串联暂缓：本机缺 yosys / nextpnr / gowin_pack；真实工具链未跨端验收。
- Agent 需 Python 包可导入；未配模型 Key 时只做规则讲解。
- 无 HTTPS；已有 CSRF 同源检查与登录限流。

## ④ 最近变更（只记影响接口的）

- 2026-11-12 契约更新：wave/analysis/target/nextpnr/pack/sim 细节已按我方意见修订；Node 补齐 wave 错误码映射。
- 2026-11-08 V8：新增 `/api/projects/{id}/wave/artifacts|open|signals|query|close` 与每用户 `wave serve` worker。
- 2026-11-08 V9：新增 `GET /api/devices`，透传 `devices list`。
- 2026-11-08 V11：新增工程 `export` / `import`（与 C++ 无关）。

## ⑤ 契约评审结论

| 契约 | 结论 | 需明确 / 修改 |
|---|---|---|
| analysis-v1（H-CPP-009） | 通过（已确认） | 相对路径、固定超限 code、叶节点 `children` 可省略已写入 |
| target-validation（H-CPP-012） | 通过（已确认） | `line` 0 起始、`-1` 表示无定位、非法 CST 以 `valid=false` 返回 |
| nextpnr 指标（H-CPP-018） | 通过（已确认） | 资源键动态；`percent` 为百分数可超 100；错误行 1 起始；挂在 `report.metrics[].nextpnr` |
| pack 参数（H-CPP-019） | 通过（已确认） | `output` 相对 `artifacts/`、拒绝绝对路径；产物 schema `eda.bitstream.apicula.v1` |
| simulation（H-CPP-021） | 通过（已确认） | `duration_ticks` 十进制字符串；testbench 简化范围已写明 |
| wave（H-CPP-010） | 通过 | 错误码已映射，E2E 覆盖 close 后 409；等 D-18 冻结 |
| devices/capture（H-CPP-026） | 暂缓 | `devices list` 可用；capture 按 D-20 转虚拟板卡方向 |

## ⑥ 对 H-CPP 的回复

| 请求 | 回复 |
|---|---|
| H-CPP-007 取消测试 | 正常取消已覆盖；早期取消按确认接受“可能无报告”，未单独断言 |
| H-CPP-009/012/018/019/021 评审 | 已回复，见 ⑤ |
| H-CPP-010 波形 | 错误码已映射（NOT_OPEN→409 等），E2E 已补；请冻结 D-18 |
| H-CPP-011 flash 确认/租约 | 建议一次性 `confirm_token` + `device_id`，按板卡串行化；等 P4 |
| H-CPP-013 Node 22 | 已完成：v22.23.4 下 `node --test` 28/28 |
| H-CPP-014 lint 工程根 | 已修，structure/lint 都传 `--project-root` |
| H-CPP-015 网表路径/归属 | 现约定 `.sigflow/agent/jobs/synth/<id>/artifacts/…`；归属校验等 V3 |
| H-CPP-026 capture | 按 D-19/D-20 暂缓，P8 启动时再评审 |
