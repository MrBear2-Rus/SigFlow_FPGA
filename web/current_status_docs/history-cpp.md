> 负责人：cpp工
> 类型：实况
> 更新：2026-11-07
> 状态：进行中
> 阶段：P3–P8 联调与 P9 部署验收

# cpp工 · 模块实况

本文件描述现有代码；计划见 [plan-cpp.md](plan-cpp.md)，对接待办见
[handoff-cpp.md](handoff-cpp.md)，架构与协议分别见 [README](../README.md) 和 [contracts](../backend/cpp/contracts/)。

## 入口与执行

| 模块 | 入口与当前行为 | 明确未完成项 |
|---|---|---|
| CLI | `backend/cpp/src/cli/main.cpp` 分发命令；`run` 接入单任务执行，`devices list` 已接入本机端口/构建能力查询 | synth、pnr、pack 和 sim 已接入；target/validate、structure/lint 已接入；capture 与其他 Job provider 仍为占位 |
| 平台层 | `src/eda-platform/` 提供 Linux 进程执行及独立 guardian 回收工具进程组、工具发现依赖、路径、摘要和动态库能力 | Linux 生产环境的独立部署验收待完成 |
| 核心层 | `src/eda-core/` 提供 Job 参数处理、目录检查、报告序列化、事件输出、取消与产物登记 | Node 新版本的实际端到端行为尚待核对 |
| 综合 | `src/plugins/synth-yosys/` 的 provider 和脚本生成器已接入生产工厂 | Node 假工具联调的早期取消待对齐；真实工具跨端验证待验收 |
| 布局布线 | `src/plugins/pnr-nextpnr/` 的 provider 与日志分析已接入生产工厂；Runtime 负责报告及摘要 | 真实 nextpnr/芯片数据库、指标草案评审和 Node 联调待完成 |
| 打包 | `src/plugins/pack-gowin/` 已接入生产工厂，校验位流并写入旧格式打包清单 | 真实 gowin_pack、参数草案评审及 Node 联调待完成 |
| 仿真 | `src/plugins/sim-verilator/` 的构建与运行 provider 已接入；运行前复核构建产物 | 原生 Verilator、简化 testbench 支持范围和 Node 联调待验收 |
| 目标板与约束 | `src/plugins/target-tangnano9k/`、`src/plugins/cst-gowin-cst/` 和共享静态数据支持查询、CST 与引脚校验 | 接口草案、旧引脚表准确性与 Node 联调待核验；旧规则不保证全部端口覆盖 |
| 其他工具 | CMake 保留其他插件目标 | flash 尚未迁入 |
| 分析与波形 | `src/analysis/` 的 structure/lint 已接入单文件解析；`src/plugins/wave-vcd/` 提供 VCD 数据层、单次查询和 JSONL `wave serve` | Node lint 需传工程根；serve 提案待 JS/主 contributor 评审 |
| TraceBridge | `src/debug/` 提供 Linux 串口枚举与传输、可选 FTDI、传输选择器、I/O 指标、framed/minimal 协议客户端、只读 `devices list`、带范围校验和原子提交的采集数据层、callback 驱动的 run 调度核心及单次 acquisition adapter | capture CLI/JSONL、Node 租约/D11 评审、真实 RTL 和硬件验收尚未完成 |

## 数据与依赖

- Job 执行事实与产物写入用户工程下的任务目录；协议和样例以 [run-v1](../backend/cpp/contracts/run-v1/) 为准。
- `backend/cpp/third_party/nlohmann/` 与 Tree-sitter runtime/Verilog 语法已随工程提供；来源与许可状态见 [依赖说明](../backend/cpp/third_party/README.md)。
- Node 当前已有 Job 调用和分析调用代码；analysis-v1 和 [C++ 实现补充](../backend/cpp/contracts/analysis-v1/cpp-implementation-draft.md)仍待评审。

## 验证现状

- 最近一次完整 C++ 回归 16/16 通过；analysis CLI 的结构、语法、UTF-16 列号和路径/大小边界测试通过；目标板和约束校验正反例及 Verilog 解析核心的 Unicode、错误与空文件测试通过。nextpnr 假工具及模拟 synth→pnr 链路覆盖日志分析、工具错误、路径边界、超时和运行中取消。假工具的 synth→pnr→pack 链路、打包失败和取消测试通过；仿真构建与运行、产物身份与摘要不匹配、超时和取消测试通过；wave serve JSONL、超长行恢复及时间范围回归通过；TraceBridge fake transport 覆盖帧校验、重试、FTDI 字节过滤、PTY 双向/断连、波特率、选择器和 devices list 字段；Linux 生产构建通过。新增的 debug protocol、transport、capture decoder、VCD backend 与 wave integration 定向测试 5/5 通过；run 调度器与 acquisition adapter 的运行时测试和硬件验收未执行，capture CLI 尚未接线。
- VCD 数据层覆盖 64 位时间戳、别名、向量、超长 token、指令参数、时间范围和失败清理；单次 signals/query 与 JSONL serve 均支持分页，serve 字段见待评审的 [波形草案](../backend/cpp/contracts/wave-cpp-draft.md)。
- Node analysis runner 实测可读取 structure；lint 未传工程根，待 JS 方接入。Node 22.23.4 下直接运行测试文件，假工具跨端 10/11 通过；早期取消缺报告两次复现，用户已接受该时机允许无报告。
- 根据用户反馈，主 contributor 已完成 Linux 原生 Yosys 验收；当前仓库未找到对应版本和原始记录。Windows Yosys 0.49 的测试适配器验证也曾通过。
- 验证方式见 [README 的构建与运行](../README.md#8-构建与运行)；可选互操作测试见 `backend/cpp/tests/windows_yosys_interop.py`。
- 真实 Yosys/nextpnr/gowin_pack/Verilator 的 Node ↔ C++ 跨端链路与硬件尚未验证；JS 早期取消测试待按用户确认语义更新。
- TraceBridge fake transport 和 capture decoder 离线目标不代表 RTL、串口/FTDI 或硬件兼容，详见 [TraceBridge 草案](../backend/cpp/contracts/tracebridge-cpp-draft.md)。

- 远端已将 `web_js_test` 改为逐个传入测试文件；Node 22 下的目标运行结果待 H-CPP-013 复验。
