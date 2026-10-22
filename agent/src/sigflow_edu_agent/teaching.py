"""Trusted U1 composition facade, with no parallel teaching workflow."""
from dataclasses import asdict
from functools import wraps
from threading import Event

from .errors import AgentError
from .teaching_store import identifier,fingerprint

SCHEMA='u1.u2-teaching.v1'


def public(operation):
    @wraps(operation)
    def call(*args,**kwargs):
        try:return operation(*args,**kwargs)
        except AgentError:raise
        except ValueError as exc:
            code=str(exc)
            if code in ('scope_denied','action_denied','approval_denied','receipt_unavailable'):
                name,status='POLICY_DENIED',403
            elif code in ('not_found','session_not_found'):name,status='NOT_FOUND',404
            elif code in ('cursor_expired','cursor_stale'):name,status='CURSOR_EXPIRED',410
            elif code in ('session_limit','action_limit','budget_exceeded'):name,status='RESOURCE_EXHAUSTED',429
            elif code in ('stale_state','action_conflict','project_busy','host_busy','operation_pending',
                          'session_busy','recovery_required','recovery_not_available','shared_projection_stale'):
                name,status='IDEMPOTENCY_CONFLICT',409
            elif code in ('shared_evidence_unavailable','storage_error','store_closed','store_unavailable',
                          'state_corrupt','checkpoint_corrupt','host_closed'):
                name,status='SERVICE_UNAVAILABLE',503
            else:name,status='INVALID_ARGUMENT',400
            raise AgentError(name,'teaching operation rejected',http_status=status) from None
        except Exception:
            raise AgentError('SERVICE_UNAVAILABLE','teaching service unavailable',http_status=503) from None
    return call


