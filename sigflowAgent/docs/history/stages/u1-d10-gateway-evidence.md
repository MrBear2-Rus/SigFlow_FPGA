# U1 D10：Gateway 可信证据与空诊断任务观察

日期：2026-10-15。范围：显式 Gateway 取证与中性任务报告教学。

## 启用方式

启动 Python sidecar 时显式传入 `--gateway-evidence`：

```powershell
$env:PYTHONPATH = 'sigflowAgent'
& sigflowAgent/.venv/Scripts/python.exe -m sigflow_edu_agent.host --bootstrap-stdin --gateway-evidence
```

bootstrap 仍使用现有单行 stdin 协议；Gateway Agent token、UI token 与模型配置保持原有身份边界。不得把 UI 控制身份交给 Gateway 证据客户端。

该模式与 `--anchors`、`SIGFLOW_U2_ANCHORS` 环境目录互斥，并要求 `--evidence-origin tool`。未指定新参数时，默认行为仍是外部可信 AnchorCatalog 或空目录。外部目录继续支持既有 evidence ID、源码引用、诊断选择和实时重读规则。

新模式的会话可以使用 `diagnostic_id=job-report`。这是完整报告的任务观察选择器，并非报告中伪造的 diagnostic。会话仍必须固定 instance/project/revision/snapshot/job，不能传入任意 URL、文件路径、shell 命令或产物内容读取路径。

## 认证与绑定

`GatewayEvidenceCatalog` 每次收集都重新请求以下固定路由，不缓存成功或失效结果：

1. 同实例 `/api/v1/health` 与认证的 `/api/v1/jobs/{job}/report`。
2. 报告必须来自 Core、完整、具有非空 input_fingerprint，并精确匹配 project/revision/snapshot/job。
3. 能力/provider 固定为 `eda.synth` / `eda-synth-yosys`、`eda.sim.build` / `eda-sim-verilator`、`eda.sim.run` / `eda-sim-verilator-run`。构建插件不能作为运行报告的 provider。
4. 对报告中的每个 artifact，认证请求 `/api/v1/artifacts/{artifact_id}`，校验 artifact/project/revision/job/hash 以及 schema。Core 的 Describe 会重新计算文件 hash；文件变化或消失时拒绝。

metadata 路由没有 snapshot_id。snapshot 绑定来自精确匹配的完整报告与 input_fingerprint；不能凭 metadata 自行推导 snapshot。产物 ID 必须是有界 opaque identifier，hash 为 64 位 SHA-256（兼容既有 sha256 前缀）。metadata 还校验字符串类型的十进制 uint64 size、schema/role/media_type 字符串和 inline_readable 布尔值；不读取产物内容。

Core metadata 的 role 可能使用插件原始产物 ID（如 `netlist`），报告 role 则是插件角色（如 `primary`），因此不要求二者相等；所有权由精确范围、artifact ID 与 hash 建立。不会从诊断自由文本推断 role、源码行或文件归属。

单次收集最多 16 个产物，超限、空列表、重复 ID、无 hash 或含未知产物字段时，在任何 metadata 请求前拒绝。最多执行 1 个报告请求和 16 个 metadata 请求，每个请求均带前置 health；总计最多 34 次 GET，继续使用 GatewayConfig 的有界超时和单响应字节限制。没有分页、重试或动态 URL。

## 任务观察和诊断

`job-report` 要求 `Succeeded`、整数 `exit_code=0`，以及至少一个已核验产物。`diagnostics=[]` 是合法成功报告，不代表报告缺失。Evidence 的 concept 为 `job_observation`、origin/confidence 为 tool；摘要只说明工具任务状态、固定范围和已核验产物数量。

观察 ID 为完整结构化报告规范 JSON 的 SHA-256，锚点携带精确 project/revision/snapshot/job 和产物 ID/hash，source_ref 为空。同一报告稳定重现相同 ID，范围、产物或报告变化产生不同 ID。产物失效后不返回旧锚点。

显式选择普通 diagnostic 时，只接受该诊断已经携带的结构化 evidence_refs，且引用必须精确指向已核验 artifact ID。不会填补缺失 evidence_refs 或通过摘要关键词生成诊断。外部别名 evidence ID 的已有目录模式仍保留原逻辑。

结果 resolver 只在同一 GatewayEvidenceCatalog 模式下使用 `job-report` 观察；外部 AnchorCatalog 结果仍要求唯一已绑定诊断。无诊断的成功结果不会被伪装成 latch 或任何硬件错误。

## 教学和结果判定边界

独立的中性报告教学评估支持完整、有效、带锚点的 tool job_observation 在 L1–L3 进入 Provider。它位于八类硬件错误 PATTERNS 之外，不注册第九个 detector，不改变未知概念和现有硬件模式的门禁。缺锚点、过期、无效、不完整、fixture 来源等仍不能调用模型；下一张提示前重新取证，产物过期会进入 WaitingEvidence。

规则卡和 DeepSeek 系统上下文明确：综合或其他工具任务成功不证明功能正确，诊断消失也不证明修复。模型生成仍受既有结构/引用/等级检查，教学语义质量需独立评审。

后继 Job 完成且有新证据，也只支持有界任务观察。没有可信 expected_resolver 时，反馈继续使用 outcome_indeterminate；不得从 exit_code=0 或诊断消失推断设计功能正确。本阶段不修改审批、grant、验证账本、恢复、取消或自动执行策略。

## 验证

```powershell
$env:PYTHONPATH = 'sigflowAgent'
& sigflowAgent/.venv/Scripts/python.exe -m unittest discover -s sigflowAgent/tests -p test_gateway_evidence.py
```

测试使用受控传输或测试自建 loopback Gateway 与 sidecar 子进程，并隔离本机模型配置；不会联系外部模型服务。覆盖认证与固定路由、错误范围/hash/metadata 类型、Core role 差异、过期与无缓存、产物预算、未完成/无产物/空诊断/正常诊断、中性 L1–L3 实际 Provider 调用、坏证据与未知概念抑制、显式入口和外部模式兼容、结果不确定性。

本阶段新增用例 15 项通过，相关定向回归合计 108 项通过；需求符合性与代码质量独立审查通过。

该测试不能替代真实 Core/Yosys/DeepSeek 验收；真实闭环入口由 U1 Task 3 提供。
