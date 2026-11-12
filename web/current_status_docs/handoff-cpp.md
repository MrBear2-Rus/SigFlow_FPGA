> 负责人：cpp工
> 类型：交接
> 更新：2026-11-12
> 状态：进行中
> 阶段：P3–P7 联调、P8 待规划、P9 部署验收

# cpp工 · 交接

协议以 [run-v1](../backend/cpp/contracts/run-v1/README.md) 和
[analysis-v1 草案](../backend/cpp/contracts/analysis-v1/README.md) 为准；
模块实况见 [history-cpp.md](history-cpp.md)。

## 待对方配合

| ID | 可执行事项 |
|---|---|
| H-CPP-009 | JS 方确认已更新的 [analysis-v1 C++ 补充草案](../backend/cpp/contracts/analysis-v1/cpp-implementation-draft.md)：相对路径、固定超限 code、叶节点可省略 `children`。 |
| H-CPP-010 | JS 方将 `FILE_NOT_FOUND`、`NOT_OPEN`、`RESPONSE_TOO_LARGE` 映射为合适的 HTTP 状态并复验波形 E2E；主 contributor 冻结 D18 其余细节（`initial_value` 严格边界已由用户确认）。 |
| H-CPP-011 | JS 方在 P4 flash 联调前与 C++ 方确认用户确认及设备租约如何贯穿任务调用。 |
| H-CPP-012 | JS 方确认已更新的 [P5 接口草案](../backend/cpp/contracts/target-validation-draft.md)：0 起始 `line`、`-1` 显示为无定位、`content` 语义及非法 CST 仍以 `valid=false` 成功返回。 |
| H-CPP-015 | JS 方在 P4 联调前与 C++ 方确认上游网表的路径、归属校验及结果展示方式。 |
| H-CPP-016 | 主 contributor 在审阅后更新 README 中 nextpnr、pack、sim、target、validate、structure 和 lint 的实现状态。 |
| H-CPP-017 | 集成方为 P4 真实验收提供 Linux nextpnr 和匹配器件数据库的环境。 |
| H-CPP-018 | JS 方确认已更新的 [nextpnr 指标草案](../backend/cpp/contracts/run-v1/nextpnr-metrics.schema.json)：动态资源标签、`percent` 为百分数、错误行 1 起始、挂在 `report.metrics[].nextpnr`。 |
| H-CPP-019 | JS 方确认已更新的 [Gowin 打包参数草案](../backend/cpp/contracts/run-v1/gowin-pack-params.schema.json)：`output` 相对 `artifacts/`，产物 schema 为 `eda.bitstream.apicula.v1`。 |
| H-CPP-020 | 集成方提供 Linux gowin_pack 及匹配芯片数据库，验收真实 synth→pnr→pack。 |
| H-CPP-021 | JS 方确认已更新的 [仿真接口草案](../backend/cpp/contracts/run-v1/simulation-draft.md)：`duration_ticks` 十进制字符串边界和简化 testbench 支持范围。 |
| H-CPP-022 | 集成方提供 Linux Verilator、make 和 C++ 编译环境，验收真实仿真及 testbench 支持范围。 |
| H-CPP-023 | 集成方复核已按 Gowin UG803 QN88P 与 Sipeed 3672/3674 资料修正的共享引脚表适用于目标板修订版，并对齐 Bank 电压和默认 IO_TYPE 的表达。 |
| H-CPP-024 | 集成方提供旧 `3rd` 工作区中 Tree-sitter runtime 与 Verilog grammar 的原始 checkout/tag 或归档元数据；本地 runtime 44 个代码文件有 43 个匹配 `v0.26.1`，但 `wasm-stdlib.h` 与 grammar parser 均不匹配，精确 revision 仍待核实。 |
| H-CPP-028 | 主 contributor 与 JS/C++ 方在启动 P8 时，依据 D-20 形成虚拟板卡、调试层及验收边界的实施规划。 |

## 待定接口

