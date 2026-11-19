# 阶段 26：冻结模型输入与人工评审文件流程

日期：2026-10-20。对应 UA-08 的可运行评测交接，不代替 UA-11 数据冻结或实际教师评分。

## 输入与版本绑定

`evaluation.frozen.load_frozen_model_suite` 读取操作者明确指定的 manifest 和 cases 文件。manifest 限 16 KiB，cases 限 4 MiB、1–256 个唯一 ModelCase；要求外部另行提供 manifest 原始字节的 SHA-256，并核对 cases 原始字节摘要、数量和版本。拒绝重复 JSON 键、非有限数字、额外字段、错误标量类型、损坏嵌套 DTO 或体积超限。

manifest 格式为：

```json
{
  "schema_version": "edu.frozen-model-suite.v1",
  "suite_version": "team-approved-version",
  "suite_sha256": "cases 文件原始字节的 64 位 SHA-256",
  "case_count": 40,
  "reviewers": ["reviewer-first", "reviewer-second"]
}
```

cases 为 JSON 数组，每项严格对应 ModelCase 的 case_id、question、level、evidence；Evidence 及嵌套值采用现有 dataclass 的完整字段结构。加载为受控域对象，不跟随输入中的路径，不执行工具，不把 fixture 改标为 tool。合法的负例可保持 incomplete/expired/unsupported，交由现有评测门禁拒绝。

reviewers 仅为已选批次的评审者标识声明；外部摘要、声明或文件加载成功都不证明教师身份、资格或团队已完成冻结。少于团队要求的案例数量也不能作为 UA-11 完成证据。

## 操作入口

在 sigflowAgent 目录使用已有 .venv。规则模式不读取模型凭据、不调用 API：

```powershell
.\.venv\Scripts\python.exe -m sigflow_edu_agent.evaluation.runner run `
  --manifest <已冻结的manifest.json> --cases <cases.json> `
  --manifest-sha256 <独立核对的摘要> --output-dir <新的输出目录>
```

只有显式添加 `--mode deepseek` 才加载现有模型配置并调用真实 API。`--repeats` 为 3–10，默认 3；`--max-model-calls` 默认 256、上限 512。上下文含课程时必须提供实际 `--rag-version`。工作流、策略、模型参数、源代码和提示词版本沿用已有评测记录。输出目录须不存在，避免重跑覆盖或重复消耗 API；失败后的目录保留，不自动删除。

## 输出与人工评审

| 文件 | 用途 |
| --- | --- |
| batch.json | 操作者保存的检查后卡片、完整评测报告及其 SHA-256，供后续重新加载和评分绑定 |
| review-packet.json | 顺序打乱的评审材料，含问题、证据、检查后卡片及绑定摘要，不含 producer、模型名称或连接参数 |
| ratings-template.json | 操作者汇总用的空评分模板，不含预填分数 |
| ratings-reviewer-*.json | 每名评审者各自的空模板，分别分发；不共享对方已填评分 |

每份输出限 16 MiB。只导出已检查卡片，不保存模型原始候选；若最终输出包含已加载 API Key 则拒绝导出。材料可能包含操作者选择的冻结证据内容，按团队的评审资料范围分发，不将整个 batch.json 当盲评材料。

公开卡片摘要只覆盖去除 producer 后实际分发的卡片；评分模板使用同一公开摘要。评分导入先验证其与固定内部样本公开视图一致，再转换为内部原卡摘要供既有汇总器核对。内部 batch.json 保留完整来源和原卡摘要，仅供操作者保存；评分汇总同样属于内部材料。旧版含 producer 的摘要不作为公开评分摘要兼容接受，应重新导出盲评材料。分发前应核验卡片与其公开摘要，避免评审材料被替换。

合并两份真实评分数组后，调用：

```powershell
.\.venv\Scripts\python.exe -m sigflow_edu_agent.evaluation.runner review `
  --batch <batch.json> --report-sha256 <生成时记录的报告摘要> `
  --ratings <合并后的评分.json> --output <新的评分汇总文件.json>
```

重载时校验报告外部摘要，以及每张卡片、case、repetition、evidence 和 sample 的绑定。评分维度各为整数 0–2；空分数不能自动转为 0 或视作已评分。缺少两人覆盖返回 incomplete，严重问题返回 needs_remediation，阈值沿用均分至少 6/8。输出仍明确 product_acceptance=not_determined。修复后须生成新批次、重新评分，不提供覆盖严重问题的开关。已有汇总文件不覆盖，应选择新文件名。

退出码：0 表示本次操作达到对应程序条件；1 表示评测/评分未达条件；2 表示输入、配置或输出无效。错误为固定脱敏 JSON，不输出原始异常、Key 或本地路径。0 不代表真实教师或产品验收已经完成。

## 验证与再规划

新增加载测试 5 项、流程测试 5 项，覆盖摘要篡改、严格 DTO、错类型、未知字段、重复案例、嵌套重建、真实规则子进程、空评分、外部评分/严重问题、卡片篡改及输出保护；测试评分为合成数据，不归档为教师证据。

独立审查发现原公开摘要包含已隐藏的 producer，可由两个候选值枚举还原；已修复。另增加 3 项先失败后通过的流程回归，验证枚举不能恢复来源、篡改公开卡片摘要被拒绝，以及内部摘要不能替代公开评分摘要。阶段 26 合计新增 13 项测试；连同重复建议、既有模型评测和评分汇总的 44 项相关回归无失败，独立复核确认修复。未改变汇总器的教师覆盖、四维分数或严重项门禁。

本地操作链已具备，后续等待 U3 的真实冻结 ModelCase 和两名教师独立评分。本轮继续核对新基线的仿真时长参数和真实工具闭环，不创建一份伪装成团队验收的数据集。
