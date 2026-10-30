"""面向 U2 的薄入口：消费已有教学事实，读取授权范围内的历史上下文。"""
from collections.abc import Mapping
from dataclasses import asdict, dataclass, is_dataclass
from typing import Any, Literal, Protocol

from memory import Scope
from memory.models import identifier
from .config import MemoryConfig
from .errors import ScopeError, ProjectionError, ConfigurationError
from .schemas import TeachingTask
from .service import MemoryHost
from ._core.semantics import now
from ._core.util import serial


class _TrustedBinding(Protocol):
    learner_id: str
    project_id: str
    instance_id: str
    session_id: str


@dataclass(frozen=True)
class TeachingContext:
    """前三个字段对应当前 U2 LearningHistory，其余字段供宿主审计。"""
    text: str
    references: dict[str, list[str]]
    evidence_kinds: tuple[str, ...]
    query_id: str
    unknown: tuple[str, ...]


class TeachingMemory:
    """每个工作线程/可信会话一个实例，不导入或驱动 U2 状态机。"""

    def __init__(self, config: MemoryConfig, *, trusted_binding: Mapping[str, str] | _TrustedBinding,
                 concepts: tuple[str, ...], execution_mode: Literal['mock', 'real'] = 'mock'):
        if not (is_dataclass(trusted_binding) and not isinstance(trusted_binding, type)) and not isinstance(trusted_binding, Mapping):
            raise ConfigurationError('可信绑定须为 SessionBinding 数据类或字段映射')
        binding = asdict(trusted_binding) if is_dataclass(trusted_binding) else dict(trusted_binding)
        names = ('learner_id', 'project_id', 'instance_id', 'session_id')
        values = [identifier(binding[name]) for name in names]
        if execution_mode not in ('mock', 'real'):
            raise ConfigurationError('执行模式必须由可信宿主指定')
        if not concepts or len(set(concepts)) != len(concepts):
            raise ScopeError('宿主必须绑定非空且不重复的概念范围')
        if config.default_token_budget > 1536:
            raise ConfigurationError('普通教学记忆预算不得超过 1536')
        self._host = MemoryHost(config)
        try:
            if not set(concepts) <= self._host.authority.v1.concepts:
                raise ScopeError('概念未在 V1 注册')
            self._scope = Scope(*values)
            service = self._host.authority.v1
            # 先检查删除账本；自动注册不能复活已删除的身份。
            if service.ledger.blocks(values[0], values[3]):
                from .errors import DeletedScopeError
                raise DeletedScopeError('该学习会话已删除')
            if service.db.connection.execute('SELECT 1 FROM learner_profile WHERE profile_id=?', (values[0],)).fetchone() is None:
                self._host.create_profile(values[0])
            if service.db.connection.execute('SELECT 1 FROM sessions WHERE session_id=?', (values[3],)).fetchone() is None:
                self._host.create_session(*values)
            self._api = self._host.bind(self._scope, source_kind=execution_mode)
        except Exception:
            self._host.close()
            raise
        self._concepts = frozenset(concepts)
        self._mode = execution_mode

    def record_event(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        """消费现有 ua10-host-fact.v1；返回后宿主才能确认 outbox。"""
        self._api._binding()
        if payload.get('schema_version') == 'edu.u2.teaching-fact.v1':
            from .adapters.u2_metadata import record_metadata
            return record_metadata(self, dict(payload))
        if payload.get('schema') != 'ua10-host-fact.v1' or payload.get('kind') not in ('start', 'transition'):
            raise ValueError('不支持的教学事实结构')
        binding = payload['binding']
        expected = (self._scope.profile_id, self._scope.project_id, self._scope.instance_id, self._scope.session_id)
        if tuple(binding[name] for name in ('learner_id', 'project_id', 'instance_id', 'session_id')) != expected:
            raise ScopeError('教学事实与宿主绑定身份不一致')
        concept = payload.get('concept_id')
        if concept is not None and concept not in self._concepts:
            raise ScopeError('教学事实超出宿主绑定的概念范围')
        service = self._host.authority.v1
        known = service.repo.events(self._scope.profile_id, project_id=self._scope.project_id,
            instance_id=self._scope.instance_id, session_id=self._scope.session_id)
        if any(not set(row['payload']['concept_ids']) <= self._concepts
               for row in known if row['run_id'] == payload['state']['run_id']):
            raise ScopeError('既有运行包含未授权概念，不能省略 concept_id 绕过权限')
        session = {name: payload[name] for name in ('binding', 'state', 'occurrences')}
        # 复用 V1 当前 U2 映射及事务、身份/来源校验，不重写教学事件语义。
        if payload['kind'] == 'start':
            mode = payload.get('execution_mode') or self._mode
            if mode != self._mode:
                raise ScopeError('教学事实执行模式与宿主授权不一致')
            result = service.start_u2_run(self._scope, session, {
                'project_id': self._scope.project_id, 'revision': binding['revision'], 'issue_id': binding['issue_id']
            }, execution_mode=mode)
        else:
            result = service.record_u2_snapshot(self._scope, session, trusted_concept_id=concept,
                checkpoint_ref=payload.get('checkpoint_ref'), **payload.get('action', {}))
        try:
            self._host._refresh(self._scope)
        except Exception as exc:
            raise ProjectionError('V1 已提交；重试原始 outbox 事实以恢复投影', source_committed=True) from exc
        return result

    def get_teaching_context(self, question: str, concept: str, current_evidence_id: str,
                             revision: str, *, as_of: str | None = None) -> TeachingContext:
        if not isinstance(question, str) or not 1 <= len(question) <= 4096:
            raise ValueError('教学查询必须为 1..4096 字符')
        identifier(current_evidence_id)
        identifier(revision)
        if concept not in self._concepts:
            raise ScopeError('查询超出宿主绑定的概念范围')
        bundle = self._api.get_teaching_memory_bundle(TeachingTask(
            question, concept, current_evidence_id, revision, as_of or now()))
        if any(not set(fact.concept_ids) <= self._concepts for fact in bundle.facts):
            raise ScopeError('候选事实包含授权范围外的概念，拒绝返回混合内容')
        # 仅序列化原有预算检查过的 DTO；不把完整审计副本再塞入上下文。
        text = serial(bundle.to_dict()) if bundle.facts else ''
        return TeachingContext(text, {'event_ids': [f.event_id for f in bundle.facts]},
            tuple(sorted({f.source_kind for f in bundle.facts})), bundle.query_id, bundle.unknown)

    def close(self) -> None:
        self._host.close()

    def __enter__(self) -> 'TeachingMemory':
        return self

    def __exit__(self, *args) -> None:
        self.close()
