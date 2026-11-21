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
| Phase 1 扩展 GUI MVP | 已实测收官：37/37 CTest、36 项新增 GUI 断言、四类故障与外设三次重放；Phase1_complete=true | [Phase 1 实测报告](docs/virtual/Phase1扩展外设与四大故障实测报告.md) |
| Phase 2 调试与教学闭环 | 已实测收官：41/41 CTest、2132 项原生检查、22 项新增 GUI 断言；签名四课程与三组隐藏复测；Phase2_complete=true | [Phase 2 实测报告](docs/virtual/Phase2调试探针与教学判题闭环实测报告.md) |
| Phase 3 产品化与跨端同源 | 已实测收官：44/44 CTest；SFVB 二进制帧 C++↔JS 逐字段一致、20 次浮动窗往返、鬼键 vs 二极管、reset_glitch/分频偏移；Phase3_complete=true | [Phase 3 实测报告](docs/virtual/Phase3产品化深化与跨端同源实测报告.md) |
| Phase 4 高级总线外设与协议时序故障 | 已实测收官：47/47 CTest；I2C 开漏传感器、SPI OLED 点阵、i2c_nak/spi_mode_fault/crc_tamper/cdc_sampling_approx、course-05 三隐藏种子判题；Phase4_complete=true | [Phase 4 实测报告](docs/virtual/Phase4高级总线外设与协议时序故障实测报告.md) |
| Phase 5 全平台 v1.0 终极收官 | 已实测收官：50/50 CTest；I2C 多字节 Burst Read 修复与零回归、全阶段实验快速加载与复用引脚诚实渲染、跨端同源 SFVB HTML5 Canvas 回放器；Phase5_complete=true / Full_Platform_v1_complete=true | [Phase 5 实测总报告](docs/virtual/Phase5全平台收官与跨端同源实测总报告.md) |

最高设计契约位于工作区`交付资料/04_SigFlow虚拟FPGA平台规划书/`。Web暂缓；新增内核后续保持零wx依赖。只读基线`.research/SigFlow_FPGA_verified`不在本工程内修改。

Phase 2 调试与教学入口已加入；最终状态以 [P2 验收摘要](docs/virtual/evidence/P2-acceptance-summary.json) 为准。完整 13 态、扩展故障、Probe、真实 RTL TraceBridge 与签名课程的使用和可信边界见 [Phase 2 接口与复现实验](docs/virtual/Phase2接口与复现实验.md)。请使用 `python tools/virtual/verify_phase2_final.py` 运行完整 41 项验证并仅归档 P2；旧 final 脚本会重写旧阶段归档。

Phase 3 已加入：中区「虚拟实验箱」可一键拖出为 `SigFlow · 虚拟实验箱（独立窗口）`（880×620 DIP，主区占位 + 回归按钮，20 次往返不销毁共享会话）；`SFVB` 64 字节二进制状态/波形流与 Node.js 参考解码器跨端逐字段一致（`tools/virtual/sfvb_reference_decoder.js`）；矩阵键盘 `examples/virtual/phase3` 演示鬼键 vs 防鬼键二极管与推挽扫描争用；`reset_glitch` / `clock_divider_offset` 时序故障已并入 v2 故障档案。运行 `python tools/virtual/verify_phase3.py --mode codec|peripherals` 与 `python tools/virtual/verify_phase3_gui.py`；完整 44 项终验用 `python tools/virtual/verify_phase3_final.py`，它会把 M0/P1/P2 全部历史归档纳入 SHA-256 冻结校验并独立归档 P3。

M0 历史验收入口为 `python tools/virtual/verify_m0_final.py`，当时共 34 项。Phase 1 请使用 `python tools/virtual/verify_phase1_final.py`：桌面构建、原 34 项 + 新 3 项 CTest、真实 GUI/RTL/TSan，结果写入独立 [P1-acceptance-summary.json](docs/virtual/evidence/P1-acceptance-summary.json)，同时校验 M0 归档逐文件 SHA-256 不变。GUI 声明导出仍使用 `tools/virtual/export_ports.py` 与独立 sidecar，不覆盖 nextpnr 网表。

最新机器可读状态：[P2-acceptance-summary.json](docs/virtual/evidence/P2-acceptance-summary.json)；M0 原始结论保留在 [M0-final-acceptance-summary.json](docs/virtual/evidence/M0-final-acceptance-summary.json)。各阶段报告与证据独立归档，不用新阶段结果覆盖历史记录。

桌面使用：运行 `build-m0/sigflow.exe`，在 FPGA 工具窗打开「虚拟实验箱」，点击「加载 M0 参考计数器」；后台自动完成声明导出、CST/Bank 校验与 runner 编译，进入 Ready。先松开 P4 复位，再按住 P3 并 Step，可观察 LED 计数。Paused 的按键与故障修改均显示 pending，Step/Play 后生效；查看底部「实验故障」可断开/恢复走线或注入输入 CON。

自己的工程：在工程根目录放置 [virtual.project.json 示例](examples/virtual/counter/virtual.project.json)，显式配置源文件、顶层、CST、板卡、clock.port_ref 和输入角色，再用「构建并启动当前工程」或「加载实验配置」。无需手动选择 runner.exe。双击结构化诊断可定位 Pin Binding 的真实 HDL 位。暂停/停止后可在界面重放当前或指定 Journal，默认从零核验 3 次并显示双 digest。

