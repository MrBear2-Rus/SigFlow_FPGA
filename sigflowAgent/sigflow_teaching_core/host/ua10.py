"""复用正式 U2 内存端口；不读取凭据，不启动模型或实验后端。"""
from dataclasses import replace
from threading import RLock

from .teaching_memory import validate_fact


class U2TeachingMemoryPort:
    scoped_history = True

    def __init__(self, config, *, instance_id, concepts, evidence_origin='fixture'):
        from memory.models import identifier
        if evidence_origin not in ('tool','fixture'):raise ValueError('invalid_memory_origin')
        identifier(instance_id)
        if not 128<=config.default_token_budget<=1536:raise ValueError('invalid_memory_budget')
        self.config,self.instance_id,self.concepts=config,instance_id,tuple(concepts)
        self.mode='real' if evidence_origin=='tool' else 'mock'
        self._lock=RLock();self._closed=False

    def _memory(self, binding, config=None):
        from ua10 import TeachingMemory
        if self._closed:raise RuntimeError('memory_closed')
        if binding['instance_id']!=self.instance_id:raise ValueError('memory_instance_mismatch')
        return TeachingMemory(config or self.config,trusted_binding=binding,concepts=self.concepts,execution_mode=self.mode)

    def record_teaching_fact(self, fact):
        validate_fact(fact,fact)
        with self._lock, self._memory(fact) as memory:
            return memory.record_event(fact)

    def get_teaching_context(self, *, learner_id, project_id, instance_id, session_id, revision,
                             concept_ids, max_bytes, as_of):
        if type(max_bytes) is not int or max_bytes<128:raise ValueError('invalid_memory_budget')
        if len(concept_ids)!=1 or not set(concept_ids)<=set(self.concepts):raise ValueError('memory_single_concept_scope')
        budget=min(self.config.default_token_budget,max_bytes,1536)
        binding=dict(learner_id=learner_id,project_id=project_id,instance_id=instance_id,session_id=session_id)
        with self._lock, self._memory(binding,replace(self.config,default_token_budget=budget)) as memory:
            concept=concept_ids[0]
            result=memory.get_teaching_context('历史提示 '+concept,concept,'current-context',revision,as_of=as_of)
        summary=result.text
        if len(summary.encode('utf-8'))>budget:raise ValueError('memory_byte_budget_exceeded')
        return dict(learner_id=learner_id,project_id=project_id,concept_ids=concept_ids,summary=summary)

    def close(self):
        with self._lock:self._closed=True


class BudgetedJSONService:
    """授权与预算均由宿主提供；与卡片复用同一个 Provider 实例。"""
    def __init__(self, provider, reserve, *, authorized=False):
        if type(authorized) is not bool or not callable(reserve):raise ValueError('invalid_pattern_authorization')
        self.provider,self.reserve,self.authorized=provider,reserve,authorized

    def complete_json(self,messages,*,max_tokens):
        if not self.authorized:raise PermissionError('pattern_network_not_authorized')
        return self.provider.complete_json(messages,max_tokens=max_tokens,reserve=self.reserve)
