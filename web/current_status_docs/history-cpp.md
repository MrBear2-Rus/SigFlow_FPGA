> 负责人：cpp工
> 类型：实况
> 更新：2026-11-12
> 状态：进行中
> 阶段：P3–P7 联调、P8 待规划、P9 部署验收

# cpp工 · 模块实况

本文件描述现有代码；计划见 [plan-cpp.md](plan-cpp.md)，对接待办见
[handoff-cpp.md](handoff-cpp.md)，架构与协议分别见 [README](../README.md) 和 [contracts](../backend/cpp/contracts/)。

## 入口与执行

| 模块 | 入口与当前行为 | 明确未完成项 |
|---|---|---|
| CLI | `backend/cpp/src/cli/main.cpp` 分发命令；`run` 接入单任务执行，`devices list` 已接入本机端口/构建能力查询 | synth、pnr、pack 和 sim 已接入；target/validate、structure/lint 已接入；capture CLI 尚未接入，后续按 D-20 规划 |
| 平台层 | `src/eda-platform/` 提供 Linux 进程执行及独立 guardian 回收工具进程组、工具发现依赖、路径、摘要和动态库能力 | Linux 生产环境的独立部署验收待完成 |
| 核心层 | `src/eda-core/` 提供 Job 参数处理、目录检查、报告序列化、事件输出、取消与产物登记 | Node 新版本的实际端到端行为尚待核对 |
| 综合 | `src/plugins/synth-yosys/` 的 provider 和脚本生成器已接入生产工厂 | Node 假工具联调的早期取消待对齐；真实工具跨端验证待验收 |
| 布局布线 | `src/plugins/pnr-nextpnr/` 的 provider 与日志分析已接入生产工厂；Runtime 负责报告及摘要 | 真实 nextpnr/芯片数据库、指标草案评审和 Node 联调待完成 |
| 打包 | `src/plugins/pack-gowin/` 已接入生产工厂，校验位流并写入旧格式打包清单 | 真实 gowin_pack、参数草案评审及 Node 联调待完成 |
| 仿真 | `src/plugins/sim-verilator/` 的构建与运行 provider 已接入；运行前复核构建产物 | 原生 Verilator、简化 testbench 支持范围和 Node 联调待验收 |
| 目标板与约束 | `src/plugins/target-tangnano9k/`、`src/plugins/cst-gowin-cst/` 和共享静态数据支持查询、CST 与引脚校验；CST 诊断行按 0 起始输出、无定位时为 -1；Tang Nano 9K QN88P 表与板载资源已按官方芯片/板卡资料修正 | 接口草案待 JS 最终确认；板卡修订版、Bank 电压/default IO_TYPE 表达与 Node 联调待核验；旧规则不保证全部端口覆盖 |
| 其他工具 | CMake 保留其他插件目标 | flash 尚未迁入 |
| 分析与波形 | `src/analysis/` 的 structure/lint 已接入单文件解析；资源超限使用固定 `ANALYSIS_LIMIT_EXCEEDED`；`src/plugins/wave-vcd/` 提供 VCD 数据层、单次查询和 JSONL `wave serve`；query 的 `initial_value` 严格早于 begin，open 返回信号数与时间范围元数据 | Node 已接入每用户 worker/API 和波形面板；波形 HTTP 错误映射、E2E 复验与 D18 冻结待完成 |
| TraceBridge | `src/debug/` 现有代码提供设备传输、枚举、协议客户端、采集数据层和 run 调度核心；`devices list` 已接入 CLI | P8 方向按 [D-20](../README.md#决策记录) 保留，虚拟板卡方案和接线尚待规划 |

## 数据与依赖

- Job 执行事实与产物写入用户工程下的任务目录；协议和样例以 [run-v1](../backend/cpp/contracts/run-v1/) 为准。
- `backend/cpp/third_party/nlohmann/` 与 Tree-sitter runtime/Verilog 语法已随工程提供；依赖说明记录 SHA-256、许可证和已知 ABI，Tree-sitter 精确 upstream revision 仍未知，见 [依赖说明](../backend/cpp/third_party/README.md)。
- Node 当前已有 Job 调用和分析调用代码；analysis-v1 与 [C++ 实现补充](../backend/cpp/contracts/analysis-v1/cpp-implementation-draft.md)的 JS 评审意见已纳入，待最终确认。

## 验证现状

- WSL Ubuntu 22.04 / GCC 11.4 下 Linux CMake 构建通过，CTest 17/17 通过。波形集成覆盖 `initial_value` 严格早于 begin、同边界不重复计入初始值，以及 64 位时间戳分页；TraceBridge 仍未进行运行时/硬件验收，P8 详细方案待规划。
- VCD 数据层覆盖 64 位时间戳、别名、向量、超长 token、指令参数、时间范围和失败清理；单次 signals/query 与 JSONL serve 均支持分页，serve 字段和错误码见 [波形草案](../backend/cpp/contracts/wave-cpp-draft.md)。Node 的波形页面已对 begin 边界进行显示去重。
- JS 方报告 Node 22.23.4 测试 28/28，structure/lint 均已传工程根；本地此前 Node/C++ 假工具跨端 10/11，早期取消缺报告已获用户接受。当前 WSL 无 Node，拉取后的跨端回归仍待做。
- 根据用户反馈，主 contributor 已完成 Linux 原生 Yosys 验收；当前仓库未找到对应版本和原始记录。Windows Yosys 0.49 的测试适配器验证也曾通过。
- 验证方式见 [README 的构建与运行](../README.md#8-构建与运行)；可选互操作测试见 `backend/cpp/tests/windows_yosys_interop.py`。
- 真实 Yosys/nextpnr/gowin_pack/Verilator 的 Node ↔ C++ 跨端链路与硬件尚未验证。
- TraceBridge fake transport 和 capture decoder 离线结果不代表虚拟板卡行为；P8 目标及验收方案待规划，见 [D-20](../README.md#决策记录)。
