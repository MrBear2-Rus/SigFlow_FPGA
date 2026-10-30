"""Trusted U1 composition port; no HTTP routes, duplicate store or scheduler.

U1 authenticates callers and persists the external session/run-to-handle map.
The frozen run schema has no waiting state: pending teaching publishes cards
separately and never invents a terminal shared run.
"""
from copy import deepcopy
from dataclasses import asdict, dataclass
import hashlib
import json
import re
from urllib.parse import urlsplit

from ..context.models import EvidenceAnchor
from ..context.report import valid_anchor
from ..domain import Evidence, RunRequest
from ..pedagogy import valid_evidence
from .runtime import identifier, digest
from ..runtime.checkpoint import _decode as decode_readonly_checkpoint
from ..runtime.verification_checkpoint import decode_verification_checkpoint


def _version(value):
    if (type(value) is not str or re.fullmatch(r'0|[1-9][0-9]{0,18}',value) is None
            or int(value)>=2**63):
        raise ValueError('invalid_state_version')
    return int(value)


@dataclass(frozen=True)
class SharedRunContext:
    session_id: str
    run_id: str
    instance_id: str
    learner_id: str
    project_id: str
    revision: str
    snapshot_id: str
    job_id: str
    diagnostic_id: str
    issue_id: str

    def __post_init__(self):
        for key,value in asdict(self).items():
            identifier(value)
        if (re.fullmatch(r'sess-[1-9][0-9]{0,18}',self.session_id) is None
                or re.fullmatch(r'run-[1-9][0-9]{0,18}',self.run_id) is None):
            raise ValueError('invalid_shared_context')


@dataclass(frozen=True)
class TeachingHandle:
    context: SharedRunContext
    teaching_session_id: str
    kind: str


