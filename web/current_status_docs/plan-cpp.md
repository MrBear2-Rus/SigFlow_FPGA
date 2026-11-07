> 负责人：cpp工
> 类型：规划
> 更新：2026-11-07
> 状态：进行中
> 阶段：P3–P8 联调与 P9 部署验收

# cpp工 · 规划

## 当前目标

收敛 P7 `wave serve` 提案与 Node 波形接入；并行补齐 P3/P4/P5/P6 的跨端和真实工具验收，同时分层迁入 P8 TraceBridge 协议、传输和采集命令。当前实现状态见 [history-cpp.md](history-cpp.md)，
跨角色待办见 [handoff-cpp.md](handoff-cpp.md)；本轮不修改 JS 方的计划或代码。

## 阶段与验收

| 阶段 | 目标 | 验收条件 | 状态 |
|---|---|---|---|
| P0–P2 | 平台层、任务契约和单任务执行 | 历史本地回归通过；当前版本的跨端验收另计 | 本地已实现 |
| P3-A | 拉取后的代码与工具基线 | Linux 环境完成构建、C++ 测试，记录工具版本与跳过项 | 上轮 C++ 16/16 通过；Node 测试目标入口已修复，Node 22 复验待做 |
| P3-B | synth 跨端联调 | 从 Node 提交，经 C++ 产出报告，验证事件、状态、下载及失败路径 | 部分通过；早期取消待对齐 |
| P3-C | Linux 原生 Yosys 验收 | 最小 RTL 经原生工具成功综合，网表与报告一致 | 主 contributor 已完成（用户确认） |
| P4 | pnr、pack、sim、flash | synth→pnr→pack、仿真及各自失败路径；flash 另验确认与租约 | nextpnr、pack、sim 假工具通过；flash 待实现 |
| P5 | target、validate | 共享板卡数据与约束校验正反例通过 | 本地实现；接口评审和数据核验待完成 |
| P6 | structure、lint | 双方评审分析契约，离线构建并验证定位与 Node 接入 | 本地 CLI 实现；Node 工程根接入和契约评审待完成 |
| P7 | 波形读取与会话服务 | 固定查询语义，Node 每用户服务联调与波形 API 验收 | C++ 单次查询及 JSONL serve 已实现；serve 草案待评审 |
| P8 | TraceBridge 与设备命令 | 对齐采集契约、设备能力和硬件验收 | 协议、Linux 串口/枚举、可选 FTDI、选择器、devices list 提案、离线采集数据层、run 调度核心及单次 acquisition adapter 已实现并纳入生产构建；capture CLI、D11、Node 租约和硬件验收待完成 |
| P9 | 部署与完整验收 | 独立安装运行、真实工具链及跨端回归 | Linux 部署与 nextpnr、pack、Verilator 验收待完成 |

P3-C 按用户反馈由主 contributor 完成，工具版本和原始报告未在当前仓库找到；
P3-B 的本地跨端测试使用假 Yosys，与该原生验收分别记录。

## 下一步

1. 完成 H-CPP-010 的波形 serve 草案评审，再按双方结论接入 Node 并验收项目切换、波形查询及下载。
2. 按 H-CPP-007/H-CPP-013 收敛早期取消测试与 Node 测试入口；补记主 contributor 的 Yosys 验收记录来源。
3. 获得 Linux nextpnr 与芯片数据库后验收 synth→pnr，并评审指标；获取 gowin_pack 后验收 synth→pnr→pack。
4. 评审 target/validate 与 analysis-v1 契约，核验板卡引脚和语法依赖来源；联调 lint 工程根与 Node。
5. 对齐仿真 testbench 支持范围并验收真实 Verilator；完成 capture CLI/service 的 D11 评审与 Node 租约接入，再把已实现的 acquisition adapter 接入 CLI/JSONL，迁入 RTL 并做硬件验收。

## P4 首批依赖盘点

- nextpnr provider 与日志解析已迁入；指标结构见 [草案](../backend/cpp/contracts/run-v1/nextpnr-metrics.schema.json)。
- 旧 provider 的输入、默认值、输出及工具发现需逐项对照旧实现；实现必须约束输入与产物路径，移除旧插件注册依赖。
- 旧实现优先查找 `nextpnr-himbaechel`，兼容 `nextpnr-gowin` 名称；两者参数兼容性需以实际版本验证。
- nextpnr provider 的假工具、日志解析、错误、超时、取消和模拟 synth→pnr 链路已验证；本机未发现 Linux nextpnr，真实链路仍待验收。
- Node 侧如何将上游已验证网表映射到下一任务输入，以及报告指标展示，需与 JS 方联调确认。
- Gowin 打包已接入路径约束、取消、超时及产物登记；假工具的 synth→pnr→pack 链路通过，[参数草案](../backend/cpp/contracts/run-v1/gowin-pack-params.schema.json)待评审。
- 仿真 build/run 已接入，旧 testbench 解析为简化实现；[仿真草案](../backend/cpp/contracts/run-v1/simulation-draft.md)及真实 Verilator 环境待评审验收。

## P8 首批迁移边界

- 协议客户端、传输抽象、I/O 指标和 VCD/raw 解码数据层已迁入 `src/debug/`；接口与离线验收见 [TraceBridge C++ 草案](../backend/cpp/contracts/tracebridge-cpp-draft.md)。
- 编译时启用 FTDI 能力不能代表已检测到设备；运行期探测和打开错误必须保留不同诊断。
- [TraceBridge 草案](../backend/cpp/contracts/tracebridge-cpp-draft.md)中的 devices list 输出及 capture JSONL 输入输出需 JS 和主 contributor 按 D11 评审；capture 必须经过 Node 的设备租约后再连接设备。
- `backend/static/rtl/debug/` 当前仅有占位文件；迁入 RTL 后核对其协议版本与采样语义，再做硬件验收。

## 阻塞与依赖

- 早期取消及 Node 22 测试目标复验见 [H-CPP-007、H-CPP-013](handoff-cpp.md)；主 contributor 已完成原生 Yosys 验收。
- P6 工程根接入、契约评审与 P7 serve 草案评审尚需对齐；flash 确认和设备租约在 P4 前对齐。
- 跨角色结论以 [README 决策记录](../README.md#决策记录) 为准；实现时遵循 `web/.clang-format`。
