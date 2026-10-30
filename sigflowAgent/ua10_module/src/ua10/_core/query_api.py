"""Host-bound query prototype, common gates for B/C/D; A calls actual V1 rules.

No learner/scope argument exists on the query surface. Host issuance is the trust
boundary, not authentication implemented by a model or by this local Python demo.
"""
from .util import *
from .store import utc,ScopeViolation,EvidenceViolation
import re, uuid, time
from .embedding import representation
from dataclasses import dataclass

ALIASES={'latch':['latch','锁存','分支没给值','所有赋值路径','不完整赋值','incomplete assignment'],
 'reset':['reset','复位','清零'],
 'blocking_nonblocking':['nonblocking','非阻塞','更新顺序','blocking assignment'],
 'bit_width':['width mismatch','bit width','位宽','截断']}
def route(question):
    q=question.lower()
    if any(w in q for w in ('password','密码','pll','phase noise','抖动')): return None
    for concept in ('bit_width','blocking_nonblocking','latch','reset'):
        if any(w in q for w in ALIASES[concept]): return concept
    return None

def event_item(e,score=0):
    status={'issue_observed':'diagnostic_observed','u2_diagnostic_observed':'diagnostic_observed','u2_card_generated':'hint_generated_presentation_unknown',
       'hint_presented':'hint_presented_receipt','student_modified':'modified_retest_unknown',
       'retry_passed':'passed_within_scope_not_mastery','retry_failed':'failed_within_scope'}.get(e['event_type'],'unknown')
    return dict(memory_id=e['event_id'],summary=e['summary'],event_type=e['event_type'],score=float(score),evidence_refs=[e['event_id']],
      source_kind=e['source_kind'],run_id=e['run_id'],revision=e['revision'],issue_id=e['issue_id'],
      occurred_at=e['created_at'],available_at=e['available_at'],concept_ids=e['concept_ids'],
      diagnostic_type=e['diagnostic_type'],validation_scope=e['validation_scope'],relevant_status=status,
      is_derived=False,synthetic=e.get("synthetic",False),occurrence_id=e.get('occurrence_id'),session_id=e.get('session_id'),
      snapshot_id=e.get('snapshot_id'),exercise_id=e.get('exercise_id'))
def pattern_item(p,score=1):
    return dict(memory_id=p['pattern_id'],summary=p['derived_summary'],score=float(score),
      evidence_refs=p['supporting_event_ids'],source_kind=p['source_kind'],concept_ids=p['concept_ids'],
      run_ids=p['supporting_run_ids'],time_range=p['time_range'],created_at=p['created_at'],
      relevant_status='historical_pattern_current_state_unknown',is_derived=True,uncertainty=p['uncertainty'],synthetic=p.get('synthetic',False))

@dataclass(frozen=True)
class _Binding:
    scope: tuple
    source_kind: str
    capability: str

class Host:
    def __init__(self,store,encoder,backend=None): self.store=store; self.encoder=encoder; self.backend=backend; self._issued={}
    def bind_from_authenticated_host(self,scope,source_kind='real'):
        if len(scope)!=3 or source_kind not in ('real','mock','imported_unverified'): raise ScopeViolation('invalid binding')
        b=_Binding(tuple(scope),source_kind,uuid.uuid4().hex); self._issued[b.capability]=b
        return API(self,b)
    def _check(self,b):
        if self._issued.get(b.capability) is not b: raise ScopeViolation('unissued binding')

