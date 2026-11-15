# SigFlow_VirtroFPGA

桌面GUI虚拟FPGA平台的M0开发工程，基于SigFlow_FPGA_CMake固定提交`cba9ee1dde50ee0ae90f08ca29fb9e759595d3f7`。

| 当前阶段 | 状态 | 入口 |
|---|---|---|
| K0 基线构建与尖峰 | 已实测：桌面编译、6项现有回归、真实批量仿真、持久Verilated模型 | [K0报告](docs/virtual/K0实测报告.md) |
| K1 CST/电气 | 生产实现与 C++/真实工具验收通过，G1/A2/A3 已关闭 | [K1实测报告](docs/virtual/K1生产改造与实测报告.md) |
| K2 持久会话 | 通用生成器、durable journal/恢复、pulse 与有界调度通过；G2 已关闭 | [K2/G2 汇总](docs/virtual/evidence/K2-closeout-summary.json) |
| K3 LiveTrace | 不可变增量快照、revision 缓存、会话提交与并发/TSan 验收通过；G3/本阶段 A8 已关闭 | [K2收口与K3实测报告](docs/virtual/K2收口与K3实测报告.md) |
| K4～K5 / U0～U1 | 尚未实施 | 继续按工作区v1.1修订规格串行推进 |

最高设计契约位于工作区`交付资料/04_SigFlow虚拟FPGA平台规划书/`。Web暂缓；新增内核后续保持零wx依赖。只读基线`.research/SigFlow_FPGA_verified`不在本工程内修改。

当前完整验收：`python tools/virtual/verify_k23.py`（桌面构建 + 全部 CTest + 源码/runner 指纹归档）；K2 真实 RTL 单项：`python tools/virtual/verify_k2.py`。本次开启 `SIGFLOW_ENABLE_WSL_TSAN`，共 28 项；默认关闭该可选项时共 27 项。GUI 声明导出入口为 `tools/virtual/export_ports.py`，使用独立的 `.sigflow/virtual/declarations/<top>.json`，不覆盖 nextpnr 网表。依赖准备见 K0 与 K2/K3 报告。

K0 spike、K1 基线分析与 K2 首轮证据保留作历史记录；当前 K1 状态见 `docs/virtual/evidence/K1-implementation-summary.json`，K2/K3 状态见本轮两份汇总。完整虚拟板 GUI、输出支路开路故障等后续门槛尚未完成，不作为完整 M0 成品。
