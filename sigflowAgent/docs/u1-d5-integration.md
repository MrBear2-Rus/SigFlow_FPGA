# D5：宿主兼容与联调手册

日期：2026-10-15。本文记录 D1–D5 交付，动态证据、长 Job 续查及最新全量结果见 [D8 兼容记录](u1-d8-compatibility.md)。实现与文档保留在 `sigflowAgent`，旧 `agent/` 服务保留。

## 环境与导入隔离

Python >=3.11；当前验证使用本目录 `.venv` 的 Python 3.13.5。HTTP、SQLite、DeepSeek 客户端仅使用标准库，无新增运行依赖；指定解释器即可运行，不要求激活 Conda，也不要求安装 UCAgent。两个目录都提供 `sigflow_edu_agent` 包，不应同时安装到同一环境或混用 PYTHONPATH。

Windows 使用 `bin/sigflow-u2-host.cmd`，POSIX 使用同目录无扩展名启动器。启动器只为当前进程设置自身包路径，并使用 `-P` 防止工作目录中同名包抢先导入。Windows 启动器已通过真实子进程测试，POSIX 尚待 Linux 实测。

## 接口兼容矩阵

| 边界 | 当前结果 | 尚需工作 |
| --- | --- | --- |
| C++ AgentServiceController → sidecar | bootstrap 八字段、nonce、ready、health、认证 shutdown 形状一致；Windows `.cmd` 独立启动通过 | 本批未重新运行 C++ GUI，进程测试不等同桌面端验收 |
| TeachingSession → Runtime → SQLite → HTTP | L1–L3、版本、动作幂等、预算、只读重启恢复、取消已接通 | 达到 32 会话/每会话 256 动作上限后拒绝新增；尚无归档/保留策略 |
| UI → 教学接口 | `/api/v1/teaching/` 显式扩展 `u2.host.v1` 可运行 | 当前源码无该扩展的教学面板调用方，需 UI 适配和双方签收 |
| 旧 `agent/` → 新宿主 | 通过独立进程/包路径共存 | 旧 sessions/runs DTO 不映射为本扩展；不得直接替换业务 URL |
| VerificationPort → Gateway | 核对当前 C++ grant/Job/report 字段；fixture HTTP 验证 build/run、停止及不确定提交 | 真实 UI grant、工具安装和 Core provenance 仍待联合实测 |
| Report → 教学卡 | 必须匹配宿主锚点、报告范围和完整性；D6 支持可信目录动态更新 | 新 Job 需要可信生产方供应锚点；不能从成功退出或报告文字自动生成证据 |
| DeepSeek | 既有 Provider 复用、调用前持久预算、规则降级 | 真实 Key/账户模型配置、连通性及教学质量待独立验证 |

旧服务独立测试首次发现两处测试与现有源码不一致：fixture 清理调用不存在的 `state.learning`；能力说明测试依赖英文子串。修复仅涉及 `agent/tests/test_http_api.py`，保留服务实现与“不执行 EDA”的结构化断言。

## 接入桌面进程管理

以下示例从仓库根目录执行。可信锚点文件格式见 [D3](u1-d3-http.md)，字段必须替换为真实报告绑定；空目录只能验证启动，教学将进入 WaitingEvidence。

```powershell
$u2Root = (Resolve-Path .\sigflowAgent).Path
$env:SIGFLOW_EDU_AGENT_EXECUTABLE = Join-Path $u2Root 'bin\sigflow-u2-host.cmd'
$env:SIGFLOW_EDU_AGENT_PYTHON = Join-Path $u2Root '.venv\Scripts\python.exe'
$env:SIGFLOW_U2_ANCHORS = Join-Path $u2Root '.local\trusted-anchors.json'
```

在同一环境启动现有 SigFlow 主程序后，C++ 控制器提供 bootstrap 和瞬时 UI token。教学客户端仍需从可信控制器取得端口、实例、UI token，并使用扩展路由；不得把 Gateway token 作为 UI token 使用。默认 capabilities 中 `execution.can_run_eda=false` 不表示可跳过审批。

## 真实只读联调

首先使用现有 [Gateway 只读探针](stage-9-probe.md) 验证 health/capabilities 和已有 Job 的绑定报告。退出 0 仅表示读取成功，不证明证据可信。

随后准备本地请求文件（例如 `.local/teaching-request.json`）。以下是不含凭据的字段模板，示例 ID 不代表真实工程：