class API:
    def __init__(self,host,binding): host._check(binding); self._host=host; self._binding=binding
    def _events(self,as_of):
        self._host._check(self._binding)
        return self._host.store.events(self._binding.scope,as_of,self._binding.source_kind)
    def _finish(self,task,raw,post,selected,*,as_of,token_budget=768,reason=None):
        with self._host.store.lock:
            return self._finish_locked(task,raw,post,selected,as_of=as_of,token_budget=token_budget,reason=reason)
    def _finish_locked(self,task,raw,post,selected,*,as_of,token_budget=768,reason=None):
        if type(token_budget) is not int or not 64<=token_budget<=2048: raise ValueError('token budget 64..2048')
        # A projection/model may finish after an event deletion or summary invalidation.
        allowed={e['event_id'] for e in self._events(as_of)}
        patterns={p['pattern_id'] for p in self._host.store.patterns(self._binding.scope,as_of,self._binding.source_kind)}
        def live(item):
            return (bool(item['evidence_refs']) and set(item['evidence_refs'])<=allowed
              and item['source_kind']==self._binding.source_kind
              and (not item['is_derived'] or item['memory_id'] in patterns))
        before=len(selected); raw=[r for r in raw if live(r)]; post=[r for r in post if live(r)]; selected=[r for r in selected if live(r)]
        # Budget includes the actual serialized evidence/metadata delivered to the caller.
        final=[]
        for item in selected:
            if self._host.encoder.tokens(serial(final+[item]))<=token_budget: final.append(item)
            else: break
        qid='mq-'+uuid.uuid4().hex
        result=dict(memory_query_id=qid,task_type=task,as_of=utc(as_of),raw_candidates=raw,
          post_filtered=post,memories=final,abstained=not bool(final),abstention_reason=reason if not final else None,
          token_count=self._host.encoder.tokens(serial(final)),token_budget=token_budget,
          stale_candidates_removed=before-len(selected),truncated=len(final)<len(selected),
          omitted_memory_ids=[r['memory_id'] for r in selected[len(final):]],
          retrieval_strategy_version='trusted-longitudinal-v1',model_version='BGE-small-zh-v1.5',
          returned_to_client=True,entered_model_context='unknown',generated_response='unknown',
          actually_presented_to_student='unknown',feedback_state='unknown',reward=None)
        # Persist returned content once; immutable candidate refs/scores suffice for
        # decision provenance. Full raw candidate text is an evaluation artifact.
        journal=dict({k:v for k,v in result.items() if k not in ('raw_candidates','post_filtered','memories')},
          journal_schema_version='memory-query-v1.1',trusted_scope=list(self._binding.scope),source_kind=self._binding.source_kind,
          candidates=[{k:r[k] for k in ('memory_id','score','source_kind','is_derived','evidence_refs')} for r in raw],
          candidate_memory_ids=[r['memory_id'] for r in raw],candidate_scores=[r.get('score') for r in raw],
          selected_memory_ids=[r['memory_id'] for r in final],returned_content=final,
          derived_pattern_ids=[r['memory_id'] for r in final if r['is_derived']],
          evidence_refs=sorted({i for r in final for i in r['evidence_refs']}))
        with self._host.store.db: self._host.store.db.execute('INSERT INTO queries VALUES (?,?)',(qid,serial(journal)))
        return result
    def search_memories(self,question,*,as_of,concept=None,diagnostic_type=None,top_k=8,token_budget=768,
                        event_types=None,revision=None,run_id=None,current_only=False,diagnostic_present=None,
                        require_mastery=False,require_independent=False,include_patterns=False):
        if type(top_k) is not int or not 1<=top_k<=20: raise ValueError('top_k 1..20')
        events=self._events(as_of); permitted={e['event_id']:e for e in events}
        routed=route(question); concept=concept or routed
        if self._host.backend:
            raw=self._host.backend(self._binding,question,as_of)
            # Recheck native projection output against authoritative liveness AND knowledge time.
            raw=[r for r in raw if set(r['evidence_refs'])<=set(permitted)]
        else:
            import numpy as np
            v=self._host.encoder.encode(question)
            ranked=sorted(((float(np.dot(v,self._host.encoder.encode(representation(e)))),e) for e in events),key=lambda x:(-x[0],x[1]['event_id']))[:20]
            raw=[event_item(e,s) for s,e in ranked]
        post=[]
        reject = require_mastery or require_independent or routed is None and concept is None
        # Even an explicit concept cannot override a clearly unrelated/credential query.
        reject=reject or any(w in question.lower() for w in ('password','密码','pll','phase noise','抖动'))
        if not reject:
            latest={}
            for e in events:
                key=e['issue_id']; latest[key]=max(latest.get(key,('', '')),(e['created_at'],e['revision']))
            for r in raw:
                refs=r['evidence_refs']; facts=[permitted[i] for i in refs]
                good=[]
                for e in facts:
                    if concept not in e['concept_ids']: continue
                    if diagnostic_type and diagnostic_type!=e['diagnostic_type']: continue
                    if event_types and e['event_type'] not in event_types: continue
                    if revision and e['revision']!=revision: continue
                    if run_id and e['run_id']!=run_id: continue
                    if diagnostic_present is not None and e['diagnostic_present']!=diagnostic_present: continue
                    if current_only and e['revision']!=latest[e['issue_id']][1]: continue
                    good.append(e)
                if good:
                    # One event representation on the new cohort. Cached multi-event cases are evaluated separately.
                    post.extend(event_item(e,r['score']) for e in good)
        unique={r['memory_id']:r for r in post}; post=sorted(unique.values(),key=lambda r:(-r['score'],r['memory_id']))
        chosen=post[:top_k]
        if include_patterns and not reject and not any((event_types,revision,run_id,current_only,diagnostic_present is not None)):
            pats=self._host.store.patterns(self._binding.scope,as_of,self._binding.source_kind,concept)
            chosen=[pattern_item(p) for p in pats]+chosen
        return self._finish('search_memories',raw,post,chosen,as_of=as_of,token_budget=token_budget,
          reason='unsupported mastery/independence claim or no trusted responsive evidence')
    def get_learning_timeline(self,*,as_of,concept=None,run_id=None,token_budget=768,limit=20,offset=0):
        if type(limit) is not int or not 1<=limit<=100: raise ValueError('limit 1..100')
        if type(offset) is not int or not 0<=offset<=10000: raise ValueError('offset 0..10000')
        events=[e for e in self._events(as_of) if (not concept or concept in e['concept_ids']) and (not run_id or run_id==e['run_id'])]
        rows=[event_item(e) for e in sorted(events,key=lambda e:(e['created_at'],e['event_id']))]
        result=self._finish('get_learning_timeline',rows,rows,rows[offset:offset+limit],as_of=as_of,token_budget=token_budget)
        result['has_more']=offset+len(result['memories'])<len(rows)
        result['next_offset']=offset+len(result['memories']) if result['has_more'] and result['memories'] else None
        return result
    def get_related_attempts(self,*,as_of,concept,diagnostic_type=None,token_budget=768):
        events=[e for e in self._events(as_of) if concept in e['concept_ids'] and (not diagnostic_type or e['diagnostic_type']==diagnostic_type)]
        groups={}
        for e in events: groups.setdefault(e['run_id'],[]).append(e)
        rows=[]
        for run,group in groups.items():
            last=max(group,key=lambda e:(e['created_at'],e['event_id'])); item=event_item(last)
            same_issue_runs=sorted(other for other,fs in groups.items() if other!=run
              and set(e['issue_id'] for e in fs)&set(e['issue_id'] for e in group))
            item.update(memory_id='attempt:'+run,evidence_refs=[e['event_id'] for e in group],
                summary=f"{run} {last['revision']}；{len(group)} 条事件；最后记录："+last['summary'],
                relationship='same_issue_trusted_identity' if same_issue_runs else 'semantic_association',
                same_issue_with_other_runs=same_issue_runs or 'unknown',
                identity_basis='trusted event issue_id equality' if same_issue_runs else 'unknown',fusion_allowed=False)
            rows.append(item)
        return self._finish('get_related_attempts',rows,rows,rows,as_of=as_of,token_budget=token_budget)
    def get_learning_patterns(self,*,as_of,concept=None,token_budget=768):
        self._events(as_of)
        rows=[pattern_item(p) for p in self._host.store.patterns(self._binding.scope,as_of,self._binding.source_kind,concept)]
        return self._finish('get_learning_patterns',rows,rows,rows,as_of=as_of,token_budget=token_budget)
