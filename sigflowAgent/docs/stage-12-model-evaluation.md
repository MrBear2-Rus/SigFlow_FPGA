# 阶段 12：重复模型评测

日期：2026-10-13。对应 UA-08 的有界 runner，冻结题集仍由 U3 提供。

接口：evaluation.model.ModelCase(case_id, question, level, evidence)，以及
run_model_evaluation(cases, provider, *, suite_version, mode='deepseek', repeats=3,
max_model_calls=256, runtime_version='self-developed-v1', rag_version='not_used')。
cases 为宿主构建的列表/元组，最多 256 个匿名场景，level 限 1–3；suite_version 标识宿主冻结批次。
runner 开始前复制输入，记录输入摘要；同批重复 ID、无效参数先拒绝，不调用模型。

每场景重复 3–10 次，每次最多一个初始候选加一次修复，全批最多 512 次模型调用。
证据不合规不生成卡片；无可靠 detector 支持时只产生保守规则提示。复用 checked_card
检查等级、字段、引用和代码特征，模型错误使用固定原因并降级，不保存被拒绝候选。
这是独立 L1–L3 候选评测，不代替 E0–E7 动作生命周期或真实工具闭环测试。

mode=deepseek 必须传实际 DeepSeekProvider；测试替身必须标 mock，即使只替换真实适配器的
HTTPS 传输。rule 模式要求 provider=None。运行模式是可信宿主声明，无法证明外部调用方未替换传输。
真实批次使用 DeepSeekProvider.from_env()，会发送场景问题和证据，并可能产生 API 费用。
本阶段未发起真实请求，当前进程无 API/model 配置。

返回 EvaluationBatch：report 每次为独立 JSON 对象，report_sha256 绑定本报告，
review_samples 为通过检查的最终 Card 的内存元组。报告仅保留摘要、固定原因和计数；
不会输出问题、证据摘要、原始候选或凭据。最终卡片正文仍需教师审查，只由可信宿主
按数据范围整理评审材料，不自动打印或导出，不能把通过结构检查视为语义安全证明。

报告记录源码、运行时、工作流、策略、实际 DeepSeek SYSTEM_PROMPT 摘要、model ID、
RAG 版本及 repeats/max_calls/max_tokens/timeout/base_url。temperature 未显式发送，记录 null 与
provider_default_unavailable，不猜测服务端默认值。course 上下文需显式提供 RAG 版本。
每场景记录 distinct_output_count、模型输出次数、实际 attempted_repetitions 和原因种类，所有重复结果都保留。
预算耗尽时保留未调用模型的行，model_repetitions_complete=false，不把这些规则输出当成已完成模型重复。
服务端精确模型版本和 token 用量不可获得时标 unavailable。
teacher_review=pending、semantic_quality=not_scored；real_tools=not_run。
model_passed 仅表示模型候选通过确定性检查，不代表教师评分或教学效果通过。

## 测试与再规划

新增 13 项测试通过：重复/波动、修复次数、预算、错误脱敏、无效证据/模式门禁、
DeepSeek 适配器参数记录、mock/real 分离、UTF-8 与输入参数、来源脱敏和畸形上下文拒绝。
HTTPS 测试使用替身。checked_card 增加 UTF-8 检查，避免发布不可编码候选。

```powershell
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -p test_model_evaluation.py -v
```

宿主对真实冻结集的调用方式如下；调用方须确认问题/证据范围并配置真实账户：

```python
from sigflow_edu_agent.deepseek import DeepSeekProvider
from sigflow_edu_agent.evaluation.model import run_model_evaluation

def evaluate_frozen_cases(frozen_cases, suite_version, runtime_version, rag_version):
    return run_model_evaluation(
        frozen_cases, DeepSeekProvider.from_env(), mode='deepseek',
        suite_version=suite_version, runtime_version=runtime_version,
        rag_version=rag_version, repeats=3, max_model_calls=256,
    )
```

下一阶段绑定 report/card 摘要汇总双人评分；冻结题集、真实配置和教师参与仍是验收前提。
不修改 U3 RAG，不放开波形 tool 门禁或执行授权。

本批独立审查通过；最终全量 191 项核心通过、6 项历史实验跳过，40 项离线评测通过。
