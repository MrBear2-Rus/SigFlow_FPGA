# sigflowAgent：U2 教学主控

U2 的自研教学状态机、DeepSeek 适配器和只读证据检查层。Python 服务、测试、依赖和文档集中在本目录；真实闭环另提供可选的 CMake 集成测试宿主。

2026-10-13 已按[自研决策](../docs/edu-agent/DECISION-2026-10-07.md)同步当前任务口径。
UCAgent 适配器与旧集成文档保留为历史实验，不属于交付依赖或当前验收条件。

课程 RAG 已接入 E3/E4，代码位于 `sigflow_edu_agent/rag/`，开发 CLI 和常驻宿主 CLI 默认读取 `data/course.sqlite`。阶段 15 已补齐常驻会话的课程状态、恢复重验和历史动作重放隔离，见[课程宿主说明](docs/stage-15-course-host.md)。当前 v4 数据库加入 lowRISC、Yosys 0.47 和 Verilator v5.052 官方原文及双语检索映射；完整来源和教师审阅状态见[外部资料实施记录](docs/external-materials-catalog-2026-10-13.md)。仅保留一个当前数据库，旧索引、manifest 跳转、回滚和旧表兼容已移除。配置、重建与验证见 [RAG 接入说明](docs/rag-integration.md)。按用户 2026-10-13 的决定，当前数据库纳入 Git 提交范围，Python 安装包不携带数据库、原始官方源文件或完整 passage 正文。

## 交付范围

当前范围以 [U2 交付核对表](docs/u2-completion-status.md)为准，历史阶段与实测记录见 [task.md](docs/task.md)。

2026-10-22 阶段 35 已适配最新目录与 deb6bd9 基线，修复可选组合测试及新检出测试目录初始化，并固定 POSIX 启动器 LF。当前基线 deb6bd9 的完整回归：U1 326 项、1 条平台跳过；U2 719 项、37 条跳过记录；两套失败/错误均为 0。U2 逐项成功 684，单项跳过 35，另有 2 条类级跳过记录。开发评测 40/40；独立离线安装/名称隔离 4/4；真实当前工程闭环 3 Job、0 次模型调用。 详细范围见[当前适配](docs/stage-35-current-adaptation.md)与[机器验证](docs/verification-stage35.json)。教学扩展仍需显式启用，尚待团队接收，不改变默认基础 v1 的发布范围。

| 工作包 | 本地实现 | 完整验收边界 |
| --- | --- | --- |
| UA-05 工作流 | E0–E7、证据/上下文/计划、受控执行、跨版本复测、事务/恢复/预算 | 正式共享服务组合与 UI 保存同步另签收 |
| UA-06 提示策略 | L1–L3、显式升级、连续问题建议、receipt 持久协议、固定审阅材料提供器和可信内部正文读取 | 普通 HTTP L4 关闭；真实材料、教师身份和正文 UI 待签收 |
| UA-07 模式与波形 | 八类要求/排除条件和不支持路径、精确波形、学生预测、当前工具链适配 | 每类生产 detector 和教师认可预期分别核验，不虚构硬件根因 |
| UA-08 评测 | 开发/对抗评测、重复模型 runner、冻结输入/预算离线预检、盲评材料、双人评分导入及整改门禁 | 实际冻结集、真实模型批次、教师评分及教学质量不能由程序代签 |
| UA-09/10 接口消费 | CourseProvider、事实队列、生成前历史、操作级连接适配器及可信启动注入 | 不修改 U3 数据结构，实际 UA-10 接收端仍待组合 |

本地闭环或离线通过不代表团队 Gate 完成。阶段 32–34 已在正式 U1 提供可选同进程教学组合与 `/api/v1/u2` 扩展；独立 U2 宿主的 u2.host.v1 保留。原生 AgentApiClient 尚未消费新教学扩展，不能把服务端联通当作 GUI 已接通。

## 开发入口

