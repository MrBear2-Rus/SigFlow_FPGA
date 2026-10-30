"""Host-bound memory DTOs; shared evidence pool makes B/C a controlled comparison."""
from .util import *
from .semantics import now
from .store import utc,EvidenceViolation
from .query_api import Host as OriginalHost,API,event_item,pattern_item
from .hybrid import LABELS,apply_rerank
import uuid

class Host(OriginalHost):
    def __init__(self,store,encoder,hybrid,semantic_hook=None):
        super().__init__(store,encoder); self.hybrid=hybrid; self.semantic_hook=semantic_hook
    def bind_from_authenticated_host(self,scope,source_kind='real'):
        original=super().bind_from_authenticated_host(scope,source_kind)
        return MemoryService(self,original._binding)

class MemoryService(API):
    def search_memories(self,question,*,as_of,token_budget=768,top_k=6,version=2,**options):
        self._host._check(self._binding)
        t=self._host.hybrid.search(self._binding.scope,question,as_of,version=version,k=top_k,options=options,source_kind=self._binding.source_kind)
        if t['needs_semantic'] and self._host.semantic_hook:
            t=apply_rerank(t,self._host.semantic_hook(t))
        def items(rows): return [event_item(self._host.store.event(r['event_id']),r['rrf']) for r in rows if self._host.store.event(r['event_id'])]
        return self._finish('search_memories',items(t['raw']),items(t['post']),items(t['selected']),as_of=as_of,token_budget=token_budget)
    def get_learning_patterns(self,*,as_of,concept=None,token_budget=768):
        self._events(as_of)
        rows=[]
        for p in self._host.store.patterns(self._binding.scope,as_of,self._binding.source_kind,concept):
            item=pattern_item(p)
            for field in ('observations','difficulties','teaching_strategies','unsupported_conclusions','model_id','prompt_version','needs_teacher_review'):
                if field in p: item[field]=p[field]
            rows.append(item)
        return self._finish('get_learning_patterns',rows,rows,rows,as_of=as_of,token_budget=token_budget)
    def get_teaching_memory_bundle(self,task,*,condition='pattern',version=2,token_budget=768,request_time=None):
        # task is a trusted host task, not model-controlled identity arguments.
        self._host._check(self._binding)
        if tuple(task['scope'])!=self._binding.scope: raise EvidenceViolation('task_binding_mismatch')
        if condition not in ('none','basic','pattern'): raise ValueError('condition')
        if type(token_budget) is not int or not 128<=token_budget<=2048: raise ValueError('budget')
        clock=utc(request_time or now()); cutoff=utc(task['as_of'])
        with self._host.store.lock:
            events=self._events(cutoff); live={e['event_id']:e for e in events}
            # Course relation is appropriate here because the current concept is host-verified.
            trace=(self._host.hybrid.search(self._binding.scope,task['question']+' '+LABELS.get(task['concept'],task['concept']),cutoff,version=version,k=6)
                if self._binding.source_kind=='real' else dict(raw=[],post=[],selected=[],abstained=True,reason='untrusted source'))
            candidates=[p for p in self._host.store.patterns(self._binding.scope,cutoff,self._binding.source_kind,task['concept'])
                if set(p['supporting_event_ids'])<=set(live) and p['prompt_version']=='llm-pattern-v'+str(version)
                and len(p['concept_ids'])==1]
            candidates.sort(key=lambda p:p['created_at'],reverse=True)
            pat=candidates[0] if candidates else None
            # Both B and C get precisely the same original events, including expanded
            # evidence dependencies. Pattern text never substitutes for extra facts.
            if pat: ids=list(pat['supporting_event_ids'])
            else: ids=[r['event_id'] for r in trace['selected'] if task['concept'] in live[r['event_id']]['concept_ids']]
            if version>=2 and ids:
                issues={live[i]['issue_id'] for i in ids}
                # Include the full trusted state sequence, not just observations;
                # otherwise a question about a past pass invites unsupported guessing.
                ids=sorted(set(ids)|{e['event_id'] for e in events if e['issue_id'] in issues and task['concept'] in e['concept_ids']})
            if task['concept'] not in LABELS or self._binding.source_kind!='real': ids=[]; pat=None
            facts=sorted([live[i] for i in ids],key=lambda e:(
                0 if e['event_type'] in ('retry_passed','retry_failed') else
                1 if e['event_type'] in ('issue_observed','u2_diagnostic_observed') else 2,
                e['created_at'],e['event_id']))
            original_fact_count=len(facts)
            runs=sorted({e['run_id'] for e in facts})
            def row(e):
                # Columnar DTO preserves original identity; semantic state is an enum,
                # not an LLM interpretation. Full original facts remain queryable.
                if version>=2: return [e['event_id'],runs.index(e['run_id']),e['revision'],e['event_type'],e['hint_level']]
                return [e['event_id'],e['run_id'],e['revision'],e['event_type'],e['outcome'],e['hint_level']]
            shared=dict(schema='teaching-memory-v2',synthetic=all(e.get('synthetic',False) for e in facts),source_kind=self._binding.source_kind,
                historical_before=cutoff,current_state='use_current_evidence_only',
                columns=['event_id','run_id','revision','event_type','outcome','hint_level'],facts=[row(e) for e in facts],
                uncertainty=['generated is not presented','modified is not passed','past pass is not current pass or mastery',
                    'different runs are related, not the same issue; independence unknown'])
            if version>=2:
                shared.pop('current_state')
                shared['columns']=['event_id','run_index','revision','event_type','hint_level']
                shared['runs']=runs
                shared['historical_before']=cutoff.replace('.000000','').replace('+00:00','Z')
                shared['uncertainty']=['历史不能覆盖当前或证明掌握/独立；生成≠展示；修改≠通过；不同issue不合并']
            derived=[]
            if pat:
                # Whole claims only; no string slicing that could drop a negation.
                for s in pat['teaching_strategies']:
                    derived.append(dict(layer='strategy_recommendation',text=s['suggestion'],evidence_indices=[ids.index(i) for i in s['event_ids']]))
                for o in pat['observations']:
                    derived.append(dict(layer='pattern',kind=o['kind'],text=o['text'],evidence_indices=[ids.index(i) for i in o['event_ids']]))
                if version>=2:
                    # Give one observation AND one recommended action priority;
                    # repeated recommendations must not crowd out the actual pattern.
                    first=derived[0] if derived else None
                    derived.sort(key=lambda r:(0 if r is first else 1 if r['layer']=='pattern' else 2))
                # indices must refer to actual ordered rows, not the original sorted IDs.
                ordered=[e['event_id'] for e in facts]
                for item in derived: item['evidence_indices']=[ordered.index(ids[i]) for i in item['evidence_indices']]
            # Choose a common fact set and C additions BEFORE selecting condition.
            # If a complete pattern cannot fit, retain common facts and omit pattern.
            while facts and self._host.encoder.tokens(serial(shared))>token_budget:
                facts.pop(); shared['facts']=[row(e) for e in facts]; pat=None; derived=[]
            additions=[]
            for item in derived:
                trial=dict(shared,derived=additions+[item],needs_teacher_review=True)
                if self._host.encoder.tokens(serial(trial))<=token_budget: additions.append(item)
            result=dict(shared)
            if condition=='none':
                result['facts']=[]
                result.pop('runs',None)
            if condition=='pattern' and additions:
                result['derived']=additions
                result['needs_teacher_review']=True
            if condition=='none': result.pop('usage_policy',None)
            if self._host.encoder.tokens(serial(result))>token_budget:
                result={'facts':[],'uncertainty':['budget_exhausted']}
                pat=None; additions=[]
            result['truncated']=len(result['facts'])<original_fact_count or len(additions)<len(derived)
            result['omitted_fact_count']=original_fact_count-len(result['facts'])
            while self._host.encoder.tokens(serial(result))>token_budget:
                result.pop('derived',None)
                result.pop('needs_teacher_review',None)
                if result['facts']:result['facts'].pop()
                else:result={'facts':[],'uncertainty':['budget_exhausted'],'truncated':True,'omitted_fact_count':original_fact_count}
                result['truncated']=True
                result['omitted_fact_count']=original_fact_count-len(result['facts'])
                if not result['facts']:break
            assert self._host.encoder.tokens(serial(result))<=token_budget
            qid='bundle-'+uuid.uuid4().hex
            refs=[r[0] for r in result['facts']]
            journal=dict(memory_query_id=qid,task_type='get_teaching_memory_bundle',condition=condition,
                trusted_scope=list(self._binding.scope),as_of=cutoff,request_time=clock,
                selected_memory_ids=refs,pattern_id=pat['pattern_id'] if pat and condition=='pattern' and additions else None,
                returned_content=result,token_count=self._host.encoder.tokens(serial(result)),token_budget=token_budget,
                current_evidence_id=task['evidence_id'],reward=None,feedback_state='unknown')
            with self._host.store.db: self._host.store.db.execute('INSERT INTO queries VALUES (?,?)',(qid,serial(journal)))
            return dict(bundle=result,journal=journal,retrieval_trace=trace)
