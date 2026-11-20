# Phase 3 桌面产品化深化与跨端同源实测报告

验收时间：2026-11-20T21:54:21.264510+08:00；基线 `4edd496`；分支 `feat/virtual-fpga-phase3`。

最终状态：**passed**，`Phase3_complete=true`。

| 验收 | 实测结果 |
|---|---|
| 全部 CTest | 44/44；原 41 项 + 新 3 项 |
| SFVB 原生代数/故障/键盘 | 79 项检查；含 64 字节头逐字段小端对照 |
| SFVB 跨端一致性 | 30 项；真实 Verilator 轨迹 C++↔JS 逐字段一致 |
| 矩阵键盘/时序故障 | 33 项真实 RTL 断言；鬼键 vs 二极管、CON 争用守卫 |
| GUI | 147 项真实 wxApp 断言；20 次弹出↔回归 + 修复后 Light/Dark 截图 |
| 历史归档 | 150 份 SHA-256 保持：True；M0+P1 88 份与全部 P2 归档 |
| 参与验收的源码 | 构建前后逐文件哈希一致：True |

## 实现与证据

| 模块 | 生产入口 / 证据 |
|---|---|
| SFVB 二进制帧 | BinaryFrameCodec（零 wx）；P3/codec/frames.sfvb、frames.json、decoded.json |
| JS 参考解码器 | tools/virtual/sfvb_reference_decoder.js（DataView/BigInt）；P3/codec/js-decoded.json |
| 时序故障 | Phase3Faults：reset_glitch / clock_divider_offset；P3/peripherals-results.json |
| 矩阵键盘 | sigflow.matrix-keypad@1 + phase3 板卡 + examples/virtual/phase3；鬼键 vs 二极管可配 |
| 独立浮动窗口 | VirtualBoardFloatFrame + 占位页；20 次往返不销毁共享会话 |
| 离屏截图修复 | PrintWindow(PW_RENDERFULLCONTENT) + WM_PRINT 递归合成；P3/gui/teaching-light.png、teaching-dark.png |
| 教学 Agent 联动 | VirtualTeachingPanel::ComposeEvidence → AgentApiClient 通道；P3/gui/agent-evidence.json |

原生核心测试中的替身仅用于代数/会话隔离单测；SFVB 轨迹、键盘、故障与 GUI 验收均以真实 Verilator DUT 与真实 wxApp 执行。盲测运行经 StudentProjection 投影后再编码，跨端解码仍零泄密。

## 复现与边界

执行 `python tools/virtual/verify_phase3_final.py`；需要交互式 Windows 桌面、Node.js 24 与固定工具链。三个新模式亦可单独运行：`python tools/virtual/verify_phase3.py --mode codec|peripherals` 与 `python tools/virtual/verify_phase3_gui.py`。

- SFVB v1 frames are the frozen sigflow.virtual-frame.v1 projection; payload trace blocks are uncompressed
- JS reference decoder covers the frame layer with DataView/BigInt; no browser runtime claims
- Float window migration is view-host only; physical multi-monitor dragging is manual UX, placement is verified per-display
- Offscreen capture renders via PW_RENDERFULLCONTENT with a recursive WM_PRINT fallback; no analog voltage is claimed
- Matrix keypad ghosting follows the digital junction model; push-pull scan contention pauses under the strict UnknownInput guard
- reset_glitch and clock_divider_offset are digital teaching timing faults; no analog metastability or MTBF claims
- Diode board variant is generated at acceptance time from the reviewed phase 3 board; registry stays single-source
- Agent bridge sends student projections only; grading stays with deterministic teaching.grade.submit
- Web remains deferred; binary frames are consumed by the reference decoder, not a browser GUI

机器可读结论和逐文件哈希见 [P3-acceptance-summary.json](evidence/P3-acceptance-summary.json)。原 M0/P1/P2 报告及归档保持原字节。