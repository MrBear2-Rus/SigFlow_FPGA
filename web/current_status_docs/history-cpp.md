> 负责人：cpp工
> 类型：实况
> 更新：2026-11-03
> 状态：进行中
> 阶段：P3 联调、P4 工具、P5 校验与 P6 解析

# cpp工 · 模块实况

本文件描述现有代码；计划见 [plan-cpp.md](plan-cpp.md)，对接待办见
[handoff-cpp.md](handoff-cpp.md)，架构与协议分别见 [README](../README.md) 和 [contracts](../backend/cpp/contracts/)。

## 入口与执行

| 模块 | 入口与当前行为 | 明确未完成项 |
|---|---|---|
| CLI | `backend/cpp/src/cli/main.cpp` 分发命令；`run` 接入单任务执行 | synth、pnr、pack 和 sim 已接入；target/validate、structure/lint 已接入；其他 Job provider 与命令仍为占位 |
| 平台层 | `src/eda-platform/` 提供 Linux 进程执行、工具发现依赖、路径、摘要及动态库能力 | Linux 生产环境的独立部署验收待完成 |
| 核心层 | `src/eda-core/` 提供 Job 参数处理、目录检查、报告序列化、事件输出、取消与产物登记 | Node 新版本的实际端到端行为尚待核对 |
| 综合 | `src/plugins/synth-yosys/` 的 provider 和脚本生成器已接入生产工厂 | Node 假工具联调的早期取消待对齐；真实工具跨端验证待验收 |
| 布局布线 | `src/plugins/pnr-nextpnr/` 的 provider 与日志分析已接入生产工厂；Runtime 负责报告及摘要 | 真实 nextpnr/芯片数据库、指标草案评审和 Node 联调待完成 |
| 打包 | `src/plugins/pack-gowin/` 已接入生产工厂，校验位流并写入旧格式打包清单 | 真实 gowin_pack、参数草案评审及 Node 联调待完成 |
| 仿真 | `src/plugins/sim-verilator/` 的构建与运行 provider 已接入；运行前复核构建产物 | 原生 Verilator、简化 testbench 支持范围和 Node 联调待验收 |
| 目标板与约束 | `src/plugins/target-tangnano9k/`、`src/plugins/cst-gowin-cst/` 和共享静态数据支持查询、CST 与引脚校验 | 接口草案、旧引脚表准确性与 Node 联调待核验；旧规则不保证全部端口覆盖 |
| 其他工具 | CMake 保留其他插件目标 | flash 尚未迁入 |
| 分析与波形 | `src/analysis/` 的 structure/lint 已接入单文件解析；波形目标仍为占位 | Node lint 需传工程根，契约待评审；wave、TraceBridge 和设备命令尚未实现 |

## 数据与依赖

- Job 执行事实与产物写入用户工程下的任务目录；协议和样例以 [run-v1](../backend/cpp/contracts/run-v1/) 为准。
- `backend/cpp/third_party/nlohmann/` 与 Tree-sitter runtime/Verilog 语法已随工程提供；来源与许可状态见 [依赖说明](../backend/cpp/third_party/README.md)。
- Node 当前已有 Job 调用和分析调用代码；analysis-v1 和 [C++ 实现补充](../backend/cpp/contracts/analysis-v1/cpp-implementation-draft.md)仍待评审。

## 验证现状

- C++ 完整回归 12/12 通过；analysis CLI 的结构、语法、UTF-16 列号和路径/大小边界测试通过；目标板和约束校验正反例及 Verilog 解析核心的 Unicode、错误与空文件测试通过。nextpnr 假工具及模拟 synth→pnr 链路覆盖日志分析、工具错误、路径边界、超时和运行中取消。假工具的 synth→pnr→pack 链路、打包失败和取消测试通过；仿真构建与运行、产物身份与摘要不匹配、超时和取消测试通过；关闭测试的 Linux 生产构建通过。
- Node analysis runner 实测可读取 structure；lint 未传工程根，待 JS 方接入。Node 22.23.4 下直接运行测试文件，假工具跨端 10/11 通过；早期取消缺报告两次复现，用户已接受该时机允许无报告。
- 根据用户反馈，主 contributor 已完成 Linux 原生 Yosys 验收；当前仓库未找到对应版本和原始记录。Windows Yosys 0.49 的测试适配器验证也曾通过。
- 验证方式见 [README 的构建与运行](../README.md#8-构建与运行)；可选互操作测试见 `backend/cpp/tests/windows_yosys_interop.py`。
- 真实 Yosys/nextpnr/gowin_pack/Verilator 的 Node ↔ C++ 跨端链路与硬件尚未验证；JS 早期取消测试待按用户确认语义更新。

- `web_js_test` 在 Node 22 下因测试目录参数报 MODULE_NOT_FOUND；直接指定测试文件可运行，详见交接项 H-CPP-013。
