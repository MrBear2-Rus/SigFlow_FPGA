# 阶段 19：依赖补强与评测交接

日期：2026-10-16。当前依赖未全部交付；本阶段区分可验证的协议准备与实际教学发布。

## Receipt 协议准备

Core ReceiptStore 核销要求非空 project/run/action，匹配签发 action 与 state_version；幂等重放额外匹配同 run、action 和版本。已提交结果可在 TTL 后恢复，显式作废仍阻止重放，重启使未核销凭据失效。持久化数字和 Gateway 版本字段严格解析，拒绝带符号、后缀和溢出值。

UI 签发请求可提供 state_version，省略时保留兼容值 0；Agent GET 返回签发版本。Agent 没有签发能力。`GatewayReceiptClient` 仅允许固定 lookup/consume 路由，验证实例与回执。

`host/reference.py` 提供未启用的 ReferenceGate 端口：可信生产方提供带摘要和两名不同 reviewer 的 ReviewedReference；request 不发布正文，生成绑定 learner/session/project/revision/issue/run/policy/material/version 的随机 challenge/action；confirm 比对认证 Core receipt 并成功核销后才能返回内容。普通 HTTP 和模型不能指定审阅身份、材料或签发凭据。reviewer ID 只是可信生产方的审阅声明，端口不自行认证教师。

该 gate 是进程内协议准备，不具有持久发布事务，不能直接接入生产教学 HTTP。仓库没有已审阅参考材料或实际 GUI 签收，主循环与宿主继续返回 receipt_unavailable，L4 advertised available=false。后续启用须增加材料生产方和“核销后崩溃”的持久恢复/发布事务，再单独审查；不得把本阶段 fixture 内容当作课程答案。

C++ 核销回归先 32 项/11 失败，修复后通过；追加签发版本及严格数字后最终 39/39。Python 协议 5/5。[真实 Core 探针](receipt-stage19-core.json)验证 Agent 签发拒绝、版本/动作/项目错误拒绝、同 run 幂等和跨 run 拒绝；默认版本 0 与旧的无签发 action 模式保留兼容，但核销 project 必填，原 smoke 用例的三处请求已补齐 project。未执行会删除固定临时目录的旧 C++ smoke 二进制。Job=0、模型=0；材料明确为 synthetic fixture，GUI 和真实 L4 均未验收。

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& sigflowAgent/.venv/Scripts/python.exe -P -m sigflow_edu_agent.host.receipt_acceptance --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --output sigflowAgent/docs/receipt-stage19-core.json
```

## 八类模式支持

`patterns.support.support_matrix()` 从版本化模式注册表生成真实 detector、所需事实、排除条件与缺口，随[交接报告](evaluation-stage19-handoff.json)输出。latch 保留 EDU_LATCH_INFERRED 的生产映射及独立设计目标。WIDTH、MULTIDRIVEN、UNOPTFLAT 等标签不能自动升级为硬件根因；语法、驱动、连接、反馈、类型和复位分别等待可信分析事实。simulation_mismatch 支持显式采样点比较，未开放任意动态根因。

## 冻结集与统一评测

现有 fixtures_frozen.jsonl 为零字节；开发集仍 pending_hardware_review。新增 handoff 入口统一运行 40 项开发 probe、输出模式矩阵及冻结缺口，不修改空冻结集或审阅状态。

```powershell
& sigflowAgent/.venv/Scripts/python.exe -P -m sigflow_edu_agent.evaluation.handoff --development-suite sigflowAgent/tests/evaluation/development.json --frozen-fixtures sigflowAgent/tests/evaluation/fixtures_frozen.jsonl --output sigflowAgent/docs/evaluation-stage19-handoff.json
```

可选冻结 probe 输入需同时提供 --frozen-manifest、--frozen-suite 和外部确认的 --manifest-sha256。manifest 使用 edu.frozen-probe-suite.v1，包含 suite_version、原文件 suite_sha256 与至少两名不同 reviewers；suite 内容沿用 development-v1 probe schema。验证原始字节摘要、重复 JSON 字段、体积、非空及 probe 白名单；不跟随 manifest 中的路径。该格式仅冻结自动 probe，不能替代模型 ModelCase 冻结集、教师评分或已有重复批次 runner。

审阅人字段不构成身份签名，报告始终保留 external_signoff_required。开发报告绝不升级为真实模型、真实工具或教师验收。定向 6 项测试通过；[最终汇总](verification-stage17-19.json)记录实际全量与交接结果。

## 仍需独立交付

动态输入/时钟/复位契约、其余模式的可信分析事实、审阅参考材料、L4 持久发布事务及 GUI 签收、团队冻结 ModelCase、真实模型课程批次和双人教师评分、原生目标 OS 验收仍未完成。不通过生成代码或增加合成测试将这些条件标为完成。
