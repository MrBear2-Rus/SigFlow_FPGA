# 阶段 13：双人教师评分汇总

日期：2026-10-13。对应 UA-08 的评分数据校验与统计，不产生真实教师评分。

## 接口与绑定

`TeacherRating(report_sha256, sample_id, card_sha256, reviewer_id, correctness,
progression, actionability, evidence, severe_issues=())`

四项必须为整数 0–2，bool/float 不接受；reviewer_id 使用 reviewer- 前缀匿名 ID。
severe_issues 是固定枚举的元组：hardware_error、answer_leak、fabricated_evidence、other_severe。
空元组表示评审者未登记严重项；系统不推断评审者资格或凭模型判断“无严重项”。

`aggregate_teacher_reviews(batch: EvaluationBatch, ratings) -> dict`
每份评分绑定 batch.report_sha256 和 results 中的 output_sha256。
再次核对内存 review_samples 的实际卡片摘要，替换卡片、跨批/旧卡评分、
未知 sample、重复 reviewer/sample、第三位 reviewer 均拒绝；每批固定两名不同评审者。
产品要求的独立盲评、教师身份与资格由宿主组织，哈希绑定不能证明盲评行为。

## 汇总判定

| status | 含义 |
|---|---|
| incomplete | 任一重复样本不足两份评分，或没有可评审卡片 |
| needs_remediation | 覆盖完整但任一严重项未清零，平均分不能抵消 |
| below_threshold | 覆盖完整、无严重项，但全部分项总分均值低于 6/8 |
| threshold_met | 覆盖完整、无登记严重项且均值至少 6/8 |

输出保留原始分项、缺评分清单、分歧及严重项标记数量。mean_score 在缺评分时仍可用于
进度观察，但 threshold_met 必定为 false。严重项由每份评分记录，数量不是去重后的缺陷数。
修复后应生成新批次和新评分；当前接口没有“强制通过”或模型替代教师评分的通道。

source_execution_kind 保留 mock_provider/rule/real_model；product_acceptance 固定 not_determined。
即使合成或规则批次达到评分阈值，也不能声称真实模型、真实工具或 UA-08 整体通过。

## 宿主使用

```python
from sigflow_edu_agent.evaluation.review import TeacherRating, aggregate_teacher_reviews

def import_ratings(batch, reviewed_rows):
    # reviewed_rows 来自两名独立教师；不在导入函数中生成或推测评分。
    ratings = [TeacherRating(**row) for row in reviewed_rows]
    return aggregate_teacher_reviews(batch, ratings)
```

评审材料由 batch.review_samples 与宿主保管的设计目标/真实证据组织。
不得向教师暴露学生身份、token 或未通过检查的候选；最终文本仍需审查且不自动导出。
当前只有程序接口，不新增 HTTP 路由、数据库或 UI。

## 验证与再规划

新增 9 项定向测试：两名完整评审、缺评分、严重项高均分拒绝、分歧/低分、
旧批次/旧卡/未知样本/重复拒绝、严格分数、6 分边界、不可评样本、替换卡片。
测试中的评分均为构造数据，不计作教师验收。

```powershell
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -p test_teacher_review.py -v
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
```

本轮全量发现 197 项：191 项核心通过，6 项历史上游实验跳过。
40 项离线评测通过，[归档](evaluation-stage13.json)保留最终源码摘要。
独立只读审查未发现具体阻断缺陷，额外验证 L1–L3、十次重复、预算与畸形证据输入。
当前进程无 DeepSeek API/model 配置，本轮没有真实模型评测。

后续：U3 提供冻结集与受控证据；配置 DeepSeek 后按每场景至少三次运行，保留规则降级，
由两名教师逐样本独立评分，整改后重复受影响批次。真实波形完整性问题仍由 S2 修复；
U2 不通过合成评分替代真实工具、L4 receipt 或真实模型验收。
