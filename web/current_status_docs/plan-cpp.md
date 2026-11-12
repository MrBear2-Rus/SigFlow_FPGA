> 负责人：cpp工
> 类型：规划
> 更新：2026-11-12
> 状态：进行中
> 阶段：P3–P7 联调、P8 待规划、P9 部署验收

# cpp工 · 规划

## 当前目标

收敛 P7 波形错误码映射与 D18；按 JS 评审收敛 analysis、target、nextpnr、pack、simulation 契约；保留 P8 后续规划项，方向依据 [D-20](../README.md#决策记录)，细化方案暂缓。当前实现状态见 [history-cpp.md](history-cpp.md)，
跨角色待办见 [handoff-cpp.md](handoff-cpp.md)；本轮不修改 JS 方的计划或代码。

## 阶段与验收

| 阶段 | 目标 | 验收条件 | 状态 |
|---|---|---|---|
| P0–P2 | 平台层、任务契约和单任务执行 | 历史本地回归通过；当前版本的跨端验收另计 | 本地已实现 |
| P3-A | 拉取后的代码与工具基线 | Linux 环境完成构建、C++ 测试，记录工具版本与跳过项 | WSL Ubuntu 22.04 / GCC 11.4 构建通过、CTest 17/17；Node 未安装，JS 28/28 为 JS 方报告 |
| P3-B | synth 跨端联调 | 从 Node 提交，经 C++ 产出报告，验证事件、状态、下载及失败路径 | 既有假工具链路部分通过；早期取消可能无报告已获用户接受，拉取后跨端复验待做 |
| P3-C | Linux 原生 Yosys 验收 | 最小 RTL 经原生工具成功综合，网表与报告一致 | 主 contributor 已完成（用户确认） |
| P4 | pnr、pack、sim、flash | synth→pnr→pack、仿真及各自失败路径；flash 另验确认与租约 | nextpnr、pack、sim 假工具通过；参数意见已纳入草案，真实工具与最终确认待完成；flash 待实现 |
| P5 | target、validate | 共享板卡数据与约束校验正反例通过 | 本地实现；QN88P 引脚和板载资源已按官方资料修正，JS 确认、板卡修订版及 Bank 电压语义待收敛 |
| P6 | structure、lint | 双方评审分析契约，离线构建并验证定位与 Node 接入 | CLI 已实现，Node 已传 `--project-root`；analysis-v1 评审意见待收敛 |
| P7 | 波形读取与会话服务 | 固定查询语义，Node 每用户服务联调与波形 API 验收 | 用户确认 `initial_value` 严格早于 begin，CTest 17/17；HTTP 错误映射及 D18 其余细节冻结待完成 |
| P8 | 虚拟板卡与调试层 | 依据 D-20 形成独立规划、实现边界及验收条件 | 保留待规划；当前不进入实现 |
| P9 | 部署与完整验收 | 独立安装运行、真实工具链及跨端回归 | WSL C++ 构建和测试通过；Node、Yosys、nextpnr、gowin_pack、Verilator 均未发现，真实工具链与跨端验收待完成 |

P3-C 按用户反馈由主 contributor 完成，工具版本和原始报告未在当前仓库找到；
P3-B 的本地跨端测试使用假 Yosys，与该原生验收分别记录。

## 下一步

1. 完成 H-CPP-010：JS 方映射新增波形错误码并复验 HTTP 行为，主 contributor 冻结 D18 后补记端到端验收。
2. 请 JS 确认已更新的 analysis-v1、target、nextpnr、pack 和 simulation 契约；补记主 contributor 的 Yosys 验收记录来源。
3. 获得 Linux nextpnr 与芯片数据库后验收 synth→pnr，并评审指标；获取 gowin_pack 后验收 synth→pnr→pack。
4. 请 JS 确认已更新的 target/validate 与 analysis-v1 契约；由集成方复核板卡修订版、Bank 电压和默认 IO_TYPE 表达，并完成联调。
5. 对齐仿真 testbench 支持范围并验收真实 Verilator；完成 P9 Linux 独立部署及当前范围内的真实工具链和跨端回归。

## P4 首批依赖盘点

- nextpnr provider 与日志解析已迁入；指标结构见 [草案](../backend/cpp/contracts/run-v1/nextpnr-metrics.schema.json)。
- 旧 provider 的输入、默认值、输出及工具发现需逐项对照旧实现；实现必须约束输入与产物路径，移除旧插件注册依赖。
- 旧实现优先查找 `nextpnr-himbaechel`，兼容 `nextpnr-gowin` 名称；两者参数兼容性需以实际版本验证。
- nextpnr provider 的假工具、日志解析、错误、超时、取消和模拟 synth→pnr 链路已验证；WSL Ubuntu 22.04 未发现 nextpnr/Yosys/gowin_pack/Verilator，Windows gowin_pack 不满足 Linux 原生验收。
- Node 侧如何将上游已验证网表映射到下一任务输入，以及报告指标展示，需与 JS 方联调确认。
- Gowin 打包已接入路径约束、取消、超时及产物登记；假工具的 synth→pnr→pack 链路通过；输出相对 artifacts 的约定已落实，[参数草案](../backend/cpp/contracts/run-v1/gowin-pack-params.schema.json)待 JS 确认。
- 仿真 build/run 已接入，旧 testbench 解析支持范围已写入[仿真草案](../backend/cpp/contracts/run-v1/simulation-draft.md)，待 JS 确认并在真实 Verilator 环境验收。

## P8 后续规划

- P8 的范围依据 [D-20](../README.md#决策记录)；详细接口、模块边界和验收方案留待单独规划。
- `src/debug/` 现有迁入代码作为当前实况保留；不在本轮补接 capture CLI、Node 租约或硬件联调。

## 阻塞与依赖

- JS 方已报告 Node 22.23.4 测试 28/28，且早期取消无报告语义已获用户接受；主 contributor 已完成原生 Yosys 验收。
- P6 analysis-v1 最终确认与 H-CPP-024 精确依赖 revision、P7 错误码 HTTP 映射与 D18 冻结仍待完成；runtime 源核验为 `v0.26.1` 基线 43/44 文件匹配，grammar 与 `wasm-stdlib.h` 来源仍不确定。flash 确认和设备租约在 P4 前对齐。P8 详细规划待启动。
- 跨角色结论以 [README 决策记录](../README.md#决策记录) 为准；实现时遵循 `web/.clang-format`。
