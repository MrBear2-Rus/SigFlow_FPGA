# 阶段 10：Gateway 教学会话组装与观察去重

日期：2026-10-11。对应 UA-05 本地会话组装；不替代 U1 服务鉴权、持久化和运行时验收。

## 入口与职责

`runtime.gateway_session.create_gateway_session` 组装 GatewayClient、GatewayReportSource、
TeachingLoop 和 TeachingSession，返回 GatewayTeachingSession。
使用现有 Python 虚拟环境，无新增依赖，规则模式不要求 UCAgent 或 DeepSeek 凭据。
实例构造不联网；宿主调用 advance 才按 E0–E7 状态机推进。

| 参数 | 约束 |
|---|---|
| binding | SessionBinding，实例、session、learner、项目、revision、snapshot、Job、diagnostic、issue |
| request | RunRequest，项目/revision/issue 必须与 binding 相同，仅 explain/diagnose |
| gateway | GatewayConfig，instance_id 必须与 binding 相同 |
| anchors | 可信宿主提供的 EvidenceAnchor 列表，不能从报告正文或模型输出生成 |
| mode | 默认 deepseek；rule 显式启用离线规则，未知值拒绝 |
| evidence_origin | 默认 tool，本地 HTTP fixture 必须显式 fixture |
| combinational_goal | 明确设计目标时由宿主设定，默认 false |
| context_items | 可信局部上下文，继续执行已有 byte/token 裁剪与范围检查 |
| max_steps / max_model_calls | 默认 32 / 4，沿用 TeachingLoop 预算限制 |

绑定标识采用不超过 256 字符的 ASCII opaque ID，允许字母数字、下划线、连字符及内部点/冒号。
policy 固定为 l1-l3-l4-denied。绑定不充当认证或授权凭据；U1 必须根据已认证主体路由会话对象，
不得让客户端通过自报 learner_id 获得其他会话。

## 宿主调用

```python
from sigflow_edu_agent.domain import Status
from sigflow_edu_agent.runtime.gateway_session import create_gateway_session

def advance_to_pause(binding, request, gateway_config, trusted_anchors):
    session = create_gateway_session(
        binding=binding, request=request, gateway=gateway_config,
        anchors=trusted_anchors, mode='deepseek',
    )
    while session.state.status == Status.RUNNING:
        state = session.state
        session.advance(state.version)
    return session
```

DeepSeek 模式通过 DEEPSEEK_API_KEY / DEEPSEEK_MODEL 及可选 DEEPSEEK_BASE_URL 配置，
启动时缺配置直接拒绝，不静默伪装成模型会话。E4 会发送问题与经过裁剪的证据上下文；
会话身份和 Gateway token 不由工厂添加到模型上下文。输入正文自身的数据筛选仍由宿主负责。
API 调用失败或候选不合规时沿用主循环规则降级；卡片 producer 与 state.reason 记录结果。

等待学生动作时调用 `session.host_action('hint_next', action_id, expected_version)`，
再按上述方式推进。等待态 advance 不自转，动作幂等及旧 state_version 检查复用原有逻辑。
reference_request 仍拒绝为 receipt_unavailable；工厂不开放 verify，也不注入执行端口。

## occurrence 语义

`session.occurrences` 返回不可变元组，记录首次通过完整证据检查的观察，包括 binding、
evidence_id、evidence_sha256 和 origin。不保存摘要正文、token 或问题文本。
哈希基于内部 Evidence 的规范 JSON，包括其上下文与锚点；它不是 Gateway plan_hash 或认证签名。

每个会话固定一个 Job 的一个诊断，因此最多一条观察。同一证据重复取回不会新增记录；
同绑定下证据内容变化返回 stale_report，进入 WaitingEvidence，不发布新卡。读取失败或无效证据不记入观察。
去重只约束已归一化的选中证据，不宣称比较原始报告中所有未选字段。
不同会话分别记录，跨进程重启不保留；新版本、新 Job、另一诊断应创建新绑定和会话。
该记录不代表学生犯错次数，不用于推断掌握度或自动升级提示；跨会话 occurrence 合并仍由 U1/U3 负责。

## 验证记录

新增 10 项测试：范围/策略拒绝、构造不联网、规则循环、刷新去重、同绑定内容变化拒绝、
会话隔离、缺证据不调用模型、解释完成、L4 拒绝、实际 DeepSeek 适配器组装及本地 HTTP 取证。
DeepSeek 测试替换 HTTPS 传输，无真实 API 请求；HTTP 服务返回显式 fixture，无真实 EDA 执行。

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_gateway_session.py -v
.\.venv\Scripts\python.exe -S -m unittest discover -s tests -q
```

- 含真实 UCAgent 类集成：158 项全部通过。
- 核心隔离：152 项通过，6 项可选集成跳过。
- 40 项开发离线评测全部通过，[归档结果](evaluation-stage10.json)包含本阶段源码摘要。
- 24 份 Markdown 的本地链接和代码围栏检查通过；git diff --check 无空白错误。
- 独立只读审查未发现具体缺陷，审查者独立运行新增 10 项测试全部通过。

## 再规划与交接

当前进程未提供 DeepSeek API/model 或 Gateway 实例、token、Job 环境配置，未进行真实联网验收。
后续实际调用前由宿主配置凭据并确认上下文的数据范围。

| 负责方 | 尚需输入或工作 |
|---|---|
| U1 | 已认证主体到会话的路由、真实 StageManager/profile、持久化/恢复 |
| S1/S2 | 实际 Gateway、Job 绑定、真实报告及可信锚点目录、精确波形 |
| S1/S3 | 完整 grant 摘要绑定、receipt 原子核销与崩溃恢复协议 |
| U3/教师 | 冻结场景、真实模型每场景至少三次评测、两名教师独立评分 |

UA-05 至 UA-08 保持部分完成或待联合验收，不根据本地测试自动勾选整体完成。
