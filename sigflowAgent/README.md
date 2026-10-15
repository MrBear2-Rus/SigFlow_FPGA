# sigflowAgent：U2 教学主控

U2 的自研教学状态机、DeepSeek 适配器和只读证据检查层。源码、测试、依赖和文档集中在本目录，不加入 SigFlow CMake 构建。

2026-10-13 已按[自研决策](../docs/edu-agent/DECISION-2026-10-07.md)同步当前任务口径。
UCAgent 适配器与旧集成文档保留为历史实验，不属于交付依赖或当前验收条件。

课程 RAG 已接入 E3/E4，代码位于 `sigflow_edu_agent/rag/`，开发 CLI 默认读取 `data/course.sqlite`。当前 v4 数据库加入 lowRISC、Yosys 0.47 和 Verilator v5.052 官方原文及双语检索映射；完整来源和教师审阅状态见[外部资料实施记录](docs/external-materials-catalog-2026-10-13.md)。仅保留一个当前数据库，旧索引、manifest 跳转、回滚和旧表兼容已移除。配置、重建与验证见 [RAG 接入说明](docs/rag-integration.md)。按用户 2026-10-13 的决定，当前数据库纳入 Git 提交范围，Python 安装包不携带数据库、原始官方源文件或完整 passage 正文。

## 交付范围

| 部分 | 已实现 | 集成边界 |
|---|---|---|
| Part 1 | 有界 E0–E7 主循环、L1–L3、DeepSeek 候选生成与规则降级 | 默认 CLI 使用 fixture；真实模型质量未验收 |
| Part 2 | 自研串行会话、受信动作与阶段检查；保留旧上游适配实验 | 当前交付依赖自研会话；旧 UCAgent 集成不计入产品验收 |
| Part 3 | Gateway health/capabilities 客户端、报告证据核对、上下文预算 | HTTP 由本地测试服务验证；真实 Gateway 联调及报告路由尚未完成 |
| 阶段 4 | 详细计划、依赖/参数检查、跨 revision 复测比较与多步反馈 | 本地计划不等于 Gateway grant，真实执行适配未接入 |
| 阶段 5 | 八类教学模式注册表、合成正反例、模型调用前的证据门禁 | 仅既有锁存器报告映射可用，其余真实 detector 与精确波形待对接 |
| 阶段 6 | 40 个离线开发评测、脱敏报告、教师评审表 | 冻结验收集、真实模型重复评测与教师评分未完成 |
| 阶段 7 | 详细计划→共享 PlanCard、完整审批绑定材料、本地规范化摘要 | 摘要不是 Gateway plan_hash；UI 对扩展字段的展示待确认 |
| 阶段 8 | 只读 Job 报告 HTTP、artifact 规范化、可信锚点→主循环 | 本地 HTTP 测试通过，真实 Gateway 与证据目录待联调 |
| 阶段 11 | 精确波形窗口检查、四态采样和显式 fixture 预测对比 | 宿主末信号截断可能误标 exact，tool 路径暂拒绝；待 S2 修复联调 |
| 阶段 12–13 | 重复模型评测、调用预算、输出波动、双人教师评分汇总 | 模型批次与真实教师审阅待运行；程序测试不计作质量验收 |
| 阶段 14 | 只读暂停检查点、显式重新取证恢复、动作幂等和等级/预算保持 | 纯格式接口，待 U1 接入事务存储；真实执行与 L4 不恢复 |

离线通过不代表周期四 G0/G1 验收完成。真实 EDA 执行、事务持久化、学习事件、L4 receipt 和教师评测仍需后续集成。

## 开发入口

### 拉取后直接运行 RAG v0.1

仓库携带 `data/course.sqlite`，无需下载课程原文、重建索引或安装第三方依赖。安装 Python 3.11+（SQLite 需支持 FTS5 trigram），从仓库根目录执行：

```powershell
cd sigflowAgent
python -m sigflow_edu_agent --provider rule --once
python -m examples.rag_v01_u2
```

第一条运行离线教学流程，第二条展示实际数据库检索、装配及缺口状态；均不需要模型密钥或真实 EDA。需要安装命令入口时执行 `python -m pip install -e .`；数据库默认定位到源码目录下的 `data/course.sqlite`，从其他目录启动也可找到。Python wheel 单独分发仍需通过 `SIGFLOW_COURSE_DB` 指定外部数据库。

