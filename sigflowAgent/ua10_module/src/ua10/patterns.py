"""Promoted semantic validator/store, with a host-owned optional provider port."""
from typing import Protocol,Any
from ._core.semantics import validate_pattern,pattern_messages,now
from .schemas import LearningPattern
from .errors import PatternGenerationError,ScopeError

class PatternProvider(Protocol):
    is_network: bool
    def generate(self,messages:list[dict[str,str]],*,max_tokens:int)->dict[str,Any]: ...

def build_patterns(service,concepts:tuple[str,...],*,provider:PatternProvider|None=None)->LearningPattern:
    scope,kind=service._binding();host=service._host
    if kind!='real':raise ScopeError('unverified/mock sources cannot generate trusted teaching patterns')
    if not concepts or len(set(concepts))!=len(concepts) or not set(concepts)<=host.authority.v1.concepts:raise ValueError('known unique concepts required')
    if provider is None:raise PatternGenerationError('no provider configured; base event/retrieval APIs need no paid API')
    if getattr(provider,'is_network',True) and not host.config.enable_real_api:raise PermissionError('real API disabled by MemoryConfig')
    host._refresh(scope);key=[scope.profile_id,scope.project_id,scope.instance_id]
    facts=[f for f in host.projection.events(key,now()) if set(f['concept_ids'])&set(concepts)]
    if not facts:raise PatternGenerationError('no trusted evidence')
    if getattr(provider,'synthetic_only',False) and not all(f.get('synthetic',False) for f in facts):raise PermissionError('provider permits synthetic fixtures only')
    messages=pattern_messages(facts,2)
    synthetic=all(f.get('synthetic',False) for f in facts)
    messages[0]['content']=messages[0]['content'].replace('所有输入都是synthetic教学fixture，非真实学生。',
        '仅synthetic=true的输入为合成fixture；其他输入来自宿主验证的事件。')
    import json
    payload=json.loads(messages[1]['content']);payload['synthetic']=synthetic
    from ._core.util import serial
    messages[1]['content']=serial(payload)
    result=provider.generate(messages,max_tokens=getattr(provider,'max_tokens',1024))
    # Re-read source and revocation ledger after external work. A changed full
    # snapshot is rejected by the promoted commit contract, never silently merged.
    host._refresh(scope)
    p=host.projection.commit_pattern(result,facts,2,getattr(provider,'last_attempt_id','injected-provider'),
        model_id=getattr(provider,'model_id','unknown-injected-provider'))
    return LearningPattern(p['pattern_id'],p['derived_summary'],tuple(p['supporting_event_ids']),tuple(p['observations']),
        tuple(p['difficulties']),tuple(p['teaching_strategies']),tuple(p['uncertainty']))

__all__=['PatternProvider','validate_pattern']