```json
{"learner_id":"learner-1","project_id":"project-actual","revision":"revision-actual","snapshot_id":"snapshot-actual","job_id":"job-actual","diagnostic_id":"diagnostic-actual","issue_id":"issue-1","question":"解释当前报告中的存储行为","intent":"diagnose","mode":"rule"}
```

在 `sigflowAgent` 目录执行探针。端口、实例和 UI token 必须由可信启动器或联调 harness 提供；token 只通过环境传入，不出现在命令参数中。该入口只允许 rule 模式与 diagnose/explain，创建只读教学会话但不创建 EDA Job、不消耗模型额度。

```powershell
$u2UiSecret = Read-Host 'Host UI token' -AsSecureString
$env:SIGFLOW_U2_UI_TOKEN = [System.Net.NetworkCredential]::new('', $u2UiSecret).Password
$u2Port = Read-Host 'Host ready port'
$u2Instance = Read-Host 'Expected instance ID'
.\.venv\Scripts\python.exe -m sigflow_edu_agent.host.probe --port $u2Port --instance-id $u2Instance --request .local/teaching-request.json --key readonly-check-1 --wait 10
$LASTEXITCODE
```

探针先无认证核对 health 的实例/协议/ready，再发送 UI token，固定连接 127.0.0.1，不跟随重定向。创建后轮询已提交状态并核对 session/learner/project/revision/snapshot/Job/issue；只输出 status、state_version、card_count、evidence_origin。相同键仅用于同一请求；修改请求需新键，同项目已有非终态会话需先完成或取消。

| 退出码 | 含义 |
| --- | --- |
| 0 | WaitingStudent 或 Completed；仅表示只读教学路径完成，仍应检查来源与可信证据 |
| 2 | 配置、实例、HTTP、鉴权或响应校验失败；不回显原始异常/凭据 |
| 3 | WaitingEvidence；锚点、诊断或报告不足 |
| 4 | RecoveryRequired、Failed、BudgetExceeded 或 Cancelled；不能视为成功 |
| 5 | 有界状态轮询超时；不自动取消或重复提交 |

执行验证测试须另建 intent=verify 会话，提交严格 DetailedPlan 和独立 UI plan_hash，等待 WaitingApproval，再用当前 state_version/action_id/grant_ref 显式 execute_plan。等待超时或结果证据未就绪时，可按 [D7](u1-d7-observation.md) 通过 observe_verification 显式只读续查；不重交 Job，也不自动提交后继步骤。该验证路径不包含在只读教学探针中。

## 开发验证

从仓库根目录运行两个独立测试进程：

```powershell
Push-Location sigflowAgent
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
Pop-Location
.\sigflowAgent\.venv\Scripts\python.exe -S .\agent\tests\run_all.py
```

最终实测：核心与新宿主全量 328 项，322 通过、6 项历史可选实验跳过；其中 host/store 20、runtime 20、HTTP/工厂/探针 16、Gateway 验证 49、启动进程 4，共 109 项通过。旧 `agent/` 独立回归 124 项全部通过；离线开发评测 40 项通过。文档检查覆盖 9 个文件、59 个本地链接、2 个 JSON 示例和 8 个 PowerShell 语法块，均通过。独立审查复现的取消竞态与审批污染问题均已修复并复审，无未解决的可操作发现。

全部 Gateway、Evidence、模型候选均为明确 fixture；HTTP/进程测试运行真实本地套接字与 SQLite，不调用真实 EDA/DeepSeek。旧 UCAgent 实验测试的 skip 不算集成通过。

## 再规划与外部验收

本地 U2 运行依赖入口已具备；UA-01～04 的全部团队职责不因此全部完成。下一阶段按以下顺序执行：

1. UI 对接 teaching 扩展：版本字符串、202 轮询、409 冲突、事件游标、独立取消和恢复显式动作。
2. S1/S2 提供真实 Gateway 实例、报告、可信 EvidenceAnchor 目录；运行 Gateway 与宿主两层只读探针，记录来源及退出码。
3. 联调真实 UI 审批、受限 synth/build/run、取消竞态与未知回执的 Core provenance；未知状态不自动重交。
4. D6 已提供动态目录消费接口；仍需由可信生产方为新 Job 供应真实锚点，并提供可证实的预期观察比较器；进程成功不代替功能判定。
5. 独立验收 DeepSeek 账户模型、Linux 启动/文件锁、真实波形与 L4 receipt；未具备条件时继续拒绝相关操作。

本批未提交或推送，未清理工作区外文件。
