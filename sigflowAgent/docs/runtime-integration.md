# Part 2：UCAgent 阶段扩展

## 模块与责任

`runtime/session.py` 保持标准库依赖，管理一个 `TeachingLoop`。`runtime/ucagent_adapter.py` 导入真实上游 `VerifyStage`、`Checker` 和 `UCTool`，由 `build_stage` 注入局部 Checker 工厂。上游源码基线为 `ac00768`，不复制或替换上游类。

| 接口 | 调用方 | 行为 |
|---|---|---|
| `session.state` | 宿主、Checker | 获取状态快照 |
| `advance(expected_version)` | edu_advance 工具或受信调度器 | 核对整数版本，推进一步 |
| `host_action(kind, action_id, expected_version, grant_ref=...)` | 可信宿主 | 提交学生动作、取消或已验真的授权引用 |
| `check_phase(phase)` | Checker | 只读检查最近一次有效转换 |
| `build_tools(session)` | 教育服务启动层 | 构建唯一 `edu_advance` 工具 |
| `build_stage(session, phase, workspace)` | 阶段调度层 | 构建指定 E0–E7 的真实 VerifyStage |

会话锁覆盖完整单步与宿主动作。多个调用使用同一版本时仅一个推进成功，其余返回 `stale_state`。等待态调用 `advance` 不改变版本或重复调用模型。锁仅提供进程内串行化；模型请求阻塞期间宿主动作也需等待，不提供请求中断。

## 阶段判定

Checker 不接受模型提供的 `passed`、证据或授权作为通过依据。它要求会话最近一次转换来自指定阶段，且转换版本仍为当前版本。旧阶段的成功不能为后续阶段放行；失败、取消和预算耗尽不能放行。

E4 可以在发布卡片后进入 WaitingStudent / WaitingApproval 并通过发布阶段。E5 在宿主动作恢复 Running 或学生显式 finish 进入 Completed 后通过。E6 仅在全部验证步骤结束、进入 E7 后通过。E7 可在产生有限结论并 Completed 后通过。`get_template_data()` 仅返回阶段、状态、版本，不推进循环。

`build_stage` 使用空 pre/post commands、空文件要求、禁用技能强制使用。`on_init()` 仍执行真实上游生命周期，可能管理 workspace 内的上游临时状态；smoke 使用临时目录隔离。Checker 本身不写文件。

## 工具边界

`edu_advance` 仅接受 `expected_version: StrictInt`，禁止额外字段。模型无法通过工具传递学生动作、grant、文件路径或 shell 命令。参数校验失败转换为稳定 `invalid_tool_arguments`，避免上游 UCTool 的校验日志回显无效参数。宿主仍应限制外部 LangChain tracing/callbacks 对输入的记录。

`validate_tools` 要求工具集合恰好包含一个具体 `EduAdvance` 实例及固定名称。该检查仅验证传入集合，不会自动清空默认 VerifyAgent 的工具表。教育服务必须显式使用此集合，不能将其追加到含 shell/文件写入工具的默认注册表后声称安全隔离完成。

## 宿主调度

```python
from sigflow_edu_agent.runtime.session import TeachingSession
from sigflow_edu_agent.runtime.ucagent_adapter import build_stage, build_tools

session = TeachingSession(loop)  # loop 由宿主绑定工程、revision、端口和 Provider
tool, = build_tools(session)
stage = build_stage(session, "E0", workspace)
stage.on_init()
tool.invoke({"expected_version": session.state.version})
passed, diagnostic = stage.do_check(is_complete=True)
```

此代码为单阶段嵌入示例，`loop` 和 `workspace` 由宿主提供。完整教育调度器应根据状态选择下一阶段：hint_next 从 E5 返回 E1，explain 在 E4 结束；不能将 E0–E7 机械排列成一次性线性流程。多步骤验证可以多次执行 E6，再调用完成检查。默认 CLI 的 `run_until_pause` 不应与同一 session 的工具推进并行运行，避免绕过串行边界。

U1 后续集成项包括完整 StageManager/profile、外部调度预算、持久化及恢复、跨进程幂等、UI 事件绑定、默认工具注册表替换。适配层不提供这些服务能力。

## 验证入口

按 [环境文档](environment.md) 配置 `.venv` 和 `SIGFLOW_UCAGENT_SOURCE` 后运行：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests/integration -v
.\.venv\Scripts\python.exe -m sigflow_edu_agent.runtime.smoke
```

smoke 输出真实上游导入路径和 E0–E4 转换，使用规则 Provider 与 fixture 证据。集成测试另覆盖 E5 授权暂停、E6 fixture 验证和 E7 结束；没有执行真实 EDA 或真实 DeepSeek 请求。
