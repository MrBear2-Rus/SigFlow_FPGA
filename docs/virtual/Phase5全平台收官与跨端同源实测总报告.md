# Phase 5 全平台收官与跨端同源实测总报告

验收时间：2026-11-23T11:46:57.751442+08:00；基线 `ec24b72`；分支 `feat/virtual-fpga-phase5`。

最终状态：**passed**，`Phase5_complete=true`，`Full_Platform_v1_complete=true`。

| 验收 | 实测结果 |
|---|---|
| 产品 CTest | 50/50；原 47 项 + 新 3 项；附加回归 2/2 |
| I2C Burst Read 与零回归 | 20 项真实 Verilator 断言；3 字节连续读、MSB=1、data/addr NAK 恢复、Phase 4 双 digest 零回归 |
| 跨端同源 SFVB 与 Web 投影 | 3001 项；5 条真实工程流 100% 逐帧同源一致 + 断线重同步/幂等 ACK/BigInt 64-bit/零泄密 |
| GUI 全阶段实验台与诚实渲染 | 28 项真实 wxApp 断言；7 实验一键加载、`P16 → 外接`、`KEY P3 未挂载`、浮动窗交互与 Light/Dark 离屏导出 |
| 历史归档保护 | 199 份历史文件（M0–P4）SHA-256 100% 不变：True |
| 参与验收的源码 | 构建与测试前后逐文件哈希一致：True |

## 实现与证据

| 模块 | 生产入口 / 证据 |
|---|---|
| 模块 A：I2C Burst Read 修复 | `Phase1Peripherals::I2c`（`MasterAck` `read_bit=1` 与 `AckWriteHold`）+ `examples/virtual/phase5/i2c-burst/`；`P5/i2c_burst-results.json` |
| 模块 B：全阶段 GUI 与诚实引脚渲染 | `VirtualBoardPanel::Examples` / `LoadExample` + `BoardRenderer::LedSlotStatus` / `KeySlotStatus`；`P5/gui/full-showcase-*.png` |
| 模块 C：跨端同源 SFVB 投影与回放器 | `tools/virtual/sfvb_web_projector.js` + `GUI/virtual-sfvb-player.html` + `SPEC §7.5`；`P5/web_parity/*.sfvb` |
| 模块 D：全平台 v1.0 50/50 终验 | `tests/virtual/p5/` + `tools/virtual/verify_phase5_final.py`；`P5-acceptance-summary.json` |

## 复现与边界

执行 `python tools/virtual/verify_phase5_final.py`；三项新测试可独立运行：`python tools/virtual/verify_phase5.py --build-dir build-m0 --mode i2c_burst|web_parity` 与 `python tools/virtual/verify_phase5_gui.py --build-dir build-m0 --gui build-m0/tests/virtual/u1/u1_m0_full_product_acceptance.exe`。

- Digital nominal voltage/state models; no SPICE current or analog metastability
- I2C SDA is an open-drain 13-state bus: 0 drives SLO, 1 releases to FLT and the board pull-up synthesizes WHI
- SPI OLED is an SSD1306 teaching subset; no analog charge-pump or DMA timing
- Web SFVB player (GUI/virtual-sfvb-player.html + sfvb_web_projector.js) is an offline same-source binary stream projector and inspector, not a second Web simulation backend
- Local blind courses provide educational isolation, not hardware-enforced DRM against machine owners
- Physical multi-monitor hot-plug and real-hardware electrical timing remain out of scope

机器可读结论和逐文件哈希见 [P5-acceptance-summary.json](evidence/P5-acceptance-summary.json)。M0–P4 历史报告与归档保持原字节未变。
