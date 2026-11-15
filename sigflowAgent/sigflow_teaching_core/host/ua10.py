"""复用正式 U2 内存端口；不读取凭据，不启动模型或实验后端。"""
from dataclasses import replace
from hashlib import sha256
from threading import RLock

from .teaching_memory import validate_fact

CONCEPT_ALIASES = {
    'latch': 'latch', 'reset': 'reset', 'drivers': 'multiple_driver',
    'ports': 'port_connection', 'syntax': 'syntax_error',
    'width_signedness': 'bit_width', 'combinational_feedback': 'combinational_loop',
    'simulation_mismatch': 'simulation_semantics', 'job_observation': None,
}


def _learner_key(value):
    from memory.models import MemoryErrorBase, identifier
    try:
        return identifier(value)
    except MemoryErrorBase:
        # '+' is allowed by UA-10 and excluded from U2 learner IDs. Keep
        # existing valid profiles intact and reserve this namespace for aliases.
        return 'u2+learner+' + sha256(value.encode('utf-8')).hexdigest()

class U2TeachingMemoryPort:
    scoped_history = True
    history_timeout_seconds = 1.0

    def __init__(self, config, *, instance_id, concepts, evidence_origin='fixture'):
        from memory.models import identifier
        if evidence_origin not in ('tool','fixture'):raise ValueError('invalid_memory_origin')
        identifier(instance_id)
        if not 128<=config.default_token_budget<=1536:raise ValueError('invalid_memory_budget')
        self.config,self.instance_id,self.concepts=config,instance_id,tuple(concepts)
        if any(concept not in CONCEPT_ALIASES for concept in self.concepts):
            raise ValueError('unsupported_memory_concept')
        self.memory_concepts = tuple(sorted({CONCEPT_ALIASES[c] for c in self.concepts
                                            if CONCEPT_ALIASES[c] is not None}))
        if not self.memory_concepts:
            raise ValueError('empty_memory_concept_scope')
        self.mode='real' if evidence_origin=='tool' else 'mock'
        self._lock=RLock();self._closed=False

    def _memory(self, binding, config=None):
        from ua10 import TeachingMemory
        if self._closed:raise RuntimeError('memory_closed')
        if binding['instance_id']!=self.instance_id:raise ValueError('memory_instance_mismatch')
        return TeachingMemory(config or self.config,trusted_binding=binding,concepts=self.memory_concepts,execution_mode=self.mode)

    def record_teaching_fact(self, fact):
        validate_fact(fact,fact)
        if not set(fact['concept_ids']) <= set(self.concepts):
            raise ValueError('memory_concept_scope')
        # Neutral tool observations have no curriculum concept. The original
        # metadata remains in U2's outbox under the same immutable event ID.
        mapped = dict(fact, learner_id=_learner_key(fact['learner_id']),
                      concept_ids=sorted({CONCEPT_ALIASES[c] for c in fact['concept_ids']
                                          if CONCEPT_ALIASES[c] is not None}))
        with self._lock, self._memory(mapped) as memory:
            return memory.record_event(mapped)

    def get_teaching_context(self, *, learner_id, project_id, instance_id, session_id, revision,
                             concept_ids, max_bytes, as_of):
        if type(max_bytes) is not int or max_bytes<128:raise ValueError('invalid_memory_budget')
        if len(concept_ids)!=1 or not set(concept_ids)<=set(self.concepts):raise ValueError('memory_single_concept_scope')
        concept = CONCEPT_ALIASES[concept_ids[0]]
        if concept is None:
            return dict(learner_id=learner_id, project_id=project_id, concept_ids=concept_ids, summary='')
        budget=min(self.config.default_token_budget,max_bytes,1536)
        binding=dict(learner_id=_learner_key(learner_id),project_id=project_id,
                     instance_id=instance_id,session_id=session_id)
        with self._lock, self._memory(binding,replace(self.config,default_token_budget=budget)) as memory:
            result=memory.get_teaching_context('历史提示 '+concept,concept,'current-context',revision,as_of=as_of)
        summary=result.text
        if len(summary.encode('utf-8'))>budget:raise ValueError('memory_byte_budget_exceeded')
        return dict(learner_id=learner_id,project_id=project_id,concept_ids=concept_ids,summary=summary)

    def close(self):
        with self._lock:self._closed=True

    def _record_scope(self, binding):
        from memory import Scope
        if binding.instance_id != self.instance_id:
            raise ValueError('memory_instance_mismatch')
        return Scope(_learner_key(binding.learner_id), binding.project_id, binding.instance_id, binding.session_id)

    def learning_records_current(self, binding, result):
        """Recheck authority after a delayed response, including event revocation."""
        from ua10 import MemoryHost
        scope = self._record_scope(binding)
        event_ids = {row['event_id'] for row in result['records']}
        with self._lock:
            if self._closed:
                return False
            with MemoryHost(self.config) as host:
                # Authority facts already apply session/profile deletions and
                # individual revocations. No second timeline projection is needed.
                live = {fact['event_id'] for fact in host.authority.facts(scope)
                        if fact['source_kind'] == self.mode}
                return event_ids <= live

    def get_learning_records(self, binding):
        """Read the receiver's public timeline with the host-issued scope."""
        from ua10 import MemoryHost
        from datetime import datetime, timezone
        scope = self._record_scope(binding)
        with self._lock:
            if self._closed:
                raise RuntimeError('memory_closed')
            with MemoryHost(self.config) as host:
                api = host.bind(scope, source_kind=self.mode)
                facts = []
                offset = 0
                as_of = datetime.now(timezone.utc).isoformat()
                for _ in range(4):
                    result = api.get_learning_timeline(
                        as_of=as_of, limit=12-len(facts), offset=offset, token_budget=2048, newest_first=True)
                    facts.extend(result.facts)
                    if not result.has_more or result.next_offset is None or len(facts) >= 12:
                        break
                    offset = result.next_offset
                rows = [dict(event_id=fact.event_id, revision=fact.revision,
                             event_type=fact.event_type, source_kind=fact.source_kind,
                             occurred_at=fact.occurred_at, status=fact.status,
                             summary=fact.summary[:512]) for fact in reversed(facts)]
                return dict(status='ready', records=rows, truncated=result.truncated or result.has_more)


class BudgetedJSONService:
    """授权与预算均由宿主提供；与卡片复用同一个 Provider 实例。"""
    def __init__(self, provider, reserve, *, authorized=False):
        if type(authorized) is not bool or not callable(reserve):raise ValueError('invalid_pattern_authorization')
        self.provider,self.reserve,self.authorized=provider,reserve,authorized

    def complete_json(self,messages,*,max_tokens):
        if not self.authorized:raise PermissionError('pattern_network_not_authorized')
        return self.provider.complete_json(messages,max_tokens=max_tokens,reserve=self.reserve)