本轮维护 U2 业务及其必要 U1 接线。旧 GUI demo 目录和 main 接线未恢复；全部 stash 保留。当前开发和测试不以 GUI demo 为前提。

阶段 32–34 已补齐正式 U1 的持久外部关联、教学等待/动作/事件、取消、显式恢复及当前工程证据接线。使用 `bin/sigflow-shared-u2.cmd --bootstrap-stdin` 启用同端口组合；身份和目录仍由可信启动方通过现有 bootstrap 提供。详见[组合服务开发接入](docs/u1-u2-composition-guide.md)、[真实联合验证](docs/stage-34-formal-u1-acceptance.md)和[机器记录](docs/verification-stage32-34.json)。不替换原基础路由，不新增运行时第三方依赖。

2026-10-21 阶段 28–31 完成 U2 独立导入/安装名称、共享宿主内部适配、UA-10 操作级连接适配、固定审阅材料与可信内部正文读取及冻结评测预检。正式宿主使用 `sigflow_u2`，安装分发名 `sigflow-u2-teaching`，避免覆盖 U1 包；源码旧入口继续用于开发。该阶段组件交接见[历史交接说明](docs/u2-integration-handoff-2026-10-21.md)与[历史验证](docs/verification-stage28-31.json)。其后阶段 32–34 已注册正式 U1 教学扩展；实际 UA-10、真实材料/权限、冻结集、教师和 UI 签收仍待提供。

[连续问题建议](docs/stage-25-repeat-advice.md)与[冻结模型评测/人工评分入口](docs/stage-26-frozen-evaluation.md)说明新增 U2 能力和复现流程。

[阶段 27 动态闭环](docs/stage-27-current-baseline.md)历史记录已核验真实 Core/Yosys/Verilator 三 Job、精确波形和显式预测。当时完整回归运行 658 项，失败/错误 0，另有 35 条既有可选跳过记录；新增测试 27 项通过，开发评测 40/40。该阶段源码摘要、历史网络异常及验收边界见[阶段 25–27 验证记录](docs/verification-stage25-27.json)。

[U2 完整依赖核查](docs/u2-dependency-audit.md)区分运行链已具备与尚待交付的发布条件；[学习事实与历史端口](docs/stage-23-teaching-memory-ports.md)仅提供 U2 内部适配接口，未实现或替代 U3 数据服务；[U1 剩余交接](docs/u2-u1-handoff-2026-10-19.md)说明统一入口、状态生命周期与平台发布的责任边界。

[阶段 24 可信启动接线](docs/stage-24-memory-entry.md)支持组合层将学习适配器注入正式 bootstrap/ready 入口；普通 CLI 与 GUI 默认仍未配置。进程级契约验证不代表 UA-10 实际服务已完成联合验收。

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

环境重建与版本记录见 [environment.md](docs/environment.md)。历史上游实验及原始课程快照测试按可选依赖跳过；自研核心不依赖 UCAgent，真实服务、工具和教师验收仍须另行执行。

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

2026-10-16 阶段 16 最终 unittest 报告运行 514 项、35 条跳过记录，无失败，逐项成功 481；跳过包含 33 个方法和 2 个类（覆盖另外 18 个未运行方法），不可按 514−35 计算成功数。开发评测 40/40，源码摘要一致。真实仿真场景完成 Yosys 基线、Verilator build/run 共 3 Job，经过只读观察和显式后继确认，核验 394 字节 VCD 的归属/摘要；现有 harness 仅单次 eval/dump。真实 Yosys 加课程另通过 2 Job/4 条背景兼容回归。本阶段模型调用 0，独立审查修复复核通过，详见[阶段 16 手册](docs/stage-16-simulation.md)和[验收汇总](docs/verification-stage16.json)。

阶段 15 历史回归为 unittest 运行 503 项、35 条跳过记录，无失败，逐项成功 470。跳过原因是历史 UCAgent 实验和课程原始快照缺失。Windows DPAPI 测试使用正常当前用户上下文。该阶段完成课程状态、恢复重新检索与旧动作重放隔离；课程状态 partial、充分性 not_assessed。详见[阶段 15 手册](docs/stage-15-course-host.md)。

