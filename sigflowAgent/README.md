# sigflowAgent：U2 教学主控

U2 的教学状态机、DeepSeek 适配器、UCAgent 阶段扩展和只读证据适配层。源码、测试、依赖和文档集中在本目录，不加入 SigFlow CMake 构建。

## 交付范围

| 部分 | 已实现 | 集成边界 |
|---|---|---|
| Part 1 | 有界 E0–E7 主循环、L1–L3、DeepSeek 候选生成与规则降级 | 默认 CLI 使用 fixture；真实模型质量未验收 |
| Part 2 | 串行会话、真实 VerifyStage / Checker / UCTool 适配、受限工具集合 | 独立阶段集成；完整 VerifyAgent profile 与 StageManager 调度由服务集成层接入 |
| Part 3 | Gateway health/capabilities 客户端、报告证据核对、上下文预算 | HTTP 由本地测试服务验证；真实 Gateway 联调及报告路由尚未完成 |

离线通过不代表周期四 G0/G1 验收完成。真实 EDA 执行、持久化、学习事件、L4 receipt 和教师评测仍需后续集成。

## 开发入口

Python >=3.11。核心与 DeepSeek HTTP 客户端仅使用标准库；UCAgent 集成使用本目录 `.venv` 的可选依赖。以下命令在 `sigflowAgent` 执行，不要求 `conda activate`：

```powershell
.\.venv\Scripts\python.exe -m sigflow_edu_agent --demo
.\.venv\Scripts\python.exe -m sigflow_edu_agent --provider rule --once
$env:SIGFLOW_UCAGENT_SOURCE = (Resolve-Path '..\..\UCAgent\UCAgent').Path
.\.venv\Scripts\python.exe -m unittest discover -s tests -q
.\.venv\Scripts\python.exe -m sigflow_edu_agent.runtime.smoke
```

环境重建与版本记录见 [environment.md](docs/environment.md)。未设置 `SIGFLOW_UCAGENT_SOURCE` 时，上游集成测试会跳过；该结果不能作为 Part 2 集成通过的证据。

## DeepSeek

默认 Provider 为 DeepSeek，显式配置凭据与账户可用模型：

```powershell
$deepseekSecret = Read-Host 'DeepSeek API key' -AsSecureString
$env:DEEPSEEK_API_KEY = [System.Net.NetworkCredential]::new('', $deepseekSecret).Password
$env:DEEPSEEK_MODEL = Read-Host 'DeepSeek model ID'
.\.venv\Scripts\python.exe -m sigflow_edu_agent --provider deepseek --once
```

`.env.example` 为变量说明，程序不自动加载 `.env`。使用官方 HTTPS Chat Completions、JSON 输出、非流式，关闭 thinking。模型生成候选卡片，由确定性检查控制发布。失败最多修正一次，随后规则降级；无凭据时返回 `model_unconfigured`。

通过 `ReportEvidenceSource` 接入后，问题、规范化证据及保留的上下文片段会进入模型请求。宿主应在构造上下文前完成数据访问控制与敏感内容处理。请求上限 16 KiB，响应上限 256 KiB；片段预算不替代最终请求预算。真实 API 连通性与教学质量未在本批验证。

## 目录

```text
sigflowAgent/
  sigflow_edu_agent/
    domain.py, loop.py, ports.py, pedagogy.py
    deepseek.py, demo.py, __main__.py
    runtime/       # 会话串行化、真实 UCAgent 适配与 smoke
    clients/       # Gateway 只读客户端与 probe
    context/       # 引用模型、报告规范化、上下文预算
  tests/
    integration/   # 真实 UCAgent 类集成测试
  docs/            # 接口、环境、实施与验证记录
  pyproject.toml
  requirements-ucagent-smoke.txt
```

主循环仍由 `TeachingLoop.step()`、`run_until_pause()`、`act()` 管理。`TeachingSession` 串行化宿主动作与模型单步推进。等待态不自动推进；默认最多 32 阶段步、4 次模型调用。`expected_version` 防止旧请求重复推进；幂等记录仅在当前进程有效。

```text
E0 → E1 取证 → E2 检查 → E3 教学路径 → E4 发布
  explain: Completed
  diagnose: E5 WaitingStudent → hint_next → E1
  verify: E5 WaitingApproval → 可信授权 → E6 验证 → E7 反馈
```

## 开发文档

- [UCAgent 阶段集成](docs/runtime-integration.md)：调用边界、Checker 生命周期、工具注册与调度责任。
- [Gateway、报告与上下文](docs/gateway-context.md)：只读接口、数据绑定、失败语义与对接示例。
- [环境与依赖](docs/environment.md)：隔离环境、复现命令、版本与凭据配置。
- [Part 2–3 验证记录](docs/verification-parts-2-3.md)：实测范围及未验证项。
- [Part 2–3 实施计划](docs/parts-2-3-plan.md)。
- 第一阶段历史记录：[设计](docs/u2-first-batch-design.md)、[实施](docs/implementation-plan.md)、[验证](docs/verification.md)。

## 接口分工

U1 对接服务生命周期、可信宿主动作、StageManager 调度与持久化；S1/S2 提供可信报告、证据目录及真实 Job 执行端口；U3 对接学习事件。`VerificationPort` 默认拒绝执行，能力可用列表不代表执行授权。内部逻辑 Plan、EvidenceAnchor 和 ContextItem 不作为新增共享 HTTP 契约。
