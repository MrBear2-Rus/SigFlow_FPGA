> 负责人：cpp工
> 类型：交接
> 更新：2026-11-07
> 状态：进行中
> 阶段：P3–P8 联调与 P9 部署验收

# cpp工 · 交接

协议以 [run-v1](../backend/cpp/contracts/run-v1/README.md) 和
[analysis-v1 草案](../backend/cpp/contracts/analysis-v1/README.md) 为准；
模块实况见 [history-cpp.md](history-cpp.md)。

## 待对方配合

| ID | 可执行事项 |
|---|---|
| H-CPP-007 | JS 方按用户确认的早期取消处理方式调整测试，并与 C++ 方重验取消及恢复行为。 |
| H-CPP-009 | JS 方评审 [analysis-v1 C++ 补充草案](../backend/cpp/contracts/analysis-v1/cpp-implementation-draft.md)中的节点、定位、边界及诊断需求。 |
| H-CPP-010 | JS 方与主 contributor 评审 [波形查询和 serve 草案](../backend/cpp/contracts/wave-cpp-draft.md)，确定信号属性、时间精度、分页和会话需求。 |
| H-CPP-011 | JS 方在 P4 flash 联调前与 C++ 方确认用户确认及设备租约如何贯穿任务调用。 |
| H-CPP-012 | JS 方评审 [P5 接口草案](../backend/cpp/contracts/target-validation-draft.md)和共享引脚字段，联调绑定表与网表输入。 |
| H-CPP-013 | JS 方在 Linux Node 22 下运行远端已修复的 `web_js_test` 目标并报告结果。 |
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
| H-CPP-026 | JS 方与主 contributor 按 D11 评审 [devices list/capture 草案](../backend/cpp/contracts/tracebridge-cpp-draft.md)的 JSONL 字段、展示、取消和错误；capture 接入前明确 Node 设备租约调用。 |
| H-CPP-027 | 集成方提供装载匹配 TraceBridge RTL 的开发板及 Linux 可访问的串口/FTDI 通路，完成硬件联机验收。 |

## 待定接口

- analysis-v1、nextpnr 指标、仿真、目标板校验、波形 serve 及 capture/devices 接口均为草案，分别在跨端接入前完成双方评审。
- flash 确认的具体传递形式、设备租约和性能阈值，在对应阶段联调前对齐；相关决策见 [README](../README.md#决策记录)。

## 已知风险

- 当前 Node analysis runner 的 structure 可读取真实 C++ 结果，lint 未传工程根会失败；需 H-CPP-014 接入。Node 尚未启动 wave serve worker。
- 当前本地 Node/C++ 假工具集成 10/11 通过；用户已接受早期取消可能无报告，JS 测试尚需按确认结果更新。
- 工具被强制终止时可能没有终态报告，Node 恢复行为需要联调验证。
- `jsonschema` 缺失时 C++ 的部分契约校验会跳过；验收时记录依赖和实际执行结果。
- 主 contributor 已完成 Linux 原生 Yosys 验收（用户反馈）；当前仓库未找到版本与原始报告，跨端真实工具测试仍单独验收。
- 旧工程的 Tree-sitter 源码缺少版本元数据和根许可证；依赖说明记录了当前上游许可文本及待核验事项。
- nextpnr、gowin_pack、Verilator 尚未在本机完成 Linux 原生工具验收；假工具结果不证明工具变体参数兼容或位流可用。

## 最近变更

- 2026-11-07：实现 wave serve 的 open/signals/query/close JSONL 提案，增加行长/输出界限、会话恢复和时间范围修正；C++ 完整回归 14/14，D10 待评审。

- 2026-11-07：迁入 Linux 串口、枚举、可选 FTDI 和传输选择器，接入只读 devices list；补充 PTY 双向 I/O/断连验收，C++ 回归 16/16，D11 和硬件待接入。

- 2026-11-07：迁入 capture.raw v1 严格读写、线性采样 VCD 解码、原子元数据写入和 callback 驱动的 run 调度核心；调度器按旧 C++ 字段持久化 JSON/JSONL 并执行有界重试、超时、暂停/取消，源码已纳入生产构建；尚无运行时验收。单次 acquisition、CLI/JSONL 接线继续等待 D11 与设备租约联调。

- 2026-11-07：完成 full/minimal 单次 acquisition adapter：校验深度、时钟、触发和 probe，GET_INFO/指纹复核、有限 CONFIG、单次 ARM、deadline/cancel 轮询与分段读回，并原子提交 raw/VCD/metadata；只有 ARM 前或 STATUS 确认 done+idle 后允许安全重试，ARM 单次请求按单 attempt 分配 deadline。Linux 生产构建及 debug protocol/transport、capture decoder、VCD backend、wave integration 定向测试通过；run 调度器/acquisition adapter 运行时与硬件验收未执行。D09 工具后代清理复用现有 `PlatformProcessHost` guardian；CLI/JSONL 接线仍等待 D11 与 Node 租约评审。

- 2026-11-07：限制 VCD token、指令参数和层级，流式跳过注释并优化区间查询；C++ 回归 14/14。

- 2026-11-07：接入单次波形查询和分页，时间戳以字符串传递；C++ 回归 14/14，serve 协议待评审。

- 2026-11-07：迁入 VCD 数据层，验证时间戳、别名、向量及失败清理；C++ 回归 13/13，波形接口仍待评审。

- 2026-11-02：实现 structure/lint 本地 CLI 和工程相对路径校验；C++ 回归 12/12，Node structure 调用通过，lint 待传工程根。

- 2026-11-02：迁入本地 Tree-sitter 与 Verilog 语法，添加解析核心及独立测试，C++ 回归 11/11；P6 CLI 仍待路径约定。

- 2026-11-02：迁入旧版目标板数据及 CST/引脚校验；正反例和 C++ 10/10 回归通过，接口待评审。
