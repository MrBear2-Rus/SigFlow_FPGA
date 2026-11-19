# Phase 2 调试探针与教学判题闭环实测报告

验收时间：2026-11-19T15:07:01.147823+08:00；基线 `415b5a2`；分支 `feat/virtual-fpga-phase2`。

最终状态：**passed**，`Phase2_complete=true`。

| 验收 | 实测结果 |
|---|---|
| 全部 CTest | 41/41；原 37 项 + 新 4 项 |
| 冻结 STATE 表与原生核心 | 2132 项检查；包含 1024 格逐格对照 |
| 扩展故障/重放 | 37 项真实工具断言 |
| Probe/双模式 TraceBridge | 27 项真实工具断言；实际 RTL COBS+CRC16 回包 |
| 四课程/网关/判题 | 48 项；错误权限、错定位、未修复 RTL 拒绝，正确修复三组隐藏激励 |
| GUI | 22 项真实 wxApp 断言；原生 Light/Dark 截图 |
| 历史归档 | 88 份 SHA-256 保持：True；60 M0 + 27 P1 artifact + P1 summary |
| 参与验收的源码 | 构建前后逐文件哈希一致：True |

## 实现与证据

| 模块 | 生产入口 / 证据 |
|---|---|
| STATE | State13 / BoardTopology；registry/logic/state13.v1.json；P2/state13-native.json |
| 拓扑/协议故障 | Phase2Faults / Phase1Session / Phase1Peripherals；P2/faults-results.json；P2/journals |
| 只读仪表/严格对齐 | VirtualInstruments / VirtualCaptureArtifacts / WaveCompareSession；P2/probes-results.json |
| 实际 RTL 协议 | build_virtual_tracebridge.py / BitClockTransport / DebugProtocol / DebugAcquisition；P2/rtl-probes/capture.raw/vcd |
| 可信教学 | virtual_teaching_service.py / BlindProjection / VirtualTeachingClient；签名课程；P2/student-dtos、teacher-grading |
| 真实 GUI | VirtualTeachingPanel + Board/Fault/Trace/UART；P2/gui-results.json；P2/gui/teaching-light.png、teaching-dark.png |

原生核心测试中的计数器替身仅用于代数/会话隔离单测；真实工具断言及 GUI 验收另行编译并运行 Verilated DUT。采集回包逐值核对独立 wrapper tap，禁止用生成 VCD 冒充 RTL 协议采集。

Mode A 的最早实际分歧是开路线末端 valid 状态，不会将无效样本填成 0。Mode B 与同相位独立参考轨严格比较；跨 pre/post-eval 相位明确拒绝。

## 复现与边界

执行 `python tools/virtual/verify_phase2_final.py`；需要交互式 Windows 桌面和固定工具链。详细契约、版本差异和使用步骤见 [接口与复现实验](Phase2接口与复现实验.md)。

- Digital nominal voltage/state models; no SPICE current or analog metastability
- Canonical driver order; STATE fold is not assumed associative
- Mode A post-eval ring capture and Mode B pre-eval finite linear RTL capture have different explicit phases; cross-phase comparison is inconclusive
- RTL transport is dedicated wrapper UART; not board Pad17/18 transport validation; pretrigger=0
- Course03 1kHz and Course04 1MHz/100kbaud are explicit teaching fixtures; normal P2 board remains 27MHz
- Teacher course and private journal directories must be deployed on trusted host; local blindness does not protect against machine owner
- Interactive Windows GUI screenshots cover Light/Dark; physical multi-monitor switching not newly tested
- Web deferred

机器可读结论和逐文件哈希见 [P2-acceptance-summary.json](evidence/P2-acceptance-summary.json)。原 M0/P1 报告及归档保持原字节。
