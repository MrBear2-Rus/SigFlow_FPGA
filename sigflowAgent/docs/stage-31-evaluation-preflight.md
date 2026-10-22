# 阶段 31：真实评测批次的离线预检

日期：2026-10-21。正式组合入口为 `sigflow_u2.evaluation.preflight`，源码内旧名称仍可开发运行。仅使用标准库和现有冻结输入/证据/模式检查器。当前仓库仍未提供团队正式冻结的 `ModelCase` 集及其独立可信 manifest 摘要，因而本阶段没有运行真实 DeepSeek 批次，也没有完成教师或产品验收。既有 `fixtures_frozen.jsonl` 是 probe fixtures，不能作为本入口的冻结 `ModelCase` 输入。

## 执行方法

在 `sigflowAgent` 目录执行以下命令。没有输入时，输出一行固定结构 JSON，`can_run_real_batch=false`、`input_status=not_provided`、`issues=["frozen_model_suite_not_provided"]`，退出码为 1。

```bat
.venv\Scripts\python.exe -m sigflow_u2.evaluation.preflight
```

团队正式提供输入后，将下面的占位路径和摘要替换为操作者明确选择的本地文件与经独立渠道确认的 manifest SHA-256：

```bat
.venv\Scripts\python.exe -m sigflow_u2.evaluation.preflight --manifest "TEAM_MANIFEST.json" --cases "TEAM_MODEL_CASES.json" --manifest-sha256 "EXTERNALLY_PINNED_64_LOWERCASE_HEX" --repeats 3 --max-model-calls 256 --runtime-version self-developed-v1 --rag-version not_used
```

`--mode` 默认为 `deepseek`，这里只检查真实批次准备条件。`rule`、`mock` 或其他执行模式均报告 `unsupported_execution_mode`，不会启动模型。提供 course context 时必须传入具体有效的 `--rag-version`，不能继续使用 `not_used`。

## 输入与预算判断

- 复用 `evaluation.frozen.load_frozen_model_suite`：先核对独立 manifest pin，再核对 cases 字节摘要、schema、案例数量和完整 `ModelCase`/`Evidence` 结构。套件版本必须有效；两名 reviewer 的声明必须符合既有格式且互异。声明不认证教师身份。
- 输入案例总数必须至少 40，加载器现有限制最多 256。预检从不生成、补齐、复制或改写操作者的案例。单元测试中的 40 条合成 fixture 仅供边界验证，不是已交付的团队数据。
- `repeats` 为 3 至 10 的整数，`max_model_calls` 为 1 至 512 的整数。有效性与真实 runner 对齐：先调用 `valid_evidence`，仅有效证据再调用 `patterns.assess`；支持的案例计入 `eligible`。无效证据计入 `evidence_rejected`，其他非 supported 的模式判断计入 `unsupported_pattern`，两者独立显示且阻止 ready。
- `required_first_pass = eligible × repeats`；`required_with_repair = eligible × repeats × 2`。后者为每次首轮最多一次修复的保守上界。必须连修复预算也充足才 ready；例如 40 个有效且支持的案例、重复 3 次，首轮需 120 次，上界需 240 次。预算 120 会显示首轮充足、修复不足并退出 1。所需上界超过 512 时仍报告实际所需数量，不截断需求或偷偷降低重复数。
- `origin_counts` 原样计数 `fixture`、`tool`、`rule`，包含拒绝或不支持的案例；不会把 fixture 改成 tool，也不会因为 manifest 被 pin 就宣称这些标签已获独立真实性认证。

## 固定摘要与退出码

实际预检输出一行 `edu.evaluation-preflight.v1` JSON。核心字段为 `status`、`can_run_real_batch`、`input_status`、`issues`、`declarations`、`case_counts`、`origin_counts`、`budget`。缺输入和无效输入也保留相同字段结构；无法计算的预算为 null。版本仅显示有效性、reviewer 仅显示数量，不回显 case、问题、证据正文、reviewer ID、文件路径、配置参数原文或异常消息。

- 退出 0：`status=ready`，仅表示已选冻结输入及配置满足进入真实批次的准备检查。
- 退出 1：`status=blocked`，输入缺失或数量、证据、模式、RAG 声明、预算等条件不满足。
- 退出 2：`status=invalid_input`，参数、pin、文件、结构或声明校验失败。输出固定错误代码，不回显敏感值。

预检不读取模型配置或真实密钥、不联网、不调用模型、不生成评分或评审模板。`real_model=not_run`、`real_tools=not_run`、`teacher_review=pending`、`review_authenticity=external_signoff_required`、`semantic_quality=not_scored`、`product_acceptance=not_determined` 始终保留。`ready` 不证明模型服务可用、密钥有效、真实 EDA 成功、教师身份真实或产品已被接受；操作者仍需确认所选数据及其来源确为团队正式冻结交付。

## 后续交付与验证

真实输入到位且预检通过后，真实 DeepSeek 执行继续使用既有 `sigflow_u2.evaluation.runner run --mode deepseek`，传入同一组冻结文件、pin、重复数、预算和版本，并指定新的输出目录。预检不会代为调用该入口。真实模型结果须交由两名实际教师分别盲评，再经既有 `sigflow_u2.evaluation.runner review` 聚合并整改严重问题；评分不由预检预填。模型执行、两名教师评审、整改复查及产品接收仍是独立交付条件。

定向验证覆盖 pin 正确/错误及字节篡改、JSONL/负例结构拒绝、40 条门槛、整数边界、首轮和修复预算、证据拒绝与不支持模式分离、RAG 版本、来源计数、敏感值不回显，以及无 Key 子进程中禁止 settings/模型/网络调用。测试临时文件只在工作区内由测试自身创建并清理；没有生成正式团队案例。红绿日志位于工作区 `.superpowers/sdd/u2-stage28-31/preflight-red.log` 和 `preflight-green.log`。
