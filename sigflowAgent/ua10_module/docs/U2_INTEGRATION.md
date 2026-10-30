# U2 与 UA-10 本地集成

## 安装与范围

UA-10 源码位于 `sigflowAgent/ua10_module/`。Python 3.11+，SQLite 支持 FTS5。
从该目录运行 `python -m pip install -r requirements-source.txt`，然后从
`sigflowAgent/` 运行 `python -m pip install .` 安装 U2。不要求第二套密钥、模型下载或 BGE。
已有本地依赖可用 `--no-index --no-build-isolation` 安装。数据目录必须由宿主指定，不能提交 Git。
U2 安装入口命名为 `sigflow_u2`，直接运行源码时对应 `sigflow_edu_agent`。

## 复用正式宿主端口

最新远程 `e2e156d` 已提供 `TeachingMemoryPort`、`TeachingMemoryBridge`、
事务性 `teaching_outbox` 与 `CardContext.learning_history`，没有旧版 `LearningHistory` DTO。
本轮复用这些端口，不新建状态机或 outbox。以下是可调用的可信宿主组合代码：

```python
from pathlib import Path
from ua10 import MemoryConfig
from sigflow_u2.host.ua10 import U2TeachingMemoryPort
from sigflow_u2.host.runtime import Runtime

memory = U2TeachingMemoryPort(
    MemoryConfig(Path('./private-learning'), default_token_budget=1024),
    instance_id=bootstrap.instance_id, concepts=('latch', 'reset'),
    evidence_origin='tool',  # 必须与实际 Runtime 的受信来源相同，fixture 则使用 fixture
)
runtime = Runtime(store, bootstrap.instance_id, factory,
                  evidence_origin='tool', teaching_memory=memory)
try:
    # store、factory、bootstrap 均来自原 U2 的可信启动组合，不能来自 LLM 或页面。
    # 继续原有 create/action/HTTP 工作流；不额外同步触发 Pattern 或模型请求。
    pass
finally:
    runtime.close()
    memory.close()
    store.close()
```

Runtime 关闭后台投递后再关闭 memory；factory 和 store 沿用原有创建方式。
本例中的宿主变量是原启动组合提供的对象，不是新配置接口。
完整离线可运行示例：`python ua10_module/examples/u2_usage.py --data-dir ./demo-memory`。

## 写入、重放与事实边界

Runtime 在同一宿主事务内提交状态和 `edu.u2.teaching-fact.v1` outbox。
Bridge 后台调用 `record_teaching_fact`，适配器校验实例、来源、概念和事件 ID，
再由 TeachingMemory.record_event 写入 V1 原始事实与 UA-10 恢复日志、刷新投影。
只有持久化成功才返回原 event_id 和 accepted=True。丢失 ACK 时用同一 ID 重放，
不生成重复事实；相同 ID 改变 payload 会拒绝。投影失败不冒充成功。

元数据只声明运行创建、卡片生成、动作接受和运行结束，不能推断诊断确认、
提示已展示、学生已修改、验证通过或掌握。action 的 previous_version 表示前一条
已收到的元数据版本；不伪造提示等级变化。原始不透明 evidence ID 仍在宿主 outbox，
V1 使用摘要引用并保留原始 u2fact event_id，可回查宿主原始记录；它不是报告内容哈希。
缺少源事件时间时以首次接收时间作为可用时间，不回填历史时点。
fixture 永远记为 mock，默认不能进入真实可信教学背景。

普通模块 API 仍为 TeachingMemory 初始化/关闭、record_event、get_teaching_context，
旧 ua10-host-fact.v1 仍兼容。管理删除、撤销、索引重建沿用核心服务，不暴露给模型。
删除墓碑阻止过期 outbox 复活数据。未知诊断/验证结果保留 unknown。

## 上下文与身份门禁

本轮新增的 scoped_history 适配器从宿主绑定接收 learner/project/instance/session/revision/as_of，
身份不是模型参数。旧四字段历史响应协议保留，旧端口不接收新参数。
当前 U2 每次只有一个已确认概念；适配器按单概念查询，读取失败不影响教学状态。
原有 256 字符身份上限与 V1 160 字符上限未静默映射：超过 V1 约束显式拒绝，
不得截断。团队仍需统一生产 ID 约束或单独审核持久化映射方案。

旧本地工作树存在 600 字符切片；本轮远程基线已换成 1024 字节门禁，没有该切片。
现在 UA-10 默认 1024、普通教学最大 1536 估算 tokens。默认无 Embedding 时按 UTF-8
字节数保守估算，不将字符数当 tokens；U2 外层最大 1536 字节。未与真实 DeepSeek
分词器校准，不能声称精确 token 数。记忆部分和整个 Prompt 分开计预算，
整个请求仍遵守 DeepSeekProvider.MAX_REQUEST_BYTES，不挤掉工程证据。
超预算只移除完整事实/派生项，DTO 保留 truncated/omitted_fact_count/uncertainty；
不截断单条事实、引用或否定词。512/1024/1536 合成测试见本轮审查报告。

## DeepSeek 与 Pattern

本轮在原 DeepSeekProvider 增加 complete_json，复用同一个 config/凭据/Base URL/timeout、
HTTP 发送、解析和错误处理；generate(CardContext) 的接口不变。没有另一套 Key 或模型选择。
factory.pattern_provider(reserve=..., authorized=False) 复用 factory 已加载的 Provider。
必须显式联网授权，且核心 MemoryConfig.enable_real_api=True 才能进行联网 Pattern 构建。
默认 synthetic_only=True；真实学生数据另需宿主授权。输出最多 min(768,宿主 max_tokens)。
reserve 在连接之前收到 input_bytes/max_output_tokens，调用方应接入宿主同一会话预算：

```python
provider = factory.pattern_provider(
    reserve=lambda **estimate: store.reserve_model(session_id, 4),
    authorized=host_has_explicit_network_permission,
)
# 只在明确计划的后台整理任务中调用核心 build_learning_patterns，不随教学卡片自动触发。
```

U2 当前只提供调用次数预留，没有真实 token/费用账本；不假设已有完整成本控制。
授权不足、预算不足、网络/解析错误都向调用方传播，无自动无限重试。
本轮全部模型传输使用 Fake；未调用真实 API，也不声称真实教学质量或模型效果提升。

## 验收与待办

本轮验证 Runtime -> outbox -> UA-10 -> 跨会话 -> CardContext -> Fake 模型请求。
原正式工作树和生产库未修改；这些改动仅在本地集成工作树，尚未发布或生产启用。
生产宿主仍需审核注入位置、合法概念范围、ID 上限、真实分词与费用账本，
再做真实 Gateway/用户流程验收。普通教学启动不自动启用 Pattern、BGE 或 LightMem。
BGE-small-zh-v1.5 仅可配置已有本地模型路径；权重、缓存和数据库不得提交。