D12 跨 revision 场景本轮另完成 4 个真实 Job 回归；原 `agent/` 的 221 项独立回归（220 通过、1 项 Windows 信号测试跳过）属于 D12 历史记录，见 [D12 手册](docs/u1-d12-student-edit.md)。D11 历史验收另包含真实 DeepSeek 初始卡、实际模型调用 1 次，见 [D11](docs/u1-d11-closed-loop.md)。使用本目录 `.venv`，不要求 Conda 或 UCAgent。

## DeepSeek

Windows 可隐藏输入并保存当前用户加密配置。以下在仓库根目录执行：

```powershell
.\sigflowAgent\bin\sigflow-deepseek.cmd configure --model deepseek-flash
.\sigflowAgent\bin\sigflow-deepseek.cmd status
.\sigflowAgent\bin\sigflow-deepseek.cmd test
```

配置存放于 Git 忽略的 `sigflowAgent/.local/deepseek.credentials.json`，不接受命令参数 Key。test 只执行一次真实 API 请求，发送内置教学示例；失败不会以规则回退冒充成功。当前用户的加密配置已用于真实闭环验收，详见[密钥与测试说明](docs/deepseek-local-config.md)。

也可继续使用环境配置，以下在 sigflowAgent 目录执行：

```powershell
$deepseekSecret = Read-Host 'DeepSeek API key' -AsSecureString
$env:DEEPSEEK_API_KEY = [System.Net.NetworkCredential]::new('', $deepseekSecret).Password
$env:DEEPSEEK_MODEL = Read-Host 'DeepSeek model ID'
.\.venv\Scripts\python.exe -m sigflow_edu_agent --provider deepseek --once
```

`.env.example` 为变量说明，程序不自动加载 `.env`。存在任一 DeepSeek 环境配置变量时整套使用环境，否则读取加密配置，不混合来源。使用官方 HTTPS Chat Completions、JSON 输出、非流式，关闭 thinking。主循环失败最多修正一次，随后规则降级；独立 test 命令不重试或降级。无凭据时返回 `model_unconfigured`。

通过 `ReportEvidenceSource` 接入后，问题、规范化证据及保留的上下文片段会进入模型请求。宿主应在构造上下文前完成数据访问控制与敏感内容处理。请求上限 16 KiB，响应上限 256 KiB；片段预算不替代最终请求预算。真实 API 连通性已验证，教学质量仍待独立教师验收。

## 目录

```text
sigflowAgent/
  sigflow_edu_agent/
    domain.py, loop.py, ports.py, pedagogy.py
    deepseek.py, demo.py, __main__.py
    runtime/       # 自研会话/恢复、历史 UCAgent 实验
    host/          # SQLite、异步宿主、HTTP、Gateway 执行、probe
    clients/       # Gateway 只读客户端与 probe
    context/       # 引用模型、报告规范化、上下文预算
    courses.py     # 课程接口转换与材料预算
    rag/           # 检索、引用核对、单文件构建与整理资料
    checkers/      # 详细计划与前后证据判断
    contracts/     # PlanCard 审批材料映射，不签发授权
    patterns/      # 版本化教学策略与支持边界
    evaluation/    # 离线开发评测入口
  sigflow_u2/      # 正式 U2 独立导入桥接；wheel 将实现直接安装到该名称
  tests/
    integration/   # 真实 UCAgent 类集成测试
  integration/     # 可选 headless Core/Gateway 真实闭环宿主
  docs/            # 接口、环境、实施与验证记录
  pyproject.toml
  requirements-ucagent-smoke.txt
```

