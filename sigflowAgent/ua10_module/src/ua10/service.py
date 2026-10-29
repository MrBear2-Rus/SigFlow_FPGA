"""Public host facade. Only this layer writes V1 or issues query capabilities."""
from dataclasses import asdict
from typing import Callable,Any
from pathlib import Path
import threading,json
from memory import Scope,LearningEvent
from memory.models import utc_now,identifier
from .config import MemoryConfig
from .events import EventAuthority
from .schemas import (Observation,WriteResult,QueryResult,LearningFact,LearningPattern,PatternResult,
    TeachingTask,TeachingMemoryBundle,RevocationResult,SourceKind,RelatedAttempt,RelatedAttemptsResult)
from .errors import (ClosedServiceError,ThreadOwnershipError,ProjectionError,ScopeError,ConfigurationError)
from ._core.semantics import PatternStore,now
from ._core.store import Store,utc,EvidenceViolation
from ._core.hybrid import Hybrid,index_text
from ._core.bundles import Host as QueryHost
from ._core.query_api import event_item
from ._core.embedding import NoEmbeddings,BGEEncoder
from ._core.util import serial,digest

class AuthorityProjection(PatternStore):
    """Disposable index projection; every read verifies original V1 liveness."""
    def __init__(self,path,owner):self.owner=owner;super().__init__(path)
    def events(self,scope,as_of,source_kind='real'):
        binding=self.owner._scopes.get(tuple(scope))
        if binding is None:return []
        live={e['event_id']:e for e in self.owner.authority.facts(binding)}
        return [e for e in super().events(scope,as_of,source_kind)
            if e['event_id'] in live and digest(e)==digest(live[e['event_id']])]

class MemoryHost:
    """One instance per owning thread. Separate workers may open separate hosts.

    The application authenticates identities before bind(); these Python objects
    are capabilities inside a trusted process, not a new authentication scheme.
    """
    def __init__(self,config:MemoryConfig,*,lightmem_backend=None,_fault:Callable[[str],None]|None=None):
        self.config=config;self._thread=threading.get_ident();self._closed=False;self._scopes={};self._issued={}
        marker=config.data_dir/'ua10-module.json'
        if (config.data_dir/'events.sqlite').exists() and not marker.exists():
            raise ConfigurationError('foreign V1 database: migrate a copy explicitly; do not open production data in place')
        config.data_dir.mkdir(parents=True,exist_ok=True)
        if not marker.exists():marker.write_text('{"schema":"ua10-module-v1"}',encoding='utf-8')
        self.authority=EventAuthority(config.data_dir,_fault)
        try:
            self.encoder=BGEEncoder(config.data_dir/'vectors.sqlite',config.embedding_model_path) if config.embedding_model_path else NoEmbeddings()
            self.projection=AuthorityProjection(config.data_dir/'derived.sqlite',self)
            if config.backend=='lightmem':
                if lightmem_backend is None:raise ConfigurationError('supply the explicitly configured LightMem adapter; no implicit installation')
                if not self.encoder.available:raise ConfigurationError('LightMem backend requires the local BGE adapter')
            self.backend=lightmem_backend
            self._purge_inactive()
        except Exception:
            if hasattr(self,'projection'):self.projection.close()
            if hasattr(self,'encoder'):self.encoder.close()
            self.authority.close();raise
    def _check(self):
        if self._closed:raise ClosedServiceError('memory host is closed')
        if threading.get_ident()!=self._thread:raise ThreadOwnershipError('create a MemoryHost in the worker thread; do not share SQLite connections')
    def _purge_inactive(self):
        self.authority.purge_deleted()
        revoked={r[0] for r in self.authority.control.execute('SELECT id FROM revocations')}
        for row in self.projection.db.execute('SELECT data FROM events').fetchall():
            e=json.loads(row[0])
            if e['event_id'] in revoked or self.authority.v1.ledger.blocks(e['scope'][0],e['session_id']):
                self._remove_projection(e)
    def _remove_projection(self,event):
        eid=event['event_id'];self.projection.delete(event['scope'],eid,now());self.encoder.purge(index_text(event,{}))
        self.encoder.purge_event(eid)
        if self.backend:self.backend.purge_event(eid)
        # Returned-context journals can contain event copies. Physical session
        # deletion and revocation remove those derived copies as well.
        for qid,data in self.projection.db.execute('SELECT id,data FROM queries').fetchall():
            q=json.loads(data);refs=set(q.get('selected_memory_ids',()))|set(q.get('candidate_memory_ids',()))|set(q.get('evidence_refs',()))
            if eid in refs:
                with self.projection.db:self.projection.db.execute('DELETE FROM queries WHERE id=?',(qid,))
    def create_profile(self,profile_id:str,display_name:str='匿名学习者')->str:
        self._check();self.authority.v1.create_profile(profile_id,display_name);return profile_id
    def create_session(self,learner_id:str,project_id:str,instance_id:str,session_id:str)->Scope:
        self._check();return self.authority.v1.create_session(learner_id,project_id,instance_id,session_id)
    def bind(self,scope:Scope,*,source_kind:SourceKind='real')->'MemoryService':
        self._check()
        if source_kind not in ('real','mock','imported_unverified'):raise ScopeError('invalid source kind')
        self.authority.v1.repo.check_scope(scope)
        capability=object();self._issued[capability]=(scope,source_kind)
        self._scopes[(scope.profile_id,scope.project_id,scope.instance_id)]=scope
        return MemoryService(self,capability)
    def _refresh(self,scope):
        self.authority.v1.repo.check_scope(scope)
        facts=self.authority.facts(scope);live={f['event_id']:f for f in facts};key=(scope.profile_id,scope.project_id,scope.instance_id)
        self._scopes[key]=scope
        prior=[json.loads(r[0]) for r in self.projection.db.execute('SELECT data FROM events WHERE scope=?',(serial(list(key)),))]
        for old in prior:
            if old['event_id'] not in live:
                self._remove_projection(old)
        for fact in facts:self.projection.append(fact)
        hybrid=Hybrid(self.projection,self.encoder,{})
        if self.backend:
            # Optional projection reconciles BEFORE queries. Every native hit is
            # also intersected with V1 authority inside Hybrid.search().
            self.backend.sync(tuple(key),facts,self.encoder)
            hybrid.backend=self.backend.search
        return QueryHost(self.projection,self.encoder,hybrid)
    def close(self):
        if self._closed:return
        self._check()
        try:
            self.projection.close();self.encoder.close();self.authority.close()
        finally:self._closed=True;self._issued.clear()
    def __enter__(self):return self
    def __exit__(self,*args):self.close()