- analysis-v1、nextpnr 指标、仿真、目标板校验及波形 serve 接口均为草案，分别在跨端接入前完成双方评审。P8 详细方案依据 [D-20](../README.md#决策记录) 留待后续规划。
- flash 确认的具体传递形式、设备租约和性能阈值，在对应阶段联调前对齐；相关决策见 [README](../README.md#决策记录)。

## 已知风险

- 当前 Node wave API 未映射新增的 `NOT_OPEN` 等 C++ 错误码，未知码会落到 502；需 H-CPP-010 更新映射和关闭后查询的状态断言。
- JS 方报告 Node 22.23.4 测试 28/28，structure/lint 已传工程根；本地此前 Node/C++ 假工具集成 10/11，早期取消无报告已获用户接受，拉取后的跨端复验待做。
- 工具被强制终止时可能没有终态报告，Node 恢复行为需要联调验证。
- `jsonschema` 缺失时 C++ 的部分契约校验会跳过；验收时记录依赖和实际执行结果。
- 主 contributor 已完成 Linux 原生 Yosys 验收（用户反馈）；当前仓库未找到版本与原始报告，跨端真实工具测试仍单独验收。
- Tree-sitter runtime 与 Verilog grammar 的本地 ABI、SHA-256 和 MIT 许可已记录；旧工程缺少上游 revision 元数据，精确 tag/commit 仍待集成方提供。
- nextpnr、gowin_pack、Verilator 尚未在本机完成 Linux 原生工具验收；假工具结果不证明工具变体参数兼容或位流可用。

## 最近变更

- 2026-11-12：WSL Ubuntu 22.04 / GCC 11.4 构建成功；用户确认 `initial_value` 严格早于 begin，修正旧波形集成断言后 CTest 17/17 通过。Ubuntu 缺 Node/Yosys/nextpnr/gowin_pack/Verilator；Windows 仅有 gowin_pack。
- 2026-11-12：Tree-sitter runtime 的 43/44 个代码文件及 `ICU_SHA` 匹配官方 `v0.26.1`（忽略 CRLF/LF 差异）；唯一例外 `wasm-stdlib.h` 在已核对的 `v0.25.0`–`v0.27.2` tags 中均无匹配。grammar `v1.0.1`–`v1.0.3` 也未匹配；保留 H-CPP-024 等原始元数据。
- 2026-11-09：按 JS 反馈落实 wave、analysis、target、nextpnr、pack 与 simulation 契约细节，修正 nextpnr 超用百分比说明并记录 Tree-sitter ABI/文件指纹；按 D-20 保留 P8 虚拟板卡规划，细化方案待启动。Node 波形 HTTP 映射、契约确认、D18 冻结及依赖原始 revision 待完成。

- 2026-11-09：依据 Gowin UG803 QN88P 与 Sipeed 3672/3674 资料重建 Tang Nano 9K 引脚表，修正 LED、按键、UART、SPI Flash 与封装保留项；板卡修订版和 Bank 电压/默认 IO_TYPE 仍待集成方复核。

- 2026-11-07：实现 wave serve 的 open/signals/query/close JSONL 提案，增加行长/输出界限、会话恢复和时间范围修正；C++ 完整回归 14/14，D10 待评审。

- 2026-11-07：迁入 Linux 串口、枚举、可选 FTDI 和传输选择器，接入只读 devices list；补充 PTY 双向 I/O/断连验收，C++ 回归 16/16；当时待做的 D11 与硬件联机暂缓，后续按 D-20 规划虚拟板卡方向。

- 2026-11-07：迁入 capture.raw v1 严格读写、线性采样 VCD 解码、原子元数据写入和 callback 驱动的 run 调度核心；调度器按旧 C++ 字段持久化 JSON/JSONL 并执行有界重试、超时、暂停/取消，源码已纳入生产构建；尚无运行时验收。单次 acquisition 与 CLI/JSONL 当时待接线，后续接入依据 D-20 另行规划。

- 2026-11-07：完成 full/minimal 单次 acquisition adapter：校验深度、时钟、触发和 probe，GET_INFO/指纹复核、有限 CONFIG、单次 ARM、deadline/cancel 轮询与分段读回，并原子提交 raw/VCD/metadata；只有 ARM 前或 STATUS 确认 done+idle 后允许安全重试，ARM 单次请求按单 attempt 分配 deadline。Linux 生产构建及 debug protocol/transport、capture decoder、VCD backend、wave integration 定向测试通过；run 调度器/acquisition adapter 运行时与硬件验收未执行。D09 工具后代清理复用现有 `PlatformProcessHost` guardian；后续接线依据 D-20 待规划。

- 2026-11-07：限制 VCD token、指令参数和层级，流式跳过注释并优化区间查询；C++ 回归 14/14。

- 2026-11-07：接入单次波形查询和分页，时间戳以字符串传递；C++ 回归 14/14，serve 协议待评审。
