# SigFlow_VirtroFPGA

桌面GUI虚拟FPGA平台的M0开发工程，基于SigFlow_FPGA_CMake固定提交`cba9ee1dde50ee0ae90f08ca29fb9e759595d3f7`。

| 当前阶段 | 状态 | 入口 |
|---|---|---|
| K0 基线构建与尖峰 | 已实测：桌面编译、6项现有回归、真实批量仿真、持久Verilated模型 | [K0报告](docs/virtual/K0实测报告.md) |
| K1 CST/电气 | 生产实现与 C++/真实工具验收通过，G1/A2/A3 已关闭 | [K1实测报告](docs/virtual/K1生产改造与实测报告.md) |
| K2 持久会话 | 首轮 Core、guard 与真实 RTL 重放通过；完整 K2 仍进行中 | [K2接口与首轮实现](docs/virtual/K2接口与首轮实现.md) |
| K3～K5 / U0～U1 | 尚未实施 | 继续按工作区v1.1修订规格串行推进 |

最高设计契约位于工作区`交付资料/04_SigFlow虚拟FPGA平台规划书/`。Web暂缓；新增内核后续保持零wx依赖。只读基线`.research/SigFlow_FPGA_verified`不在本工程内修改。

K1 生产验证：`python tools/virtual/verify_k1.py`（完整构建 + 全部 CTest + 归档）；K2 真实 RTL：`python tools/virtual/verify_k2.py`。GUI 声明导出入口为 `tools/virtual/export_ports.py`，使用独立的 `.sigflow/virtual/declarations/<top>.json`，不覆盖 nextpnr 网表。依赖准备见 K0 报告。

K0 spike 与 K1 基线分析证据保留作历史记录，不能用其旧“G1未关闭”状态覆盖生产验收；最新状态见 `docs/virtual/evidence/K1-implementation-summary.json`。LiveTrace、完整虚拟板 GUI 和 crash-safe journal 尚未完成，不作为完整 M0 成品。