class SharedTeachingAdapter:
    def __init__(self,runtime,resolve_evidence,*,secrets=()):
        if not callable(resolve_evidence):
            raise ValueError('invalid_shared_adapter')
        self.runtime,self.resolve_evidence=runtime,resolve_evidence
        values=tuple(secrets)+tuple(getattr(runtime.factory,'model_secrets',()))
        if any(type(value) is not str for value in values):
            raise ValueError('invalid_shared_adapter')
        self.secrets=tuple(value for value in values if value)

    def start(self,context,question,*,idempotency_key,mode='rule',intent='diagnose',plan=None,gateway_plan_hash=None):
        if not isinstance(context,SharedRunContext):
            raise ValueError('invalid_shared_context')
        context.__post_init__()
        if context.instance_id!=self.runtime.instance_id:
            raise ValueError('scope_denied')
        identifier(idempotency_key)
        if intent not in ('explain','diagnose','verify'):
            raise ValueError('unsupported_request')
        body={key:getattr(context,key) for key in ('learner_id','project_id','revision','snapshot_id',
            'job_id','diagnostic_id','issue_id')}
        body.update(question=question,mode=mode,intent=intent)
        if plan is not None:body['plan']=deepcopy(plan)
        if gateway_plan_hash is not None:body['gateway_plan_hash']=gateway_plan_hash
        identity=[asdict(context),idempotency_key]
        key='shared-'+hashlib.sha256(json.dumps(identity,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        created=self.runtime.create(body,key)
        return TeachingHandle(context,created['session_id'],{'explain':'explain','diagnose':'report_review','verify':'plan'}[intent])

    def _state(self,handle):
        if not isinstance(handle,TeachingHandle) or not isinstance(handle.context,SharedRunContext):
            raise ValueError('invalid_shared_context')
        context=handle.context
        context.__post_init__()
        identifier(handle.teaching_session_id)
        if context.instance_id!=self.runtime.instance_id or handle.kind not in ('explain','report_review','plan'):
            raise ValueError('scope_denied')
        state=self.runtime.snapshot(handle.teaching_session_id,context.learner_id)
        if any(state.get(key)!=getattr(context,key) for key in
               ('instance_id','learner_id','project_id','revision','snapshot_id','job_id','issue_id')):
            raise ValueError('scope_denied')
        row=self.runtime.store.get(handle.teaching_session_id)
        if (row['binding']['diagnostic_id']!=context.diagnostic_id or handle.kind!=
                {'explain':'explain','diagnose':'report_review','verify':'plan'}[row['intent']]):
            raise ValueError('scope_denied')
        return state

    def _safe(self,value,field=None):
        if type(value) is str:
            if any(secret in value for secret in self.secrets):
                raise ValueError('unsafe_shared_output')
            if field=='url':
                location=urlsplit(value)
                if (location.scheme not in ('https','sigflow-note') or not location.netloc
                        or location.username or location.password or any(c.isspace() for c in value)):
                    raise ValueError('unsafe_shared_output')
                return
            if re.search(r'(?:[A-Za-z]:[\\/]|\\\\|(?<![\w:])/(?:[^\s/]+/))',value):
                raise ValueError('unsafe_shared_output')
            value.encode('utf-8')
        elif type(value) is dict:
            for key,item in value.items():self._safe(item,key)
        elif type(value) in (list,tuple):
            for item in value:self._safe(item,field)

    def _proofs(self,handle,state):
        row=self.runtime.store.get(handle.teaching_session_id)
        if row['state']['state_version']!=state['state_version']:
            raise ValueError('shared_projection_stale')
        try:
            if row['intent']=='verify':
                checkpoint,_=decode_verification_checkpoint(row['checkpoint'])
                proofs={state['job_id']:checkpoint['baseline_sha256']}
                proofs.update({item['job_id']:item['evidence_sha256'] for item in checkpoint['results']})
            else:
                checkpoint=decode_readonly_checkpoint(row['checkpoint'])
                proofs={state['job_id']:checkpoint['evidence_sha256']}
            if (checkpoint['binding']!=row['binding'] or checkpoint['state']['run_id']!=state['run_id']
                    or str(checkpoint['state']['version'])!=state['state_version']):
                raise ValueError()
        except Exception:
            raise ValueError('shared_evidence_unavailable') from None
        return proofs

    def _card(self,card,index,state,proofs):
        if (type(card) is not dict or type(card.get('level')) is not int or card['level'] not in (1,2,3)
                or card.get('producer') not in ('rule','model')
                or any(type(card.get(key)) is not str or not card[key].strip() for key in ('hint','question'))
                or type(card.get('evidence_ids')) not in (list,tuple) or not 1<=len(card['evidence_ids'])<=4):
            raise ValueError('invalid_shared_card')
        evidence=[]
        for eid in card['evidence_ids']:
            try:
                proof=self.resolve_evidence(eid)
                request=RunRequest(state['project_id'],state['revision'],state['issue_id'],'Validate the bound evidence')
                if (not isinstance(proof,Evidence) or not valid_evidence(proof,request)
                        or proof.evidence_id!=eid or digest(asdict(proof))!=proofs.get(proof.job_id)):
                    raise ValueError()
                anchor=proof.anchor
            except Exception:raise ValueError('shared_evidence_unavailable') from None
            if (not isinstance(anchor,EvidenceAnchor) or anchor.evidence_id!=eid
                    or not valid_anchor(anchor,state['project_id'],state['revision'],proof.job_id)
                    or anchor.snapshot_id!=state['snapshot_id']
                    or re.fullmatch(r'(?:sha256:)?[a-fA-F0-9]{64}',anchor.artifact_hash) is None):
                raise ValueError('shared_evidence_unavailable')
            ref={key:getattr(anchor,key) for key in ('evidence_id','project_id','revision','snapshot_id',
                'job_id','artifact_id','artifact_hash')}
            evidence.append(dict(kind='job_report',ref=ref,note='引用已由宿主证据端口核对；结论仅限其范围。'))
        courses=[]
        for ref in card.get('course_refs',()):
            courses.append(dict(course_id=ref['passage_id'],chapter=ref['section'],title=ref['title'],
                url=ref['source_url'],passage_id=ref['passage_id'],index_version=ref['index_version']))
        limitations=list(card.get('limitations',())) or ['仅依据所列证据，不代表整体设计正确或概念掌握。']
        if state['evidence_origin']=='fixture' and not any('离线' in item for item in limitations):
            limitations.append('离线示例证据，不是真实工程的 EDA 验证。')
        return dict(card_id=f'card-{index+1}',kind='teaching',level=f'L{card["level"]}',
            title=f'L{card["level"]} 教学提示',body=card['hint']+'\n\n'+card['question'],
            issue_id=state['issue_id'],source=card['producer'],evidence=evidence,course_refs=courses,
            limitations=limitations,expired=False,state_version=state['state_version'])

    def project(self,handle):
        state=self._state(handle)
        _version(state['state_version'])
        cards=[]
        omitted=[]
        if state['status'] in ('WaitingStudent','WaitingApproval','Completed'):
            original=state.get('cards',[])
            proofs=self._proofs(handle,state)
            for index,card in list(enumerate(original))[-16:]:
                cards.append(self._card(card,index,state,proofs))
            if len(original)>16:omitted.append(dict(kind='teaching_cards',reason='shared_card_limit'))
        status=state['status']
        phase='ready' if status=='Completed' else 'unavailable' if status in ('Failed','BudgetExceeded','Cancelled') else 'pending'
        run=None
        if status=='Completed' and cards:
            used=any(card['producer']=='model' for card in state.get('cards',[]))
            run=dict(run_id=handle.context.run_id,session_id=handle.context.session_id,
                kind=handle.kind,level=f'L{state["level"]}',state='succeeded' if used else 'degraded',
                state_version=state['state_version'],cards=cards,omitted=omitted,model_used=used)
        result=dict(schema_version='edu.u2.shared-projection.v1',session_id=handle.context.session_id,
            run_id=handle.context.run_id,state_version=state['state_version'],phase=phase,
            teaching_status=status,cards=cards,run=run,omitted=omitted)
        # These committed metadata ports make explicit prediction/observation
        # actions usable without publishing the private Runtime snapshot.
        prediction_keys={'status','reason','challenge_id','artifact_id','signal_id','start_tick','end_tick',
            'sample_tick','full_name','width','timescale','artifact_hash','project_id','revision',
            'snapshot_id','job_id','expires_at_epoch','expected','outcome','observed','origin','evidence_id','limitation'}
        prediction=state.get('prediction',dict(status='disabled'))
        if type(prediction) is not dict or set(prediction)-prediction_keys:
            raise ValueError('invalid_shared_prediction')
        result['prediction']=(deepcopy(prediction) if status in ('WaitingStudent','WaitingApproval','Completed')
            else dict(status='revalidation_required' if status=='RecoveryRequired' else 'unavailable'))
        if type(state.get('verification')) is dict:
            keys={'completed_steps','step_count','last_observed_step_index','job_id','job_state',
                  'result_evidence_ready','can_observe'}
            if set(state['verification'])-keys:raise ValueError('invalid_shared_verification')
            result['verification']=deepcopy(state['verification'])
        if status=='WaitingApproval' and state.get('plan_card') is not None:
            plan=deepcopy(state['plan_card'])
            if (plan.get('requires_approval') is not True or plan.get('state_version')!=state['state_version']
                    or any(plan.get(key)!=state[key] for key in ('project_id','revision','snapshot_id'))):
                raise ValueError('invalid_shared_plan')
            result['plan_card']=plan
        self._safe(result)
        # Resolving fresh external evidence must not make a stale card visible
        # after concurrent cancellation, recovery or version advancement.
        if self._state(handle)!=state:
            raise ValueError('shared_projection_stale')
        return result

    def action(self,handle,payload):
        self._state(handle)
        required={'kind','action_id','expected_state_version'}
        allowed={'hint_next':set(),'finish':set(),'refresh_evidence':set(),'observe_verification':set(),
            'execute_plan':{'grant_ref'},'recover':{'question'},
            'prediction_request':{'prediction'},'prediction_submit':{'prediction'}}
        if type(payload) is not dict or type(payload.get('kind')) is not str or payload['kind'] not in allowed:
            raise ValueError('action_denied')
        kind=payload['kind']
        if set(payload)!=required|allowed[kind]:
            raise ValueError('invalid_shared_action')
        version=_version(payload['expected_state_version'])
        sid,learner=handle.teaching_session_id,handle.context.learner_id
        if kind=='recover':
            result=self.runtime.recover(sid,learner,payload['action_id'],version,payload['question'])
        else:
            extra={key:deepcopy(payload[key]) for key in allowed[kind]}
            result=self.runtime.action(sid,learner,kind,payload['action_id'],version,**extra)
        return self._ack(handle,result)

    def _ack(self,handle,result):
        if type(result) is not dict or result.get('accepted') is not True:
            raise ValueError('invalid_shared_ack')
        # Runtime's replay result is an internal state projection, which has
        # not passed the public card boundary. Never forward it to a route.
        ack=dict(accepted=True,session_id=handle.context.session_id,run_id=handle.context.run_id,
            replay=result.get('replay') is True or result.get('status') in ('committed','interrupted'),
            pending=result.get('pending') is True or result.get('status')=='pending')
        self._safe(ack)
        return ack

    def cancel(self,handle):
        self._state(handle)
        self.runtime.cancel(handle.teaching_session_id,handle.context.learner_id)
        # Runtime cancellation returns a state acknowledgement, not an action
        # acceptance payload. Normalize only after it succeeds; no private state
        # or internal session ID crosses the shared boundary.
        return self._ack(handle,dict(accepted=True))

    def events(self,handle,after=0,limit=100):
        self._state(handle)
        return self.runtime.events(handle.teaching_session_id,handle.context.learner_id,after,limit)
