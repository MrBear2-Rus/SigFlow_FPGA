> 负责人：cpp工
> 类型：规划
> 更新：2026-11-03
> 状态：进行中
> 阶段：P3 联调、P4 工具、P5 校验与 P6 解析

# cpp工 · 规划

## 当前目标

按 [README 的 D-11](../README.md#决策记录)，先完成 Node 与 C++ 的 synth 端到端联调，
确认 P3 的部署环境与验收结果；按用户要求并行推进 P4 的 nextpnr、pack、sim、P5 校验与 P6 解析准备。当前实现状态见 [history-cpp.md](history-cpp.md)，
跨角色待办见 [handoff-cpp.md](handoff-cpp.md)；本轮不修改 JS 方的计划或代码。

## 阶段与验收

| 阶段 | 目标 | 验收条件 | 状态 |
|---|---|---|---|
| P0–P2 | 平台层、任务契约和单任务执行 | 历史本地回归通过；当前版本的跨端验收另计 | 本地已实现 |
| P3-A | 拉取后的代码与工具基线 | Linux 环境完成构建、C++ 测试，记录工具版本与跳过项 | C++ 12/12 通过；Node 目标待修复 |
| P3-B | synth 跨端联调 | 从 Node 提交，经 C++ 产出报告，验证事件、状态、下载及失败路径 | 部分通过；早期取消待对齐 |
| P3-C | Linux 原生 Yosys 验收 | 最小 RTL 经原生工具成功综合，网表与报告一致 | 主 contributor 已完成（用户确认） |
| P4 | pnr、pack、sim、flash | synth→pnr→pack、仿真及各自失败路径；flash 另验确认与租约 | nextpnr、pack、sim 假工具通过；flash 待实现 |
| P5 | target、validate | 共享板卡数据与约束校验正反例通过 | 本地实现；接口评审和数据核验待完成 |
| P6 | structure、lint | 双方评审分析契约，离线构建并验证定位与 Node 接入 | 本地 CLI 实现；Node 工程根接入和契约评审待完成 |
| P7–P9 | 波形、设备和部署 | 分别完成协议、工具/硬件、独立部署及跨端验收 | 待规划 |

P3-C 按用户反馈由主 contributor 完成，工具版本和原始报告未在当前仓库找到；
P3-B 的本地跨端测试使用假 Yosys，与该原生验收分别记录。

## 下一步

1. 按用户确认的早期取消处理方式，与 JS 方完成跨端测试调整及测试入口修复。
2. 向主 contributor 获取原生验收的工具版本与记录链接；不重复执行已完成的验收。
3. 用原生 nextpnr 验证日志、报告及 synth→pnr 链路；与 JS 方评审指标草案。
4. 与 JS 方评审 [P5 接口草案](../backend/cpp/contracts/target-validation-draft.md)，并核验旧板卡数据；评审 [analysis-v1 草案](../backend/cpp/contracts/analysis-v1/README.md)与 [C++ 实现补充](../backend/cpp/contracts/analysis-v1/cpp-implementation-draft.md)。
5. 用 Linux 原生 gowin_pack 验证 synth→pnr→pack 链路，与 JS 方评审打包接口；用 Linux 原生 Verilator 验收仿真，flash 另行对齐确认、租约及硬件环境。

## P4 首批依赖盘点

- nextpnr provider 与日志解析已迁入；指标结构见 [草案](../backend/cpp/contracts/run-v1/nextpnr-metrics.schema.json)。
- 旧 provider 的输入、默认值、输出及工具发现需逐项对照旧实现；实现必须约束输入与产物路径，移除旧插件注册依赖。
- 旧实现优先查找 `nextpnr-himbaechel`，兼容 `nextpnr-gowin` 名称；两者参数兼容性需以实际版本验证。
- nextpnr provider 的假工具、日志解析、错误、超时、取消和模拟 synth→pnr 链路已验证；本机未发现 Linux nextpnr，真实链路仍待验收。
- Node 侧如何将上游已验证网表映射到下一任务输入，以及报告指标展示，需与 JS 方联调确认。
- Gowin 打包已接入路径约束、取消、超时及产物登记；假工具的 synth→pnr→pack 链路通过，[参数草案](../backend/cpp/contracts/run-v1/gowin-pack-params.schema.json)待评审。
- 仿真 build/run 已接入，旧 testbench 解析为简化实现；[仿真草案](../backend/cpp/contracts/run-v1/simulation-draft.md)及真实 Verilator 环境待评审验收。

## 阻塞与依赖

- 早期取消及测试入口问题见 [H-CPP-007、H-CPP-013](handoff-cpp.md)；主 contributor 已完成原生 Yosys 验收。
- P6 工程根接入、契约评审与 P7 波形需求尚需对齐；flash 确认和设备租约在 P4 前对齐。
- 跨角色结论以 [README 决策记录](../README.md#决策记录) 为准；实现时遵循 `web/.clang-format`。
