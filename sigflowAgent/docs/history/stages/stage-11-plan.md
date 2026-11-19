# 阶段 11：自研基线对齐与精确波形检查

日期：2026-10-13。基线：远端 c22531d 与本地 U2 提交合并后的 f5a3dc8。
依据：docs/edu-agent/DECISION-2026-10-07.md、docs/edu-agent/ucagent/task.md UA-07、
contracts/edu-agent/v1/schemas/wave-query.schema.json。

设计：在 context/waveform.py 增加纯数据检查与有界采样对比，复用可信 EvidenceAnchor。
相比直接生成新诊断或在 Python 执行仿真，该方案先解决事实完整性，不越过 S2 的事实来源。
信号 metadata 由 signals 页提供；query 页必须覆盖指定闭区间与全部选中信号。
只接受单个无截断 exact/complete 页，partial、has_more、omitted_signals 或 cursor 未结束均拒绝。
使用 uint64 整数 tick，不转换为浮点；保留 x/z，空初值不视作零。
宿主显式提供预期采样值，返回 matches/differs/inconclusive 及范围引用，不宣称全设计正确。
不进行网络查询、不写 RTL、不对 reset 规则自动创建 Diagnostic 或解锁教学模式。

- [x] 先写失败测试：范围/哈希/元数据、分页/限额、tick 精度、初值、四态、采样对比。
- [x] 实现严格检查、不可变窗口与确定性采样结果。
- [x] 运行合并后回归及本阶段测试，核对共享 fixture。
- [x] 更新 README、环境和 task.md，以自研决策为当前基线，保留历史记录。
- [x] 产出阶段报告、独立审查及再规划。

审查后的设计修订：发现当前宿主把最后一个截断信号标为 exact，且 Python 无法可靠
推断字节截断。因此 origin 默认 tool 时拒绝；显式 fixture 路径保留用于算法回归，
来源标记保留到 WaveWindow 和 SampleComparison。S2 修复后另行联调解除门禁。

本轮只在 sigflowAgent 开发，不提交、不推送。当前无关本地改动已保存在独立 stash，暂不恢复。
