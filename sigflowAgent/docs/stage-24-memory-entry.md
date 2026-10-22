# 阶段 24：可信启动接线与学习接口交接

日期：2026-10-19。适用目录 `sigflowAgent`，补充[阶段 23 端口说明](stage-23-teaching-memory-ports.md)。本阶段只实现 U2 组合入口，不包含 UA-10 实际实现。

## 正式入口

`sigflow_edu_agent.host.__main__.main` 接受可选关键字参数 `teaching_memory`，转发到已有 Runtime。可信 Python 启动器可以在已构造适配器后调用：

```python
from sigflow_edu_agent.host.__main__ import main

def run_host(adapter):
    return main(
        ["--bootstrap-stdin", "--gateway-evidence"],
        teaching_memory=adapter,
    )
```

启动器仍须通过标准输入交付既有 bootstrap 单行，读取既有 ready 单行，并通过认证 HTTP 使用宿主。数据库位置、会话工厂、证据核验、鉴权、模型预算、事件发布及有界关闭均复用原流程。适配器必须在可信组合层构造；不得从学生问题、HTTP 请求、bootstrap 或环境中的模块路径加载代码。

普通 `python -m sigflow_edu_agent.host --bootstrap-stdin ...` 未注入适配器，保持 `configured=false` / `adapter_required`。GUI 的默认启动流程也保持该状态，不会自动发现或启动 UA-10。本阶段不添加 CLI 开关、新 bootstrap 字段、HTTP 路由或新的第三方依赖。

对象缺少所需可调用方法时，宿主不输出 ready，返回退出码 2，只写固定错误 `u2-host: initialization_or_runtime_failed`。有效对象的 `configured=true` 仅表示已注入，并不表示接收端健康、队列已经清空或联合验收通过。

## U2 内部适配约定

| 方法 | 输入 | 结果及失败处理 |
| --- | --- | --- |
| `record_teaching_fact(fact)` | 已提交的 `edu.u2.teaching-fact.v1` 元数据 | 持久接收或去重完成后返回 `{"accepted": true, "event_id": 原 event_id}`；异常、拒绝或 ID 不符保留原队列项并重试 |
| `get_teaching_context(...)` | 关键字 `learner_id`、`project_id`、`concept_ids`、`max_bytes=1024` | 返回且仅返回 `learner_id`、`project_id`、`concept_ids`、`summary`；范围精确回显，摘要超限、错范围、异常或超时降级为空 |

这是 U2 的适配端口，不要求 U3 修改已约定接口或数据库。组合层应将 U3 的实际方法、字段与确认语义映射到此端口。事实四类为 `run_created`、`card_generated`、`student_action_accepted` 和 `run_finished`；事件 ID 重试不变，最终持久去重归接收方负责。

历史作为下一张卡片的附加输入，不替代工程证据，不提升提示级或放开工具权限；每张卡片沿用原模型调用预算。原始问题、卡片正文、历史摘要和凭据不进入教学事实。规则/fixture/tool 来源仍按原规则区分。

交付与历史调用可能并发，适配器应使用线程安全调用方式并自行限制外部调用时长。U2 不拥有外部服务或学习库生命周期，不会替适配器调用关闭方法；U2 的有界退出也不代表外部阻塞调用已被强制中断。资源释放及仍在执行的调用须由组合层按实际实现协调。

## 测试范围

新增进程测试启动正式 `main`，通过真实 loopback HTTP、实际 Runtime/工厂、SQLite 事务与队列检查以下行为：

- 有效适配器公布配置状态；无效适配器在 ready 前以脱敏错误退出。
- L1 → L2 → 完成产生 6 个唯一事实；首次错误回执导致同 ID 重试，重复动作不产生额外卡片或事实，交付标志最终持久化。
- 事实绑定原学生、工程、revision、snapshot、Job 和 session；不包含问题、历史或凭据。
- 生成前历史仅来自可信绑定，错学生历史被丢弃；两张卡片只消耗两次模型调用。
- HTTP 不能注入适配器；跨学生读取被拒绝；UI 凭据不被转发到 Gateway。
- 关闭仍只输出一行 ready，队列与确认结果保留在宿主数据库。

测试专用启动器位于 `tests/helpers/teaching_memory_host.py`，使用合成 Gateway 报告、合成接收端和模型候选，不是生产启动器或 UA-10 实现，不发起真实 DeepSeek 请求。运行方法（在 `sigflowAgent` 目录）：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_teaching_memory_process.py -v
```

本机依赖沿用 `.venv` 的 Python、标准库 HTTP/SQLite；无需安装本地 UCAgent 或启动 Conda。相关回归与源码摘要见[阶段验证记录](verification-stage24.json)。历史阶段的真实 Core/Yosys/GUI 报告保留其原范围，本阶段不重复宣称真实接收端已接通。

本阶段实测：新增进程测试 3 项、相关回归 203 项、GUI demo 回归 31 项通过；全量运行 631 项、35 条既有可选跳过记录、失败/错误 0。全量凭据测试在 Windows 当前用户身份下使用虚构 Key 完成，不调用真实 API。跳过项为既有可选 UCAgent/原始课程快照场景，不计为集成通过。

## 后续交接条件

U2 本地交付点可供组合层接入。实际启用仍需 U3/组合层确认真实适配器、持久确认与去重语义、线程调用方式、profile 删除后旧队列的处置，以及退出/重启联合验收。未签收前继续保持默认未配置，不新增 U3 表结构或代写画像、检索算法。共享协议冻结和发布仍按团队分工执行。

下一阶段在实际适配器可用时进行联合测试；当前不继续扩展其他成员模块。L4 材料、真实 detector、动态仿真和模型/教师质量验收仍按各自清单推进。
