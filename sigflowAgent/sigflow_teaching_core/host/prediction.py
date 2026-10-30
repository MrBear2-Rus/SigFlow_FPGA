"""Trusted prediction action: metadata first, explicit expectation, then values."""
from copy import deepcopy
import re
import time
from uuid import uuid4

from ..clients.gateway import GatewayError,GatewayClient
from ..context.waveform import WaveformError,_tick,_signal_id,_bits,compare_sample
from ..domain import Status,Trace

KINDS=('prediction_request','prediction_submit')


def validate_payload(kind,payload):
    if type(payload) is not dict:raise ValueError('invalid_prediction')
    if kind=='prediction_request':
        if set(payload)!={'artifact_id','signal_id','start_tick','end_tick','sample_tick'}:
            raise ValueError('invalid_prediction')
        GatewayClient._identifier(payload['artifact_id'])
        if not _signal_id(payload['signal_id']):raise ValueError('invalid_prediction')
        start,end,point=(_tick(payload[k]) for k in ('start_tick','end_tick','sample_tick'))
        if not start<=point<=end:raise ValueError('invalid_prediction')
    elif kind=='prediction_submit':
        if (set(payload)!={'challenge_id','expected'} or type(payload['challenge_id']) is not str
                or re.fullmatch('[a-f0-9]{32}',payload['challenge_id']) is None
                or type(payload['expected']) is not str or not 1<=len(payload['expected'])<=8192):
            raise ValueError('invalid_prediction')
    else:raise ValueError('invalid_prediction')


class PredictionController:
    def __init__(self,reader,binding):
        self.reader,self.binding=reader,binding
        self._state=dict(status='idle');self._deadline=0

    def snapshot(self):return deepcopy(self._state)

    def invalidate(self,reason):
        self._state=dict(status='expired',reason=reason);self._deadline=0

    def begin(self,payload):
        validate_payload('prediction_request',payload)
        self.invalidate('new_request')
        description=self.reader.describe(self.binding,payload['artifact_id'])
        signal=next((s for s in description.signals if s.signal_id==payload['signal_id']),None)
        if (signal is None or not description.start_tick<=_tick(payload['start_tick'])<=
                _tick(payload['end_tick'])<=description.end_tick):raise ValueError('invalid_prediction')
        self._deadline=time.monotonic()+300
        self._state=dict(status='awaiting_prediction',challenge_id=uuid4().hex,**payload,
            full_name=signal.full_name,width=signal.width,timescale=description.timescale,
            artifact_hash=description.anchor.artifact_hash,project_id=self.binding.project_id,
            revision=self.binding.revision,snapshot_id=self.binding.snapshot_id,job_id=self.binding.job_id,
            expires_at_epoch=str(int(time.time()+300)))
        return self.snapshot()

    def validate_submit(self,payload):
        validate_payload('prediction_submit',payload)
        if (self._state.get('status')!='awaiting_prediction'
                or payload['challenge_id']!=self._state.get('challenge_id') or time.monotonic()>=self._deadline):
            raise ValueError('prediction_challenge_stale')
        expected=_bits(payload['expected'],self._state['width'])
        if any(c in 'xz' for c in expected):raise ValueError('invalid_prediction')
        return expected

    def submit(self,payload):
        expected=self.validate_submit(payload)
        # This explicit expectation is fixed before any transition query.
        self._state.update(status='observing',expected=expected)
        try:
            window=self.reader.read(self.binding,self._state['artifact_id'],
                signal_ids=(self._state['signal_id'],),start_tick=self._state['start_tick'],end_tick=self._state['end_tick'])
            if window.anchor.artifact_hash!=self._state['artifact_hash']:
                raise WaveformError('stale_waveform')
            result=compare_sample(window,self._state['signal_id'],self._state['sample_tick'],expected)
            self._state.update(status='observed',outcome=result.status,observed=result.observed,
                origin=result.origin,evidence_id=window.anchor.evidence_id,
                limitation='仅比较所选采样点，不能据此判定整个设计或复位契约正确。')
        except (WaveformError,GatewayError) as error:
            self._state.update(status='unavailable',reason=error.code)
        self._deadline=0
        return self.snapshot()


def precheck(session,kind,payload,reader):
    validate_payload(kind,payload)
    if reader is None:raise ValueError('prediction_unavailable')
    if session.state.status!=Status.WAITING_STUDENT:raise ValueError('action_denied')
    if session.state.steps>=session._loop.max_steps:raise ValueError('prediction_budget_exceeded')
    if kind=='prediction_submit':
        controller=getattr(session,'_prediction_controller',None)
        if controller is None:raise ValueError('prediction_challenge_stale')
        controller.validate_submit(payload)


def apply_prediction(session,binding,reader,kind,payload,expected_version):
    with session._lock:
        if session.state.version!=expected_version:raise ValueError('stale_state')
        controller=getattr(session,'_prediction_controller',None)
        if controller is None:
            controller=PredictionController(reader,binding);session._prediction_controller=controller
        try:
            # The challenge can expire after the HTTP reservation and before
            # the worker starts. Publish an unavailable observation, not a failed
            # teaching session, and perform no transition query in that case.
            precheck(session,kind,payload,reader)
            if kind=='prediction_request':controller.begin(payload)
            else:controller.submit(payload)
        except ValueError as error:
            reason=(error.code if isinstance(error,(WaveformError,GatewayError)) else 'invalid_prediction')
            if str(error) in ('prediction_challenge_stale','prediction_budget_exceeded','prediction_unavailable'):
                reason=str(error)
            controller._state=dict(status='unavailable',reason=reason)
        state=session._loop._state
        state.steps+=1;state.version+=1
        state.reason='prediction_unavailable' if controller.snapshot()['status']=='unavailable' else 'prediction_ready'
        state.trace.append(Trace(state.stage,state.status,state.reason,state.version))


def invalidate_prediction(session,reason):
    controller=getattr(session,'_prediction_controller',None)
    if controller is not None:controller.invalidate(reason)