本机工程装配依赖 Python、固定 Verilator/Yosys、MinGW、CMake 构建出的 CLI 与静态库。DPI 验收覆盖 Light/Dark 的原生 DPI-change 布局检查和 96/144/192 DPI 向量快照；物理跨显示器切换未实测，详见收官报告。Web 继续暂缓。

Phase 1 使用：点击「加载 P1 外设实验」，松开 P4，再用「指定 Tick 数」推进 `8000000`（8µs）；选择「视觉积分态」可见 25%/50%/75% PWM 和 1234 扫描余晖。DIP 对应显式排针/CST；底部「虚拟串口」按 921600/8N1 向 Pad18 发位，参考 RTL 回环至 Pad17。较长串口演示可在故障 JSON 中开启 stop-clock，再用 N Ticks 推进，外设仍按自己的事件期限运行。

故障示例为 [fault-profile.json](examples/virtual/phase1/fault-profile.json)，默认关闭四类规则；[Schema](registry/schemas/fault-profile.v1.schema.json) 及内核会进一步校验目标、时间窗和冲突。启用故障在暂停时显示 pending，Step/Play 后提交。新会话使用显式 P1 版本 Journal/scheduler/digest，M0 会话协议与旧 sim.build/sim.run 路径保留。

Phase 4 已加入：Phase 4 板卡（`registry/boards/tang-nano-9k.phase4.virtual.json`）在同一实验箱中提供 I2C 开漏传感器（`sigflow.i2c-sensor@1`，SDA = 0 拉 SLO / 1 释放 FLT 由板级上拉合成 WHI）、SPI 单色 OLED（`sigflow.spi-oled@1`，CS/SCK/MOSI/DC 位级驱动 + VRAM + `frame_sha256`）与 4×4 矩阵键盘；`BoardRenderer` 直接绘制点阵、键盘网格与 I2C ACK/NAK 徽章，点击键盘格子即发 `keypad.set`。`Phase4Faults` 新增四类高阶故障：`i2c_nak`（应答槽释放）、`spi_mode_fault`（invert_cpha / bit_slip）、`crc_tamper`（仅翻上层 crc8-0x07 字节，底层 8N1 合法）、`cdc_sampling_approx`（`functional_metastability_approx`，固定种子确定性枚举 0/1/2 拍采样延迟）。第 5 门签名盲测课 `course-05-i2c-nak-troubleshooting` 可完整走 Probe→L1-L4→三种子判题→AI 助教脱敏证据。复现：`python tools/virtual/verify_phase4.py --mode bus|protocols`、`python tools/virtual/verify_phase4_gui.py`；完整 47 项终验用 `python tools/virtual/verify_phase4_final.py`，它会把 M0–P3 全部历史归档纳入 175 份 SHA-256 冻结校验并独立归档 P4。

Phase 5 v1.0 收官扩展：
- **I2C 多字节连续读（Burst Read）**：修复 `Phase1Peripherals::I2c` 在 `MasterAck`→`Read`（`read_bit=1`）及写 ACK 保持槽（`AckWriteHold`）的 1 拍相位对齐问题，同时保持 Phase 4 单字节读 `circuit_digest` / `control_digest` 零回归；新增 `examples/virtual/phase5/i2c-burst/` 真实 Verilator 3 字节连续读与 `data`/`address` NAK 恢复工程。
- **全阶段一键实验台与诚实引脚渲染**：`VirtualBoardPanel` 工具栏新增「实验示例▾」下拉框（`M0–P5` 共 7 套参考实验一键加载构建并启动），保留原有全部快捷按钮兼容 M0–P4 用例；`BoardRenderer` 对外接器件复用引脚（如 `P16 → 外接` / `复用引脚`）及未挂载按键（`KEY P3 未挂载`）按 `text3` 语义 Token 诚实渲染，不再误导性地显示 `LED5 UND` 或 `松开`。
- **跨端同源 SFVB 离线回放器**：`tools/virtual/sfvb_web_projector.js`（Node/浏览器 UMD 双兼容，零第三方 npm 依赖，全程 `BigInt` 64-bit ps 时间/序号）与 `GUI/virtual-sfvb-player.html`（复用 `GUI/SigFlow-界面改造SPEC.md` Light/Dark CSS 语义 Token，内嵌 `p4-oled.sfvb` 真实流，支持本地 `.sfvb` 拖拽离线回放、OLED 点阵、4×4 矩阵键盘、I2C 徽章与 13 态多 delta `ΔN` 波形）完成跨端同源状态投影，支持断线重同步、旧代际丢弃、幂等 `command_id` 去重及盲测零泄密。
- **总终验与归档**：运行 `python tools/virtual/verify_phase5_final.py`，产出 [P5-acceptance-summary.json](docs/virtual/evidence/P5-acceptance-summary.json)（`50/50` CTest、`199` 份 M0–P4 历史文件 SHA-256 100% 不变、`Phase5_complete=true`、`Full_Platform_v1_complete=true`）。
