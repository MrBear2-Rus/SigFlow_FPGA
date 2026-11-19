# Part 2–3 实施计划

## 基线与范围

- SigFlow：`b7e5186`；`contracts/edu-agent/v1/` 已扩展 Agent/Gateway API。本批客户端范围限定为 health/capabilities，报告适配使用已有 Diagnostic、SourceRef、JobReportView Schema。
- UCAgent：`ac00768`，使用真实 `VerifyStage`、`Checker`、`UCTool`，不复制上游运行时。
- Python 核心保持标准库实现；上游集成依赖隔离在 `.venv`。上游只读，不修改其默认工作流。
- 所有新增源码、测试和文档位于 `sigflowAgent/`，保留现有入口及模块。

## Part 2：UCAgent 阶段适配

目录：`sigflow_edu_agent/runtime/`，测试 `tests/test_runtime.py`、`tests/integration/test_ucagent_runtime.py`。

`TeachingSession` 管理单个 TeachingLoop，提供 `advance(expected_version)`、`host_action(...)` 和只读阶段检查。锁串行化推进和动作；重复旧版本推进不能再次取证或调用模型。

真实 UCTool `EduAdvance` 只接受状态版本；不开放 hint_next、receipt、grant、文件路径或 shell 参数。学生动作只能由可信宿主调用 `host_action`。真实 Checker 读取最近一次阶段转换，不接受模型提交的通过标记，不在模板渲染中推进状态。

`build_stage(session, phase, workspace)` 构造真实 VerifyStage，使用局部 checker registry 注入同一会话；禁用 pre/post commands、技能钩子和文件输出要求。阶段 gate 与默认工程验证流程隔离；不直接启动 VerifyAgent 的默认工具表。

- [x] 验证暂停、旧版本推进、真实阶段/Checker/UCTool；新增 E5 finish 回归先复现失败再修复。
- [x] 实现会话适配和受限工具集合。
- [x] 在独立环境、固定上游源码路径运行真实类集成测试与 smoke trace。

## Part 3：Gateway 与证据上下文

目录：`sigflow_edu_agent/clients/`、`sigflow_edu_agent/context/`，测试 `tests/test_gateway.py`、`tests/test_context.py`。

GatewayClient 仅调用现有 GET health/capabilities；固定 127.0.0.1、端口校验、token 脱敏、实例/版本校验、不跟随重定向、响应字节/超时限制。未知或禁用能力不进入教育可用集合。

ReportEvidenceSource 消费指定 JobReportView 和指定 Diagnostic ID，并通过受信证据目录核对项目、revision、snapshot、Job、artifact/hash、源码行号。只读导出文件与 HTTP 路由是不同边界；不新增或猜测报告 URL。缺失引用、legacy_unverified、hypothesis、过期及混合版本进入 WaitingEvidence。

ContextBuilder 对 selection、report、rtl、wave、course 片段按优先级整块保留；预算不足输出 omission 与 reason，禁止拼接不同项目/版本。token 预算使用保守字节估算，Provider 仍执行最终请求字节限制。

- [x] 验证现有契约、错误信封、实例隔离、证据溯源和上下文预算测试。
- [x] 实现只读客户端、报告适配、上下文打包并连接主循环。
- [x] 使用真实本地 HTTP 测试服务验证传输；与仓库 golden fixtures 交叉检查。

## 交付验证

- [x] 核心与真实 UCAgent 集成测试分别记录，不能用 skip 表示已通过。
- [x] README 保留入口、目录索引和状态；专用文档记录接口、生命周期、限制和命令。
- [x] 检查 Markdown 链接、代码块、命令及对话式措辞；历史验收记录保持日期边界。
- [x] 修复 E5 完成判定并对修复逻辑进行独立定向审查；范围与结果见 [验证报告](verification-parts-2-3.md)。

## 边界

此阶段不宣称完整 VerifyAgent 教育 profile、安全注册表替换、Gateway 报告路由、真实 EDA 执行或 L4 UI 已完成。完整默认上游工具表不得直接接入教育服务。DeepSeek 模型使用方式保持不变，真实 API 调用单独验收。