主循环仍由 `TeachingLoop.step()`、`run_until_pause()`、`act()` 管理。`TeachingSession` 串行化宿主动作与模型单步推进。等待态不自动推进；默认最多 32 阶段步、4 次模型调用。`expected_version` 防止旧请求重复推进；直接 Session 的动作记录只在进程内有效，新 `host` 使用 SQLite 保存跨进程动作预留/提交及模型预算。

```text
E0 → E1 取证 → E2 检查 → E3 教学路径 → E4 发布
  explain: Completed
  diagnose: E5 WaitingStudent → hint_next → E1
  verify: E5 WaitingApproval → 可信授权 → E6 验证 → E7 反馈
```

## 开发文档

- [本地任务清单](docs/task.md)：阶段交付、再规划和联合验收缺口。
- [阶段 21 宿主参考元数据](docs/stage-21-reference-host.md)、[实施计划](docs/u2-stage21-reference-host-plan.md)及[验收汇总](docs/verification-stage21.json)：内部动作原子提交、同 receipt 显式恢复、取消并发修复和真实 Core 宿主探针；公开 HTTP L4 保持关闭。
- [阶段 20 持久参考协议](docs/stage-20-durable-reference.md)、[实施计划](docs/u2-stage20-durable-reference-plan.md)及[验收汇总](docs/verification-stage20.json)：同 receipt 恢复、日志事务、Core 故障探针及生产 L4 关闭边界。
- [剩余开发计划](docs/u2-remaining-development-plan.md)、[阶段 17 可信波形](docs/stage-17-trusted-waveform.md)、[阶段 18 显式预测](docs/stage-18-prediction.md)、[阶段 19 协议与评测交接](docs/stage-19-handoff.md)、[联合验收汇总](docs/verification-stage17-19.json)：现有依赖下的静态波形预测闭环、receipt 补强和外部签收边界。
- [阶段 15 计划](docs/u2-stage15-course-host-plan.md)、[课程宿主手册](docs/stage-15-course-host.md)与[验收汇总](docs/verification-stage15.json)：生产课程配置、状态契约、恢复与真实工具联合测试。
- [阶段 16 计划](docs/u2-stage16-simulation-plan.md)、[真实仿真执行链](docs/stage-16-simulation.md)与[验收汇总](docs/verification-stage16.json)：可信 build/run、VCD 产物核验及现有单次求值边界。
- [U1 依赖设计](docs/u1-dependencies-design.md)与[阶段计划](docs/u1-dependencies-plan.md)。
- [D1 存储](docs/u1-d1-store.md)、[D2 运行时](docs/u1-d2-runtime.md)、[D3 HTTP](docs/u1-d3-http.md)、[D4 Gateway](docs/u1-d4-verification.md)、[D5 联调手册](docs/u1-d5-integration.md)。
- [D6 动态可信目录](docs/u1-d6-catalog.md)、[D7 验证只读续查](docs/u1-d7-observation.md)、[D8 兼容与验证](docs/u1-d8-compatibility.md)、[D6–D8 计划](docs/u2-coupled-runtime-plan.md)。
- [D9 验证恢复](docs/u1-d9-verification-recovery.md)、[D10 Gateway 可信取证](docs/u1-d10-gateway-evidence.md)、[D11 真实闭环](docs/u1-d11-closed-loop.md)、[闭环实施计划](docs/u1-closed-loop-plan.md)。
- [D12 跨 revision 保存与重新审批](docs/u1-d12-student-edit.md)、[D12 实施计划](docs/u1-d12-revision-plan.md)。
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

U2 所需的生命周期、可信动作、事务与验证既可独立运行，也可通过正式 U1 的 `--u2-teaching` 组合；S1/S2 提供真实工具、报告与执行 provenance，U3 对接学习事件及保留策略。执行仍要求显式 UI 审批和 scoped Gateway grant，能力列表不构成授权。U1 教学扩展使用 `u1.u2-teaching.v1`，基础共享协议保持；内部 EvidenceAnchor 和 ContextItem 不接受普通 HTTP 注入。GUI、真实数据与教师签收独立推进。
