# SigFlow_VirtroFPGA

桌面GUI虚拟FPGA平台的M0开发工程，基于SigFlow_FPGA_CMake固定提交`cba9ee1dde50ee0ae90f08ca29fb9e759595d3f7`。

| 当前阶段 | 状态 | 入口 |
|---|---|---|
| K0 基线构建与尖峰 | 已实测：桌面编译、6项现有回归、真实批量仿真、持久Verilated模型 | [K0报告](docs/virtual/K0实测报告.md) |
| K1 CST/电气 | 生产实现与 C++/真实工具验收通过，G1/A2/A3 已关闭 | [K1实测报告](docs/virtual/K1生产改造与实测报告.md) |
| K2 持久会话 | 通用生成器、durable journal/恢复、pulse 与有界调度通过；G2 已关闭 | [K2/G2 汇总](docs/virtual/evidence/K2-closeout-summary.json) |
| K3 LiveTrace | 不可变增量快照、revision 缓存、会话提交与并发/TSan 验收通过；G3/本阶段 A8 已关闭 | [K2收口与K3实测报告](docs/virtual/K2收口与K3实测报告.md) |
| K4 / K5 / U0 | 板级拓扑、连接开路/恢复、含故障重放与 GUI 基础面板通过；33/33 CTest，内核 K0～K5 收官 | [K4/K5/U0报告](docs/virtual/K4_K5收官与U0实测报告.md) |
| U1 / 产品 M0 | 已实测收官：34/34 CTest、437 项 GUI 断言；U1/full_product_M0=true | [U1 联合验收与 M0 收官报告](docs/virtual/U1联合验收与M0收官报告.md) |

最高设计契约位于工作区`交付资料/04_SigFlow虚拟FPGA平台规划书/`。Web暂缓；新增内核后续保持零wx依赖。只读基线`.research/SigFlow_FPGA_verified`不在本工程内修改。

当前完整验收：`python tools/virtual/verify_m0_final.py`（桌面构建 + 全部 CTest + 真实 wxApp A1～A10 + TSan + 原始 Journal/渲染证据）。开启 `SIGFLOW_ENABLE_WSL_TSAN` 时共 34 项；新增正式目标 `u1_m0_full_product_acceptance`，原有 33 项保留。GUI 声明导出仍使用 `tools/virtual/export_ports.py` 与独立 sidecar，不覆盖 nextpnr 网表。

最新机器可读状态：[M0-final-acceptance-summary.json](docs/virtual/evidence/M0-final-acceptance-summary.json)。既有 K0～K5/U0 报告与证据保留作历史记录，U1 的产品状态独立记录；不会将历史内核通过误写为 GUI 联合验收通过。

桌面使用：运行 `build-m0/sigflow.exe`，在 FPGA 工具窗打开「虚拟实验箱」，点击「加载 M0 参考计数器」；后台自动完成声明导出、CST/Bank 校验与 runner 编译，进入 Ready。先松开 P4 复位，再按住 P3 并 Step，可观察 LED 计数。Paused 的按键与故障修改均显示 pending，Step/Play 后生效；查看底部「实验故障」可断开/恢复走线或注入输入 CON。

自己的工程：在工程根目录放置 [virtual.project.json 示例](examples/virtual/counter/virtual.project.json)，显式配置源文件、顶层、CST、板卡、clock.port_ref 和输入角色，再用「构建并启动当前工程」或「加载实验配置」。无需手动选择 runner.exe。双击结构化诊断可定位 Pin Binding 的真实 HDL 位。暂停/停止后可在界面重放当前或指定 Journal，默认从零核验 3 次并显示双 digest。

本机工程装配依赖 Python、固定 Verilator/Yosys、MinGW、CMake 构建出的 CLI 与静态库。DPI 验收覆盖 Light/Dark 的原生 DPI-change 布局检查和 96/144/192 DPI 向量快照；物理跨显示器切换未实测，详见收官报告。Web 继续暂缓。
