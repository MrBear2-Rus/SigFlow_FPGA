# 阶段 11：精确波形窗口与学生预测检查

日期：2026-10-13。对应自研 task UA-07 的波形事实检查部分。
拉取后基线为 f5a3dc8，含远端 c22531d 和此前本地 U2 提交；未发生合并冲突。

## 自研基线迁移

[当前决策](../../docs/edu-agent/DECISION-2026-10-07.md)取消 UCAgent 运行依赖与旧上游 Gate。
README、environment.md 与本地 task.md 已同步。历史适配代码和旧阶段报告保留供追溯，
不计入当前交付依赖；当前产品使用自研 TeachingLoop / TeachingSession 与标准库检查器。
实际运行的 agent/ sidecar 和 sigflowAgent 教学核心仍需 U1 联合接入，未迁移或改写 U1 的服务。

## 接口

`context.waveform.check_waveform(*, catalog, page, anchor, project_id, revision,
job_id, snapshot_id, signal_ids, start_tick, end_tick, origin='tool') -> WaveWindow`

当前默认 tool 输入返回 incomplete_waveform；只开放显式 fixture 输入用于开发。
原因是已复现的宿主完整性缺陷，修复并联调前不能把真实页改标 fixture 绕过门禁。

catalog 为 Gateway signals 页的 data，page 为 query 页的 data，非外层信封。
调用方须先完成请求鉴权、实例校验和可信 artifact 归属校验。由于波形页自身不携带
project/revision/job/hash，必须提供宿主可信 EvidenceAnchor；不能从 page 自造。
检查器只核对绑定与哈希格式，不读取本机 artifact 文件重新计算 SHA-256。

| 检查项 | 行为 |
|---|---|
| 项目/版本/Job/snapshot/artifact/hash | 与可信锚点一致且未过期；hash 为 64 位 hex，可带 sha256: |
| 查询窗口 | 与宿主请求的闭区间相同；tick 为十进制字符串，范围 uint64，无浮点转换 |
| 信号选择 | 明确 tuple，1–16 路、不重复；query 必须包含全部选中信号 |
| 元数据 | 选中信号必须具有 full_name 与 1–4096 位 width；query 中可选元数据必须一致 |
| 完整性 | exact/complete，has_more=false，cursor 为空、omitted_signals=0 |
| 跳变 | 不超过 10,000，逐信号非递减，全部位于闭区间；计数一致 |
| timescale | catalog/query 一致，采用 1/10/100 + fs/ps/ns/us/ms/s |
| 初值 | start=0 时必须 null；其他窗口可缺前值，不补零；initial_known 表示前值存在 |
| 数据预算 | 规范化后的信号名与位值最多 2 MiB；此限额不等同于 HTTP 信封字节上限 |

允许 catalog 为包含选中信号的分页目录，不要求检索整个设计的信号目录；
query 页则必须已完整，partial 即使 next_cursor 不存在也拒绝。
本阶段不拼接多页，不静默截断，不自动发起后续查询。

WaveWindow 与其中的信号/跳变不可变，修改原始 dict 不会改变已检查窗口。
原始二进制短值按 VCD 规则零扩展，x/z 从首位扩展；允许严格的带宽度 b/h 形式，
保留四态并拒绝任意数值文本、real 类型和丢失有效高位的截断。

## 先预测再观察

`compare_sample(window, signal_id, tick, expected) -> SampleComparison`

expected 由学生或可信宿主给出，不能由工具观察值反向补造。结果包含状态、
完整锚点、信号名、tick、timescale、预期与观察位值及 origin。

- matches：该采样点的已知观察值与明确预期一致。
- differs：该采样点的已知观察值与明确预期不同。
- inconclusive：缺前值或观察包含 x/z，不能作二态确定判断。

预期包含 x/z、未知信号或超出窗口会拒绝。采样采用该 tick 及之前的最后事件，
同 tick 的多条事件保持服务返回顺序并取最后值；这不是 HDL 调度区或因果先后证明。
不跨 timescale 自动比较，不推断复位极性/同步性，不从单点匹配宣称全设计正确。

```python
from sigflow_edu_agent.context.waveform import check_waveform, compare_sample

def check_student_prediction(catalog, page, trusted_anchor, signal_id, tick, expected,
                             query_start, query_end):
    wave = check_waveform(
        catalog=catalog, page=page, anchor=trusted_anchor,
        project_id=trusted_anchor.project_id, revision=trusted_anchor.revision,
        job_id=trusted_anchor.job_id, snapshot_id=trusted_anchor.snapshot_id,
        signal_ids=(signal_id,), start_tick=query_start, end_tick=query_end,
        origin='fixture',  # 本例只用于已标记的开发 fixture
    )
    return compare_sample(wave, signal_id, tick, expected)
```

query_start/query_end 来自宿主原始查询参数。观察保留在内部对象中，由 U2/S3 后续映射教学卡；
本阶段未自动创建 Diagnostic 或将 reset/simulation_mismatch 模式标为真实支持。

## 验证与状态

新增 17 项测试覆盖共享 exact fixture、uint64 上界、闭区间、空初值、x/z、
同 tick、多种位值、范围与分页拒绝、去别名及预算。测试先观察缺模块，再实施；
额外负例发现布尔宽度和位值扩展预算问题，修复后定向测试通过。
独立审查还发现 Unicode 信号名异常和宿主截断完整性缺陷，新增回归并修复/设门禁。

```powershell
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -p test_waveform.py -v
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
```

全量发现 175 项：169 项自研核心通过、6 项历史上游实验跳过。
40 项开发评测通过，[阶段归档](evaluation-stage11.json)记录最终源码摘要；26 份 Markdown 的链接和围栏检查通过。
独立复核确认两项审查问题已处理，17 项定向测试通过，未发现新的具体阻塞问题。
本地检查不代表真实 C++ 波形、双平台或教学效果验收。

## 阶段再规划

已复现的 S2 阻塞：WaveformService.cpp 第 218–227 行按事件或字节预算截断，
第 243 行只在仍有未处理信号时令 hasMore=true，第 244–248 行因此把最后一个
被截断信号标为 exact。transition_count 是保留条数，不能证明没有遗漏。
即使不到 10,000 条也可能发生字节截断；当前 DTO 不能在 Python 侧可靠推断。
U2 默认拒绝真实 tool 输入，保留 fixture 来源到结果中。S2 修复末信号分页/完整性、
提供对应限额回归和服务版本后，再单独评审解除门禁；本轮未修改 S2 C++ 文件。

S2 提供真实 waveform artifact、目录/完整 query 页和可信锚点。U2 与 S3 确定学生预测
动作、窗口选择与结果卡字段，再把检查器接到自研工作流。
U1 完成 Gateway typed 查询与实例/请求绑定；解除工具门禁后再接入本检查器。
真实授权和 L4 receipt 继续默认拒绝；U3 的 RAG 目录与实现本轮未调整。