class MemoryService:
    """Bound facade; public query parameters contain no learner/project identity."""
    def __init__(self,host:MemoryHost,capability:object):
        if capability not in host._issued:raise ScopeError('unissued binding')
        self._host=host;self._capability=capability
    def _binding(self):
        self._host._check()
        if self._capability not in self._host._issued:raise ScopeError('binding revoked')
        scope,kind=self._host._issued[self._capability];self._host.authority.v1.repo.check_scope(scope)
        return scope,kind
    def _api(self):
        scope,kind=self._binding();host=self._host._refresh(scope)
        return host.bind_from_authenticated_host((scope.profile_id,scope.project_id,scope.instance_id),kind)
    def _budget(self,value):
        if value is None:return self._host.config.default_token_budget
        if type(value) is not int or not 128<=value<=2048:raise ValueError('token budget must be an integer in 128..2048')
        return value
    def create_run(self,revision:str,snapshot_id:str,*,run_id:str,execution_mode:str='mock')->str:
        scope,_=self._binding();return self._host.authority.v1.create_run(scope,revision,snapshot_id,run_id=run_id,execution_mode=execution_mode)
    def complete_run(self,run_id:str)->None:
        scope,_=self._binding();self._host.authority.v1.set_run_status(scope,run_id,'Completed')
    def append_learning_event(self,event:LearningEvent,*,observation:Observation|None=None,available_at:str|None=None,synthetic:bool=False)->WriteResult:
        scope,_=self._binding()
        try:result=self._host.authority.append(scope,event,observation,available_at=available_at,synthetic=synthetic)
        except RuntimeError as exc:
            committed=self._host.authority.v1.repo.receipt(event.event_id) is not None
            raise ProjectionError('write interrupted; retry the identical event',source_committed=committed,event_id=event.event_id) from exc
        try:self._host._refresh(scope)
        except Exception as exc:raise ProjectionError('source committed; projection requires replay',source_committed=True,event_id=result.event_id) from exc
        return result
    def _query(self,value)->QueryResult:
        return QueryResult(tuple(LearningFact.from_item(x) for x in value['memories']),value['abstained'],value.get('abstention_reason'),
            value['memory_query_id'],value['token_count'],value['token_budget'],self._host.encoder.measurement,value['truncated'],
            value.get('has_more',False),value.get('next_offset'))
    def search_memories(self,question:str,*,as_of:str|None=None,top_k:int=6,token_budget:int|None=None,current_only:bool=False,revision:str|None=None)->QueryResult:
        if not isinstance(question,str) or not 1<=len(question)<=4096:raise ValueError('bounded nonempty question required')
        if type(top_k) is not int or not 1<=top_k<=20:raise ValueError('top_k must be 1..20')
        value=self._api().search_memories(question,as_of=as_of or now(),top_k=top_k,token_budget=self._budget(token_budget),
            current_only=current_only,revision=revision)
        return self._query(value)
    def get_learning_timeline(self,*,as_of:str|None=None,concept:str|None=None,run_id:str|None=None,limit:int=20,offset:int=0,token_budget:int|None=None)->QueryResult:
        return self._query(self._api().get_learning_timeline(as_of=as_of or now(),concept=concept,run_id=run_id,limit=limit,
            token_budget=self._budget(token_budget),offset=offset))
    def get_related_attempts(self,concept:str,*,as_of:str|None=None,token_budget:int|None=None)->RelatedAttemptsResult:
        result=self._api().get_related_attempts(concept=concept,as_of=as_of or now(),token_budget=self._budget(token_budget))
        attempts=[]
        for x in result['memories']:
            linked=x['same_issue_with_other_runs'];linked=tuple(linked) if isinstance(linked,list) else ()
            attempts.append(RelatedAttempt(x['run_id'],x['revision'],tuple(x['evidence_refs']),x['relationship'],linked))
        return RelatedAttemptsResult(tuple(attempts),result['memory_query_id'],result['truncated'])
    def get_learning_patterns(self,*,as_of:str|None=None,concept:str|None=None,token_budget:int|None=None)->PatternResult:
        result=self._api().get_learning_patterns(as_of=as_of or now(),concept=concept,token_budget=self._budget(token_budget))
        patterns=tuple(LearningPattern(x['memory_id'],x['summary'],tuple(x['evidence_refs']),tuple(x.get('observations',())),tuple(x.get('difficulties',())),
            tuple(x.get('teaching_strategies',())),tuple(x['uncertainty'])) for x in result['memories'])
        return PatternResult(patterns,result['memory_query_id'],result['truncated'])
    def get_teaching_memory_bundle(self,task:TeachingTask,*,token_budget:int|None=None,include_patterns:bool=True)->TeachingMemoryBundle:
        scope,_=self._binding();api=self._api();task_dict=asdict(task)
        task_dict.update(scope=[scope.profile_id,scope.project_id,scope.instance_id],evidence_id=task.current_evidence_id)
        result=api.get_teaching_memory_bundle(task_dict,condition='pattern' if include_patterns else 'basic',
            token_budget=self._budget(token_budget))
        dto=result['bundle'];facts=tuple(LearningFact.from_item(event_item(self._host.projection.event(row[0]))) for row in dto['facts'])
        derived=dto.get('derived',());patterns=tuple(x for x in derived if x['layer']=='pattern');strategies=tuple(x for x in derived if x['layer']=='strategy_recommendation')
        return TeachingMemoryBundle(facts,patterns,strategies,tuple(dto['uncertainty']),dto,result['journal']['memory_query_id'],
            result['journal']['token_count'],self._host.encoder.measurement,dto.get('needs_teacher_review',False))
    def revoke_learning_event(self,event_id:str)->RevocationResult:
        scope,_=self._binding();result=self._host.authority.revoke(scope,event_id)
        try:self._host._refresh(scope)
        except Exception as exc:raise ProjectionError('revocation durable; projection replay required',source_committed=True,event_id=event_id) from exc
        return result
    def delete_session(self)->dict[str,Any]:
        scope,_=self._binding();result=self._host.authority.v1.delete_session(scope)
        # V1 tombstone denies stale bindings immediately, even if projection cleanup
        # is interrupted. New queries always check the V1 ledger before reading.
        if result['status']=='completed':self._host._purge_inactive()
        return result
    def rebuild_index(self)->None:
        scope,_=self._binding()
        key=serial([scope.profile_id,scope.project_id,scope.instance_id]);live=self._host.authority.facts(scope)
        # Explicit full rebuild from V1, including a corrupt event projection.
        # Derived patterns for this scope are invalidated; no automatic LLM call.
        with self._host.projection.db:
            self._host.projection.db.execute('DELETE FROM patterns WHERE scope=?',(key,))
            self._host.projection.db.execute('DELETE FROM events WHERE scope=?',(key,))
            self._host.projection.db.execute('DELETE FROM memory_fts WHERE event_id NOT IN (SELECT id FROM events)')
            for event in live:
                self._host.projection.db.execute('DELETE FROM tombstones WHERE id=?',(event['event_id'],))
        for event in live:self._host.encoder.purge(index_text(event,{}))
        self._host._refresh(scope)
    def build_learning_patterns(self,concepts:tuple[str,...],*,provider=None)->LearningPattern:
        from .patterns import build_patterns
        return build_patterns(self,concepts,provider=provider)
