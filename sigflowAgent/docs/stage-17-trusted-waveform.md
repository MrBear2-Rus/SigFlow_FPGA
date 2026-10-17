# 阶段 17：可信波形读取

日期：2026-10-16。接续阶段 16；实现与阶段 18 联合验收，不提交或推送。依赖结论及后续边界见[剩余开发计划](u2-remaining-development-plan.md)。

## 服务契约

Core 的 `waveform_query_contract=whole-signals-v1` 只声明查询语义，不表示后端可用或授权执行。U2 只接受认证实例发布此标记的服务；旧实例继续返回明确缺口。

QueryRange 只发布完整信号。事件或序列化字节不足时省略整条信号，返回 partial；没有进展时不生成重复游标。exact 与 has_more 独立判断，恰好用尽事件预算仍可返回后续零事件信号。游标绑定文件摘要、时间范围与信号顺序，严格校验偏移和范围。字节预算计入实际 JSON 编码、uint64 十进制时间、初值和预留 envelope。

VCD 后端将同一 idCode 的全部别名映射到同一事件序列，保留 tick 0 和同 tick 的事件顺序。负数、带符号、溢出、后缀或倒退时间戳，以及未声明标识、无效位宽、异常数字值，均使加载失败并清空部分结果。支持数字波形及压缩二进制/X/Z 扩展；不支持的 real/string 值明确拒绝。独立审查发现的展开内存问题已修复：位宽最多 4096、信号最多 65536，事件展开按每个别名的位串及 WaveTransition 对象计入 64 MiB 预算，分配前检查；这不是整个进程内存的上限。

## U2 读取端口

`GatewayWaveformReader.describe(binding, artifact_id)` 只读取元数据；`read(..., signal_ids=(id,), start_tick='0', end_tick='0')` 才查询事件。端口只构造固定 loopback 路由，不接受 URL、路径、原始页或调用方锚点。报告、成功状态、能力/插件、project/revision/snapshot/Job、产物 schema/大小/摘要必须一致。查询后再次核验文件摘要。

信号目录最多读取 4096 项；一次最多 16 路、10000 个事件、2 MiB。目录超界或查询不能容纳完整窗口时要求缩小范围；不自动拼接未经校验的页、不补零。timescale 与 uint64 tick 始终显式传递。

## 验证

- C++ `edu_u2_wave_service_test`：初始 10 项中 5 项失败；修复后通过，增加最大 uint64 的 JSON 字节预算回归后最终 11/11。
- C++ `edu_u2_vcd_integrity_test`：别名与异常时间戳先失败；追加资源限制回归为 41 项/3 失败，修复后 CMake 集成构建最终 41/41。
- Python `test_gateway_waveform.py`：9/9；原精确检查器的 17 项与默认仿真转移的 3 项纳入全量回归。
- [真实联合报告](closed-loop-stage18-prediction.json)：1 个真实 Yosys 基线、Verilator build/run 共 3 Job；394 字节 VCD，TOP.q、1 bit、1ps、窗口 0..0，1 个事件，origin=tool，摘要和作用域通过。

早期 `closed-loop-stage17-wave.json` 是后端别名修复前的探索记录，不用于本阶段验收。最终证据采用上述阶段 18 报告。最终全量数字、源码摘要及审查状态统一记录于[验收汇总](verification-stage17-19.json)。

## 再规划

当前 harness 仅执行一次 eval/dump。精确读取一个静态样本不代表动态激励、复位约定或设计功能正确。阶段 18 在此端口上实现预测先于观察；动态输入契约与真实 GUI 签收保持独立依赖。
