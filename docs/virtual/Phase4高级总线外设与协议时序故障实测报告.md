# Phase 4 高级总线外设与协议时序故障实测报告

验收时间：2026-11-21T12:30:24.031861+08:00；基线 `d7ba89d`；分支 `feat/virtual-fpga-phase4`。

最终状态：**passed**，`Phase4_complete=true`。

| 验收 | 实测结果 |
|---|---|
| 全部 CTest | 47/47；原 44 项 + 新 3 项 |
| I2C/SPI-OLED/总线故障 | 26 项真实 RTL 断言；开漏 SDA、ACK/NAK、VRAM 摘要 |
| CRC/CDC/SFVB | 29 项；上层 CRC 篡改、功能级 CDC 近似、C++↔JS 一致 |
| GUI 与第 5 门课 | 33 项真实 wxApp 断言；键盘点击、OLED 点阵、course-05 三种子判题 |
| 历史归档 | 176 份 SHA-256 保持：True |
| 参与验收的源码 | 构建前后逐文件哈希一致：True |

## 实现与证据

| 模块 | 生产入口 / 证据 |
|---|---|
| I2C 开漏传感器 | Phase1Peripherals::I2c + BoardTopology junction；P4/bus-results.json |
| SPI OLED | Phase1Peripherals::Oled（VRAM + frame_sha256）；P4/bus-results.json |
| 总线/协议/CDC 故障 | Phase4Faults（i2c_nak、spi_mode_fault、crc_tamper、cdc_sampling_approx） |
| SFVB 跨端 | BinaryFrameCodec + sfvb_reference_decoder.js；P4/protocols/*.sfvb |
| GUI | BoardRenderer 键盘/OLED/I2C + VirtualBoardPanel 点击；P4/gui/*.png |
| 第 5 门盲测课 | generate_phase4_assets.py 签名 course-05；P4/gui/course05-evidence.json |

原生代数测试仍使用显式替身；I2C/SPI/CRC/CDC 与 GUI 验收全部编译并运行真实 Verilator DUT 与真实 wxApp。

## 复现与边界

执行 `python tools/virtual/verify_phase4_final.py`；三个新模式可单独运行：`python tools/virtual/verify_phase4.py --mode bus|protocols` 与 `python tools/virtual/verify_phase4_gui.py`。

- Digital nominal voltage/state models; no SPICE current or analog metastability
- I2C SDA is an open-drain 13-state bus: 0 drives SLO, 1 releases to FLT and the board pull-up synthesizes WHI
- SPI OLED is an SSD1306 teaching subset (page addressing, 0xAE/0xAF/0xA6/0xA7/0xB0..0xB7, column nibbles); no DMA/charge-pump modeling
- crc_tamper only flips the upper-layer crc8-0x07 byte; bottom-layer 8N1 framing and baud stay legal by construction
- cdc_sampling_approx is a functional_metastability_approx: seeded whole-cycle sampling branches, never analog metastability or MTBF
- Matrix keypad ghosting follows the digital junction model; diode mode conducts only the scanned row
- Course 05 grading runs on the trusted side with three hidden seeds; local blindness does not protect against a machine owner
- Physical multi-monitor and real-hardware I2C/SPI electrical timing are out of scope
- Web remains deferred; SFVB frames are consumed by the Node reference decoder

机器可读结论和逐文件哈希见 [P4-acceptance-summary.json](evidence/P4-acceptance-summary.json)。原 M0/P1/P2/P3 报告及归档保持原字节。