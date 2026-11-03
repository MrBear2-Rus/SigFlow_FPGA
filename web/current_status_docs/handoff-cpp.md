> 负责人：cpp工
> 类型：交接
> 更新：2026-11-03
> 状态：进行中
> 阶段：P3 联调、P4 工具、P5 校验与 P6 解析

# cpp工 · 交接

协议以 [run-v1](../backend/cpp/contracts/run-v1/README.md) 和
[analysis-v1 草案](../backend/cpp/contracts/analysis-v1/README.md) 为准；
模块实况见 [history-cpp.md](history-cpp.md)。

## 待对方配合

| ID | 可执行事项 |
|---|---|
| H-CPP-007 | JS 方按用户确认的早期取消处理方式调整测试，并与 C++ 方重验取消及恢复行为。 |
| H-CPP-009 | JS 方评审 [analysis-v1 C++ 补充草案](../backend/cpp/contracts/analysis-v1/cpp-implementation-draft.md)中的节点、定位、边界及诊断需求。 |
| H-CPP-010 | JS 方在 P7 实现前提供波形视图需要的信号属性、时间精度和分页需求。 |
| H-CPP-011 | JS 方在 P4 flash 联调前与 C++ 方确认用户确认及设备租约如何贯穿任务调用。 |
| H-CPP-012 | JS 方评审 [P5 接口草案](../backend/cpp/contracts/target-validation-draft.md)和共享引脚字段，联调绑定表与网表输入。 |
| H-CPP-013 | JS 方修正 `web_js_test` 的测试目录入口并在 Linux Node 22 下复验。 |
| H-CPP-014 | JS 方在 Node 的 lint 调用中传入 `--project-root` 并联调相对路径；现调用未传时返回错误。 |
| H-CPP-015 | JS 方在 P4 联调前与 C++ 方确认上游网表的路径、归属校验及结果展示方式。 |
| H-CPP-016 | 主 contributor 在审阅后更新 README 中 nextpnr、pack、sim、target、validate、structure 和 lint 的实现状态。 |
| H-CPP-017 | 集成方为 P4 真实验收提供 Linux nextpnr 和匹配器件数据库的环境。 |
| H-CPP-018 | JS 方评审 [nextpnr 指标草案](../backend/cpp/contracts/run-v1/nextpnr-metrics.schema.json)，确认展示和兼容需求。 |
| H-CPP-019 | JS 方评审 [Gowin 打包参数草案](../backend/cpp/contracts/run-v1/gowin-pack-params.schema.json)，联调上游产物传递与位流下载。 |
| H-CPP-020 | 集成方提供 Linux gowin_pack 及匹配芯片数据库，验收真实 synth→pnr→pack。 |
| H-CPP-021 | JS 方评审 [仿真接口草案](../backend/cpp/contracts/run-v1/simulation-draft.md)，负责选取并校验可信构建产物，再联调仿真与波形下载。 |
| H-CPP-022 | 集成方提供 Linux Verilator、make 和 C++ 编译环境，验收真实仿真及 testbench 支持范围。 |
| H-CPP-023 | 集成方对照芯片和板卡资料核验共享引脚表的器件、封装、可用性与保留引脚。 |
| H-CPP-024 | 集成方核对迁入 Tree-sitter 与 Verilog 语法的原始版本、来源和许可信息。 |

## 待定接口

- analysis-v1、nextpnr 指标、仿真及目标板校验接口均为草案，分别在实现和跨端接入前完成双方评审。
- 波形接口、flash 确认的具体传递形式和性能阈值，在对应阶段联调前对齐；相关决策见 [README](../README.md#决策记录)。

## 已知风险

- 当前 Node analysis runner 的 structure 可读取真实 C++ 结果，lint 未传工程根会失败；需 H-CPP-014 接入。
- 当前本地 Node/C++ 假工具集成 10/11 通过；用户已接受早期取消可能无报告，JS 测试尚需按确认结果更新。
- 工具被强制终止时可能没有终态报告，Node 恢复行为需要联调验证。
- `jsonschema` 缺失时 C++ 的部分契约校验会跳过；验收时记录依赖和实际执行结果。
- 主 contributor 已完成 Linux 原生 Yosys 验收（用户反馈）；当前仓库未找到版本与原始报告，跨端真实工具测试仍单独验收。
- 旧工程的 Tree-sitter 源码缺少版本元数据和根许可证；依赖说明记录了当前上游许可文本及待核验事项。
- nextpnr、gowin_pack、Verilator 尚未在本机完成 Linux 原生工具验收；假工具结果不证明工具变体参数兼容或位流可用。

## 最近变更

- 2026-11-02：实现 structure/lint 本地 CLI 和工程相对路径校验；C++ 回归 12/12，Node structure 调用通过，lint 待传工程根。

- 2026-11-02：迁入本地 Tree-sitter 与 Verilog 语法，添加解析核心及独立测试，C++ 回归 11/11；P6 CLI 仍待路径约定。

- 2026-11-02：迁入旧版目标板数据及 CST/引脚校验；正反例和 C++ 10/10 回归通过，接口待评审。

- 2026-11-02：迁入仿真构建与运行，按用户确认的信任边界复核构建产物，C++ 回归 9/9；摘要边界补强后仿真测试再次通过。

- 2026-11-02：接入 Gowin pack，假工具覆盖错误、取消及 synth→pnr→pack；C++ 完整回归 8/8。

- 2026-11-02：迁入 nextpnr 日志分析，补充指标草案与边界测试；C++ 完整回归 7/7。

- 2026-11-02：接入 nextpnr provider；假工具 19 场景和模拟 synth→pnr 链路通过，完整 C++ 回归 6/6；修复部分 stdin 输入时取消挂起。

- 2026-11-02：新版本 C++ 5/5 测试通过；Node 假工具跨端 10/11，早期取消失败已复现；完成 nextpnr 旧代码盘点。
- 2026-11-02：按用户反馈记录主 contributor 已完成 Linux 原生 Yosys 验收。
- 2026-11-02：按新文档规范补齐 C++ 模块实况，调整计划以优先完成 synth 联调。
- 2026-11-01：实现 synth provider；假工具和 Windows Yosys 测试适配器验证通过。
- 2026-11-01：实现单 Job Runtime，并完成当时的本地回归。