class TeachingService:
    def __init__(self,runtime,bindings,prepare,adapter_factory,check_current,*,resource_closers=()):
        self.runtime,self.bindings=runtime,bindings
        self.prepare,self.adapters,self.check_current=prepare,adapter_factory,check_current
        self._closed=Event()
        self._closers=tuple(resource_closers)

    def _available(self):
        if self._closed.is_set():raise ValueError('host_closed')

    def _current(self,session):
        self._available()
        self.check_current(session)
        self._available()

    @public
    def create_session(self,learner,body,key):
        if type(body) is not dict or set(body)!={'project_id','revision'}:
            raise ValueError('invalid_request')
        for value in (learner,body['project_id'],body['revision'],key):identifier(value)
        self._current(dict(learner_id=learner,**body))
        view=self.bindings.create_session(learner,body['project_id'],body['revision'],key)
        self._current(view)
        return dict(schema_version=SCHEMA,**view)

    @public
    def session(self,sid,learner):
        self._available()
        view=self.bindings.session(sid,learner)
        self._current(view)
        return dict(schema_version=SCHEMA,**view)

    @public
    def start(self,sid,learner,body,key):
        from sigflow_u2.host.shared_adapter import SharedRunContext
        from sigflow_u2.host.runtime import parse_plan
        self._available()
        required={'question','job_id','issue_id'}
        allowed=required|{'snapshot_id','diagnostic_id','intent','mode','plan','gateway_plan_hash'}
        if type(body) is not dict or not required<=set(body) or set(body)-allowed:
            raise ValueError('invalid_request')
        question=body['question']
        if type(question) is not str or not question.strip() or len(question)>4096:
            raise ValueError('invalid_request')
        question.encode('utf-8')
        for name in ('job_id','issue_id','snapshot_id','diagnostic_id'):
            if name in body:identifier(body[name])
        intent,mode=body.get('intent','diagnose'),body.get('mode','rule')
        if intent not in ('explain','diagnose','verify') or mode not in ('rule','deepseek'):
            raise ValueError('invalid_request')
        if intent=='verify':
            parse_plan(body.get('plan'))
            identifier(body.get('gateway_plan_hash'))
        elif 'plan' in body or 'gateway_plan_hash' in body:raise ValueError('unexpected_plan')
        session=self.bindings.session(sid,learner)
        self._current(session)
        prepared=self.prepare(session,body)
        if type(prepared) is not dict or set(prepared)!={'job_id','snapshot_id','diagnostic_id','issue_id'}:
            raise ValueError('invalid_request')
        if any(prepared[k]!=body[k] for k in prepared if k in body):raise ValueError('scope_denied')
        if intent=='verify' and any(body['plan'].get(k)!=v for k,v in dict(
                project_id=session['project_id'],revision=session['revision'],snapshot_id=prepared['snapshot_id']).items()):
            raise ValueError('scope_denied')
        self._current(session)
        record=self.bindings.reserve_run(sid,learner,key,fingerprint(body),dict(prepared,intent=intent,mode=mode))
        context=SharedRunContext(sid,record['run_id'],self.runtime.instance_id,learner,
            session['project_id'],session['revision'],prepared['snapshot_id'],prepared['job_id'],
            prepared['diagnostic_id'],prepared['issue_id'])
        adapter=self.adapters(context,None)
        handle=adapter.start(context,question,idempotency_key='u1-create',mode=mode,intent=intent,
            plan=body.get('plan'),gateway_plan_hash=body.get('gateway_plan_hash'))
        self.bindings.attach(record['run_id'],learner,asdict(handle))
        self._available()
        return dict(schema_version=SCHEMA,accepted=True,session_id=sid,run_id=record['run_id'])

    def _owned(self,rid,learner,current=True):
        from sigflow_u2.host.shared_adapter import SharedRunContext,TeachingHandle
        self._available()
        record=self.bindings.run(rid,learner)
        if current:self._current(record['session'])
        raw=record['handle']
        if raw is None:return record,None,None
        handle=TeachingHandle(SharedRunContext(**raw['context']),raw['teaching_session_id'],raw['kind'])
        return record,handle,self.adapters(handle.context,handle.teaching_session_id)

    @public
    def project(self,rid,learner):
        record,handle,adapter=self._owned(rid,learner)
        if handle is None:
            return dict(schema_version=SCHEMA,session_id=record['session']['session_id'],run_id=rid,
                phase='pending',teaching_status='StartRequiresRetry',state_version='0',cards=[],run=None,omitted=[])
        view=adapter.project(handle)
        self._current(record['session'])
        return dict(view,schema_version=SCHEMA)

    @public
    def action(self,rid,learner,payload):
        record,handle,adapter=self._owned(rid,learner)
        if handle is None:raise ValueError('operation_pending')
        result=adapter.action(handle,payload)
        self._available()
        return dict(result,schema_version=SCHEMA)

    @public
    def cancel(self,rid,learner):
        # Ownership survives a source edit or Gateway outage; cancellation must
        # remain available for the old bound Job and not require current I/O.
        record,handle,adapter=self._owned(rid,learner,current=False)
        if handle is None:raise ValueError('operation_pending')
        return dict(adapter.cancel(handle),schema_version=SCHEMA)

    @public
    def events(self,rid,learner,after=0,limit=100):
        record,handle,adapter=self._owned(rid,learner)
        if handle is None:raise ValueError('operation_pending')
        raw=adapter.events(handle,after,limit)
        events=[dict(sequence=str(row['sequence']),action_id=row['action_id'],
            state_version=row['state_version'],teaching_status=row['status']) for row in raw['events']]
        self._current(record['session'])
        return dict(schema_version=SCHEMA,session_id=record['session']['session_id'],run_id=rid,
            events=events,high_watermark=str(raw['high_watermark']))

    def close(self):
        if self._closed.is_set():return
        self._closed.set()
        failure=None
        for close in (lambda:self.runtime.close(timeout=2),self.bindings.close)+self._closers:
            try:close()
            except Exception as exc:
                if failure is None:failure=exc
        if failure is not None:raise failure
