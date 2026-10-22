# U2 可信组合与交付说明

本文保留阶段 28–31 的组件交接快照。阶段 32–34 已完成必要 U1 正式组合，当前路由、启动、持久关联与真实验证以[组合服务接入](u1-u2-composition-guide.md)和[阶段 34](stage-34-formal-u1-acceptance.md)为准；下文当时的 U1 待办不再作为当前未实现项。

日期：2026-10-21。工作基线 04648a9。实现集中在 sigflowAgent；demo 不属于本轮。运行时 Python >=3.11、标准库及 SQLite，不要求 UCAgent 或激活 Conda。正式组合导入 `sigflow_u2`，分发名 `sigflow-u2-teaching`；U1 的 `sigflow_edu_agent` 保持独立。

## U1：共享入口组合

以下函数接受由正式宿主已经准备好的 Runtime、认证上下文和证据 resolver；不创建第二个服务。resolver 的返回值是完整、当前、已授权的 Evidence，其摘要必须等于该会话已提交 checkpoint 中的事实。

```python
from dataclasses import asdict
from sigflow_u2.host.shared_adapter import (
    SharedRunContext, SharedTeachingAdapter, TeachingHandle,
)

def start_teaching(runtime, resolve_evidence, authenticated_context,
                   question, request_key):
    adapter = SharedTeachingAdapter(runtime, resolve_evidence)
    handle = adapter.start(SharedRunContext(**authenticated_context), question,
                           idempotency_key=request_key, intent='diagnose')
    return adapter, asdict(handle)

def restore_handle(persisted_handle):
    return TeachingHandle(
        context=SharedRunContext(**persisted_handle['context']),
        teaching_session_id=persisted_handle['teaching_session_id'],
        kind=persisted_handle['kind'],
    )
```

调用方将返回的 handle 记录保存到现有 U1 session/run 关联事务，不让客户端选择他人的 handle。后续由同一工程范围的 Runtime 调用 `adapter.project(handle)`；后台动作尚未提交时保持 pending，不能把接受回执当成完成结果。重启后使用现有显式 recover 流程，不直接重新发布旧卡。execute_plan 使用真实审批的 grant_ref，U1/S1 负责授权，U2 不签发 grant。动作 expected_state_version 使用 project 返回的字符串版本。

当前共享接口不能完整表示教学等待、学生动作及内部事件，U1/S1 需确定注册位置和扩展契约。TeachingCard、PlanCard、终态 agent-run 已用实际冻结 schema 验证；外层 shared-projection 是内部组合结果，不是已发布的新公共 API。细节见[共享适配器](stage-28-shared-adapter.md)。

## U3：实际 UA-10 接收端

```python
from sigflow_u2.host.memory_adapter import MemoryAdapter

def build_memory(receiver_factory, scope_allowed):
    return MemoryAdapter(receiver_factory, scope_allowed)
```

receiver_factory 必须返回 context manager，在每次调用的当前线程创建并关闭接收器连接。接收器实现现有 `record_teaching_fact(fact)` 和 `get_teaching_context(learner_id=..., project_id=..., concept_ids=..., max_bytes=...)`；实际 DTO 见[事实与历史端口](stage-23-teaching-memory-ports.md)。将 adapter 传给 `Runtime(..., teaching_memory=adapter)`，或由[可信启动注入](stage-24-memory-entry.md)组合，复用提交后 outbox 和生成前历史加载；无额外模型调用。普通 CLI 默认仍未配置真实接收端。

scope_allowed 是可信权限/撤销回调，不能直接取自 HTTP 或模型。U3 在自身事务内执行 event_id 去重、删除 tombstone 及写删竞争检查；U2 的调用后检查不能撤销接收端已经提交的写入。停止时先拒绝新会话/动作，关闭 Runtime 的工作者，再关闭适配器；后端工厂管理连接生命周期。不得共享预先创建的线程受限 SQLite 连接。连接围栏与失败语义见[操作级适配器](stage-29-memory-adapter.md)。本轮没有创建 U3 学习数据库。

## U1/S3 与教师：L4 内部正文交付

```python
from sigflow_u2.host.materials import ReviewedMaterialProvider

def build_reference_provider(manifest_path, external_pin,
                             trusted_directory, verify_review):
    return ReviewedMaterialProvider(manifest_path, external_pin,
                                    trusted_directory, verify_review)
```

配置方从独立可信渠道提供 manifest SHA-256 和真实审阅权限校验，不能把同目录自动算出的摘要当作外部固定值。manifest schema、有效期、两个不同审阅者和正文文件规则见[材料与正文端口](stage-30-reference-body.md)。审阅者字符串本身不证明教师身份；verify_review 必须现场核验权限、材料摘要、范围、撤销和有效期，目录需用 OS 权限限制写入。

将 provider 与现有真实 receipt_port 一起传给 `Runtime(..., reference_provider=provider, receipt_port=receipt_port)`。按既有 reference_request/confirm 流程完成两个确认和持久核销后，由可信调用方执行 `runtime.reference_body(sid, learner_id, expected_version)`，此处版本是整数。正文仅返回给该内部调用者，不写事件、状态、动作回执、学习事实或普通检索；读取中取消或版本变化会拒绝返回。重启须 recover 及 reference_reconcile。普通 HTTP L4 保持禁用，真实材料、教师认证和正文 UI 路由仍需相应责任方交付。

## U2 评测操作

sigflowAgent 目录中的 `.venv\Scripts\python.exe -m sigflow_u2.evaluation.preflight` 不读 Key、不联网；当前没有团队冻结 ModelCase，输出 blocked 并退出 1 是准备条件报告，不是已运行批次或业务测试失败。具备独立固定摘要、至少 40 个有效支持案例及版本后，按[预检说明](stage-31-evaluation-preflight.md)检查至少三次重复和修复预算。

预检通过后，由明确授权的操作者使用既有 `sigflow_u2.evaluation.runner run --mode deepseek` 和同一输入、pin、版本及预算写入新输出目录。真实 API 批次及其成本不由预检触发；两名实际教师独立评分，再用 runner review 汇总与整改。开发 40 场景、Mock/合成测试或 probe fixtures 不得替代团队冻结输入和真实评分。

## 交接验收与责任边界

阶段 28–31 的实现、回归、当前真实工具闭环、源码摘要及历史失败记录见[机器验证](verification-stage28-31.json)，完整状态见[交付核对表](u2-completion-status.md)。已交付 U2 可独立完成的组合组件；尚需 U1 正式注册/生命周期、U3 实际接收端和数据、S2 可信 detector/行为契约、真实审阅材料与教师权限、S3 UI/目标平台签收、冻结模型批次和双教师评分。未到位条件保留未完成，不能用示例输入代签。