当前为 **READY_WITH_KNOWN_LIMITATIONS**：554 个 Parent、762 个 Child。旧题完整证据覆盖 43/67，新题 9/20，充分性未验证；完整教学质量尚未验收。版本指纹、验证结果及来源使用边界见 [v0.1 交付记录](docs/rag-v0.1-release.md)，接口见 [U2 联调说明](docs/rag-v0.1-u2-integration.md)。

Python >=3.11。核心与 DeepSeek HTTP 客户端仅使用标准库。以下命令在 `sigflowAgent` 执行，不要求 `conda activate`：

```powershell
.\.venv\Scripts\python.exe -m sigflow_edu_agent --demo
.\.venv\Scripts\python.exe -m sigflow_edu_agent --provider rule --once
.\.venv\Scripts\python.exe -m unittest discover -s tests -q
```

环境重建与版本记录见 [environment.md](docs/environment.md)。6 项历史上游实验测试跳过，不影响自研核心验收；真实服务与工具验收仍须另行执行。

独立核心开发不需要 UCAgent。清除可选变量后运行测试：

```powershell
Remove-Item Env:SIGFLOW_UCAGENT_SOURCE -ErrorAction SilentlyContinue
.\.venv\Scripts\python.exe -m unittest discover -s tests -q
.\.venv\Scripts\python.exe -m sigflow_edu_agent.evaluation --suite tests/evaluation/development.json
```

完整 RAG 构建回归另需安装 `python -m pip install -e ".[course-build]"` 并准备登记的原始快照；预构建数据库的日常运行不需要这些数据。完整测试不使用 `-S`，以便加载可选的 PyYAML 构建依赖；运行时仅标准库的边界由离线示例单独检查。

2026-10-14 合并 RAG 前的核心隔离测试共 219 项：213 项通过，6 项历史上游实验跳过。
开发评测 40 项通过。检查点接口与恢复边界见 [阶段 14](docs/stage-14-checkpoint.md)，
报告证据接口见 [阶段 8](docs/stage-8-gateway-report.md)。

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
    courses.py     # 课程接口转换与材料预算
    rag/           # 检索、引用核对、单文件构建与整理资料
    checkers/      # 详细计划与前后证据判断
    contracts/     # PlanCard 审批材料映射，不签发授权
    patterns/      # 版本化教学策略与支持边界
    evaluation/    # 离线开发评测入口
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

- [本地任务清单](docs/task.md)：阶段交付、再规划和联合验收缺口。
- [阶段 4](docs/stage-4-verification.md)、[阶段 5](docs/stage-5-pedagogy.md)、[阶段 6](docs/stage-6-evaluation.md)：接口与实测记录。
- [阶段 7 审批材料](docs/stage-7-plan-contract.md)、[阶段 8 报告对接](docs/stage-8-gateway-report.md)。
- [阶段 9 只读联调入口](docs/stage-9-probe.md)：现有 Job 报告查询命令、环境配置及退出码；最新全量 148 项测试通过。
- [阶段 10 宿主会话组装](docs/stage-10-session.md)：Gateway→证据→规则/DeepSeek→教学会话，观察去重；最新全量 158 项测试通过。
- [阶段 11 精确波形检查](docs/stage-11-waveform.md)：共享 DTO 检查、四态采样、预测对比及自研基线迁移。
- [阶段 12 重复模型评测](docs/stage-12-model-evaluation.md)、[阶段 13 教师汇总](docs/stage-13-teacher-summary.md)：冻结批次与评分绑定，最新 191 项核心测试通过。
- [提示策略](docs/policy-v1.md)、[教师评审](docs/teacher-review.md)。
- [UCAgent 阶段集成](docs/runtime-integration.md)：调用边界、Checker 生命周期、工具注册与调度责任。
- [Gateway、报告与上下文](docs/gateway-context.md)：只读接口、数据绑定、失败语义与对接示例。
- [环境与依赖](docs/environment.md)：隔离环境、复现命令、版本与凭据配置。
- [Part 2–3 验证记录](docs/verification-parts-2-3.md)：实测范围及未验证项。
- [Part 2–3 实施计划](docs/parts-2-3-plan.md)。
- 第一阶段历史记录：[设计](docs/u2-first-batch-design.md)、[实施](docs/implementation-plan.md)、[验证](docs/verification.md)。

## 接口分工

U1 对接服务生命周期、可信宿主动作、StageManager 调度与持久化；S1/S2 提供可信报告、证据目录及真实 Job 执行端口；U3 对接学习事件。`VerificationPort` 默认拒绝执行，能力可用列表不代表执行授权。内部逻辑 Plan、EvidenceAnchor 和 ContextItem 不作为新增共享 HTTP 契约。
