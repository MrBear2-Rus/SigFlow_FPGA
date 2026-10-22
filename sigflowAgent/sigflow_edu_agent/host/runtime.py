"""Bounded trusted host: only committed states cross the HTTP boundary."""

from dataclasses import asdict
import hashlib
import json
import threading
import time

from ..domain import RunRequest, Status
from ..runtime.gateway_session import SessionBinding
from ..runtime.checkpoint import restore_readonly_session
from ..runtime.verification_checkpoint import (decode_verification_checkpoint,
    export_verification_checkpoint, restore_verification_session, validate_binding, approval_data)
from ..checkers.plan import DetailedPlan, PlanStep, PlanChecker
from ..contracts.plan import build_plan_card
from .course_state import course_state, quarantined_course
from .prediction import KINDS as PREDICTION_KINDS,validate_payload,precheck,apply_prediction,invalidate_prediction
from .reference_actions import KINDS as REFERENCE_KINDS, ReferenceActions, project_reference
from .reference_actions import validate_payload as validate_reference_payload
from .teaching_memory import TeachingMemoryBridge, concept_ids


TERMINAL = {'Completed', 'Cancelled', 'Failed', 'BudgetExceeded'}


def digest(value):
    raw = json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':'), allow_nan=False)
    return hashlib.sha256(raw.encode('ascii')).hexdigest()


def identifier(value):
    import re
    if type(value) is not str or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}', value):
        raise ValueError('invalid_identifier')
    return value


def parse_plan(value):
    required = {'project_id', 'revision', 'snapshot_id', 'goal', 'steps', 'expected_observations'}
    if type(value) is not dict or not required <= set(value) or set(value) - required - {'max_jobs', 'stop_conditions'}:
        raise ValueError('invalid_plan')
    if type(value['steps']) is not list or not 1 <= len(value['steps']) <= 3:
        raise ValueError('invalid_plan')
    steps = []
    for step in value['steps']:
        keys = {'step_id', 'capability', 'provider', 'params'}
        if type(step) is not dict or not keys <= set(step) or set(step) - keys - {'dependencies'}:
            raise ValueError('invalid_plan')
        steps.append(PlanStep(**step))
    return PlanChecker().check(DetailedPlan(**(value | {'steps': tuple(steps)})))


class _ScopedVerification:
    """Recheck persistent cancellation after a blocking stage-local collection."""
    def __init__(self, port, cancelled):
        self.port, self.cancelled = port, cancelled
        self.supports_observation = callable(getattr(port, 'observe', None))

    def authorized(self, *args):
        return not self.cancelled() and self.port.authorized(*args)

    def execute(self, *args):
        if self.cancelled():
            raise ValueError('cancel_requested')
        return self.port.execute(*args)

    def observe(self, *args):
        if self.cancelled():
            raise ValueError('cancel_requested')
        return self.port.observe(*args)

    def bind_run(self, run_id):
        binder = getattr(self.port, 'bind_run', None)
        if callable(binder):
            self.port = binder(run_id)
        return self

    def __getattr__(self, name):
        return getattr(self.port, name)


class Runtime:
    def __init__(self, store, instance_id, factory, *, evidence_origin='tool', workers=4,
                 reference_provider=None, receipt_port=None, teaching_memory=None):
        identifier(instance_id)
        if evidence_origin not in ('tool', 'fixture') or type(workers) is not int or not 1 <= workers <= 8:
            raise ValueError('invalid_host_config')
        self.store, self.instance_id, self.factory = store, instance_id, factory
        self.origin = evidence_origin
        self._guard = threading.RLock()
        self._capacity = threading.BoundedSemaphore(workers)
        self._live, self._threads = {}, {}
        self._cancellers = {}
        self._closed = False
        if (reference_provider is None) != (receipt_port is None):
            raise ValueError('invalid_reference_config')
        self._references = (None if reference_provider is None else
            ReferenceActions(store, reference_provider, receipt_port, lambda: self._closed))
        self._teaching_memory=(None if teaching_memory is None else
            TeachingMemoryBridge(store,teaching_memory,secrets=getattr(factory,'model_secrets',())))

    @property
    def teaching_memory_configured(self):
        return self._teaching_memory is not None

    def _owned(self, sid, learner):
        identifier(sid)
        identifier(learner)
        row = self.store.get(sid)
        if row['binding']['instance_id'] != self.instance_id or row['binding']['learner_id'] != learner:
            raise ValueError('scope_denied')
        return row

    def snapshot(self, sid, learner):
        with self._guard:
            row = self._owned(sid, learner)
            return self._published(row)

    def _published(self, row):
        state = dict(row['state'])
        state['reference'] = project_reference(state, cancelled=row['cancel_requested'])
        state['event_high_watermark'] = str(row['event_high_watermark'])
        if state['status'] == 'RecoveryRequired' or row['cancel_requested']:
            state['cards'] = []
            state['course'] = quarantined_course('cancelled' if row['cancel_requested'] else 'revalidation_required')
            state['prediction'] = dict(status='cancelled' if row['cancel_requested'] else 'revalidation_required')
        if 'verification' in state and (row['session_id'] not in self._live or row['cancel_requested']):
            state['verification'] = dict(state['verification'], can_observe=False)
        return state

    def _replay(self, row, replay):
        result = replay.get('result')
        before_recovery = (type(result) is dict and
            int(result.get('state_version', '0')) < int(row['state'].get('recovery_version', '0')))
        if row['state']['status'] == 'RecoveryRequired' or row['cancel_requested'] or before_recovery:
            replay = dict(replay, result=self._published(row))
        elif (type(result) is dict and result.get('reference', {}).get('status') not in (None, 'idle', 'disabled')
              and (result.get('state_version') != row['state']['state_version']
                   or result['reference'] != project_reference(row['state']))):
            replay = dict(replay, result=self._published(row))
        return dict(accepted=True, session_id=row['session_id'], **replay)

    def events(self, sid, learner, after, limit=100):
        with self._guard:
            self._owned(sid, learner)
            return self.store.events(sid, after, limit)

    def reference_body(self, sid, learner, expected_version):
        """Trusted internal return only; never persist a body or consume again.

        Read durable metadata under the runtime guard, release it for provider
        I/O, then fence the result against cancellation, actions and recovery.
        """
        if type(expected_version) is not int or not 0 <= expected_version < 2**63:
            raise ValueError('invalid_state_version')

        def context():
            row = self._owned(sid, learner)
            if self._references is None:
                raise ValueError('receipt_unavailable')
            state = row['state']
            if (self._closed or sid not in self._live or row['cancel_requested']
                    or state['status'] != 'WaitingStudent' or state.get('level') != 3
                    or state.get('reference', {}).get('status') != 'confirmed'):
                raise ValueError('reference_body_unavailable')
            if int(state['state_version']) != expected_version:
                raise ValueError('stale_state')
            if self.store.has_pending(sid):
                raise ValueError('session_busy')
            live = self._live[sid]
            binding = live[1]
            if row['binding'] != asdict(binding):
                raise ValueError('reference_body_unavailable')
            record = self._references.journal.get(binding, state['run_id'])
            if record is None or record['phase'] != 'released':
                raise ValueError('reference_body_unavailable')
            metadata, challenge = state['reference'], record['challenge']
            review_hash = digest(sorted(metadata.get('reviewers', [])))
            if (metadata.get('reference_id') != challenge['reference_id']
                    or metadata.get('sha256') != challenge['reference_sha256']
                    or metadata.get('policy') != challenge['policy']
                    or review_hash != challenge['review_sha256']):
                raise ValueError('reference_body_unavailable')
            return live, state['run_id'], record, metadata

        # Workers commit without _guard. Keep each metadata snapshot atomic
        # against them while retaining existing store/journal validation.
        with self._guard, self.store._mutex:
            live, run_id, record, metadata = context()
            gate = self._references.gate(live[1], run_id)
        # DurableReferenceGate performs the same reviewed material validation
        # as confirmation/reconciliation, with fixed external-error redaction.
        item, _ = gate._material(record['challenge'])
        with self._guard, self.store._mutex:
            current, current_run, current_record, current_metadata = context()
            if (current is not live or current_run != run_id or current_record != record
                    or current_metadata != metadata):
                raise ValueError('reference_body_unavailable')
            return dict(reference_id=item.reference_id, sha256=item.sha256,
                        policy=metadata['policy'], reviewers=list(item.reviewers), content=item.content)

    def _wire(self, session, binding):
        state = session.state
        wire = dict(session_id=binding.session_id, instance_id=self.instance_id,
                    learner_id=binding.learner_id, project_id=binding.project_id,
                    revision=binding.revision, snapshot_id=binding.snapshot_id,
                    issue_id=binding.issue_id, job_id=binding.job_id,
                    run_id=state.run_id, stage=state.stage, status=str(state.status),
                    level=state.level, state_version=str(state.version), steps=state.steps,
                    model_calls=state.model_calls, cards=[dict(asdict(c), evidence_ids=list(c.evidence_ids),
                                                              limitations=list(c.limitations)) for c in state.cards],
                    reason=state.reason, evidence_origin=self.origin)
        wire['course'] = course_state(session._loop)
        evidence=session._loop._evidence
        wire['teaching_concept_ids']=concept_ids(evidence.concept) if evidence is not None else []
        from .repetition import project_observation, repetition_advice
        observation = project_observation(evidence, session._loop.request)
        wire['teaching_observation'] = observation
        wire['teaching_advice'] = repetition_advice(binding, observation,
            self.store.list_sessions() if observation and observation['eligible'] else ())
        controller=getattr(session,'_prediction_controller',None)
        wire['prediction']=(controller.snapshot() if controller is not None else
            dict(status='idle' if getattr(self.factory,'prediction_available',False) else 'disabled'))
        wire['reference'] = project_reference(dict(wire, reference=getattr(session, '_reference_state',
            dict(status='idle' if self._references is not None else 'disabled'))))
        if hasattr(session, '_recovery_version'):
            wire['recovery_version'] = str(session._recovery_version)
        wire['trace'] = [dict(stage=t.stage, status=str(t.status), reason=t.reason, state_version=str(t.version))
                         for t in state.trace]
        if state.status == Status.RECOVERY_REQUIRED:
            wire['cards'] = []
        plan = session._loop.plan
        if plan is not None:
            wire['plan_fingerprint'] = plan.fingerprint
            wire['plan_card'] = build_plan_card(plan, 'plan-1', state.version).card
            wire['verification'] = session._loop.verification_progress
        return wire

    def _new_session(self, binding, request, mode, plan=None, approval_hash=None):
        reserve = lambda: self.store.reserve_model(binding.session_id, 4)
        if plan is not None:
            session = self.factory(binding, request, mode, reserve, plan=plan, gateway_plan_hash=approval_hash)
            session._gateway_plan_hash = approval_hash
            session._loop.verification = _ScopedVerification(session._loop.verification,
                lambda: self._closed or self.store.cancel_requested(binding.session_id)).bind_run(session.state.run_id)
        else:
            session=self.factory(binding, request, mode, reserve)
        self._attach_history(session,binding)
        return session

    def _attach_history(self,session,binding):
        if self._teaching_memory is not None:
            bridge=self._teaching_memory
            session._loop.history_provider=lambda concepts:bridge.history(binding,concepts)

    def create(self, body, idempotency_key):
        identifier(idempotency_key)
        required = {'learner_id', 'project_id', 'revision', 'snapshot_id', 'job_id',
                    'diagnostic_id', 'issue_id', 'question'}
        if type(body) is not dict or not required <= set(body) or set(body) - required - {'intent', 'mode', 'plan', 'gateway_plan_hash'}:
            raise ValueError('invalid_request')
        mode, intent = body.get('mode', 'deepseek'), body.get('intent', 'diagnose')
        if mode not in ('rule', 'deepseek') or intent not in ('diagnose', 'explain', 'verify'):
            raise ValueError('unsupported_request')
        sid = 'session-' + digest([self.instance_id, idempotency_key])[:24]
        binding = SessionBinding(self.instance_id, sid, *(body[k] for k in
            ('learner_id', 'project_id', 'revision', 'snapshot_id', 'job_id', 'diagnostic_id', 'issue_id')))
        request = RunRequest(binding.project_id, binding.revision, binding.issue_id, body['question'], intent)
        if len(request.question) > 4096:
            raise ValueError('invalid_request')
        identifier(binding.learner_id)
        plan, approval_hash = None, None
        if intent == 'verify':
            plan, approval_hash = parse_plan(body.get('plan')), body.get('gateway_plan_hash')
            if ((plan.project_id, plan.revision, plan.snapshot_id) !=
                    (binding.project_id, binding.revision, binding.snapshot_id)
                    or type(approval_hash) is not str or not approval_hash.strip() or len(approval_hash) > 4096):
                raise ValueError('invalid_approval_binding')
        elif 'plan' in body or 'gateway_plan_hash' in body:
            raise ValueError('unexpected_plan')
        request_hash = digest(asdict(request) if plan is None else
                              dict(request=asdict(request), plan=plan.fingerprint, approval_hash=approval_hash))
        with self._guard:
            if self._closed:
                raise ValueError('host_closed')
            existing = next((r for r in self.store.list_sessions() if r['session_id'] == sid), None)
            if existing is not None:
                if (existing['binding'], existing['request_sha256'], existing['mode'], existing['intent']) != (
                        asdict(binding), request_hash, mode, intent):
                    raise ValueError('action_conflict')
                return dict(accepted=True, session_id=sid, replay=True)
            if any(r['binding']['project_id'] == binding.project_id and r['state']['status'] not in TERMINAL
                   for r in self.store.list_sessions()):
                raise ValueError('project_busy')
            if not self._capacity.acquire(False):
                raise ValueError('host_busy')
            try:
                session = self._new_session(binding, request, mode, plan, approval_hash)
                self.store.create(sid, asdict(binding), request_hash, mode, intent, self._wire(session, binding),
                                  initial_action=('create', digest(['create', request_hash])))
                self._live[sid] = (session, binding, request)
                self._start(sid, 'create', 'create', 0)
            except Exception:
                self._capacity.release()
                raise
            return dict(accepted=True, session_id=sid, replay=False)

    def action(self, sid, learner, kind, action_id, expected_version, *, grant_ref='',prediction=None,reference=None):
        identifier(action_id)
        if kind in REFERENCE_KINDS and self._references is None:
            with self._guard:self._owned(sid,learner)
            raise ValueError('receipt_unavailable')
        if type(expected_version) is not int or not 0 <= expected_version < 2**63:
            raise ValueError('invalid_state_version')
        if kind not in ('hint_next', 'finish', 'refresh_evidence', 'cancel', 'execute_plan', 'observe_verification',*PREDICTION_KINDS,*REFERENCE_KINDS):
            raise ValueError('action_denied')
        if kind in PREDICTION_KINDS:validate_payload(kind,prediction)
        elif prediction is not None:raise ValueError('invalid_prediction')
        if kind in REFERENCE_KINDS:validate_reference_payload(kind,reference)
        elif reference is not None:raise ValueError('invalid_reference')
        if grant_ref and kind != 'execute_plan':
            raise ValueError('unexpected_grant')
        if kind == 'execute_plan':
            identifier(grant_ref)
        fingerprint = digest([kind, expected_version, grant_ref])
        if kind in PREDICTION_KINDS:fingerprint=digest([kind,expected_version,prediction])
        if kind in REFERENCE_KINDS:fingerprint=digest([kind,expected_version,reference])
        with self._guard:
            row = self._owned(sid, learner)
            # Replays must be resolved before state/version checks.
            try:
                previous = self.store.action(sid, action_id)
            except ValueError:
                previous = None
            if previous is not None:
                replay = self.store.claim(sid, action_id, fingerprint, expected_version)
                return self._replay(row, replay)
            if self._closed or sid not in self._live or (row['state']['status'] == 'RecoveryRequired'
                                                       and kind not in ('observe_verification', 'cancel')):
                raise ValueError('recovery_required')
            state = row['state']
            if int(state['state_version']) != expected_version:
                raise ValueError('stale_state')
            allowed = ((kind in ('hint_next', 'finish') and state['status'] == 'WaitingStudent') or
                       (kind == 'refresh_evidence' and state['status'] == 'WaitingEvidence') or
                       (kind == 'execute_plan' and state['status'] == 'WaitingApproval') or
                       (kind == 'observe_verification' and state['status'] == 'RecoveryRequired'
                        and state.get('verification', {}).get('can_observe') is True) or
                       (kind in PREDICTION_KINDS and state['status']=='WaitingStudent') or
                       (kind in REFERENCE_KINDS and state['status']=='WaitingStudent') or
                       (kind == 'cancel' and state['status'] not in TERMINAL))
            if not allowed or (kind == 'hint_next' and state['level'] >= 3):
                raise ValueError('action_denied')
            if kind != 'cancel' and self.store.has_pending(sid):
                # Prechecks may read session.state, whose lock is held during
                # external operations. Never wait for that lock under _guard.
                raise ValueError('session_busy')
            if kind in PREDICTION_KINDS:
                precheck(self._live[sid][0],kind,prediction,getattr(self.factory,'waveform_reader',None))
            if kind in REFERENCE_KINDS:
                session, binding, _ = self._live[sid]
                self._references.precheck(session,binding,kind,reference,expected_version)
            if kind == 'cancel':
                # Both HTTP cancellation forms share the durable teaching stop
                # and owned-Job hook. Its reservation becomes interrupted, so a
                # duplicate replays the committed cancellation projection.
                self.store.claim(sid, action_id, fingerprint, expected_version,kind=kind)
                return dict(accepted=True, **self.cancel(sid, learner))
            if not self._capacity.acquire(False):
                raise ValueError('host_busy')
            try:
                self.store.claim(sid, action_id, fingerprint, expected_version,kind=kind)
                if kind in PREDICTION_KINDS:
                    self._start(sid,action_id,kind,expected_version,grant_ref,prediction)
                elif kind in REFERENCE_KINDS:
                    self._start(sid,action_id,kind,expected_version,grant_ref,None,dict(reference))
                else:self._start(sid, action_id, kind, expected_version, grant_ref)
            except Exception:
                self._capacity.release()
                raise
            return dict(accepted=True, session_id=sid, status='pending')

    def recover(self, sid, learner, action_id, expected_version, question):
        identifier(action_id)
        if type(expected_version) is not int:
            raise ValueError('invalid_state_version')
        with self._guard:
            row = self._owned(sid, learner)
            request = RunRequest(row['binding']['project_id'], row['binding']['revision'],
                                 row['binding']['issue_id'], question, row['intent'])
            checkpoint_data, plan = None, None
            if row['intent'] == 'verify':
                if not row['checkpoint']:
                    raise ValueError('recovery_not_available')
                checkpoint_data, plan = decode_verification_checkpoint(row['checkpoint'])
                validate_binding(checkpoint_data, SessionBinding(**row['binding']), row['request_sha256'],
                                 row['state'].get('plan_fingerprint'))
                request_hash = digest(dict(request=asdict(request), plan=plan.fingerprint,
                                           approval_hash=checkpoint_data['gateway_plan_hash']))
            else:
                request_hash = digest(asdict(request))
            if request_hash != row['request_sha256']:
                raise ValueError('request_mismatch')
            fingerprint = digest(['recover', expected_version, row['request_sha256']])
            try:
                previous = self.store.action(sid, action_id)
            except ValueError:
                previous = None
            if previous is not None:
                replay = self.store.claim(sid, action_id, fingerprint, expected_version)
                return self._replay(row, replay)
            if self._closed or row['state']['status'] != 'RecoveryRequired' or not row['checkpoint']:
                raise ValueError('recovery_not_available')
            if int(row['state']['state_version']) != expected_version:
                raise ValueError('stale_state')
            binding = SessionBinding(**row['binding'])
            if plan is not None:
                fresh = self._new_session(binding, request, row['mode'], plan, checkpoint_data['gateway_plan_hash'])
                session = restore_verification_session(checkpoint_data, binding=binding, request=request,
                                                        fresh=fresh, state_version=expected_version)
                session._loop.verification.bind_run(session.state.run_id)
                session._gateway_plan_hash = checkpoint_data['gateway_plan_hash']
            else:
                fresh = self._new_session(binding, request, row['mode'])
                session = restore_readonly_session(row['checkpoint'], binding=binding, request=request,
                                                   source=fresh._loop.source, provider=fresh._loop.provider,
                                                   course_provider=fresh._loop.course_provider,
                                                   state_version=expected_version)
            if session.state.version != expected_version:
                raise ValueError('recovery_version_mismatch')
            # Persist the publication barrier with the recovered state. Historic
            # actions remain idempotent but cannot replay pre-revalidation cards
            # or course metadata after recovery has completed.
            session._recovery_version = expected_version
            self._attach_history(session,binding)
            if not self._capacity.acquire(False):
                raise ValueError('host_busy')
            try:
                self.store.claim(sid, action_id, fingerprint, expected_version,kind='recover')
                self._live[sid] = (session, binding, request)
                self._start(sid, action_id, 'recover', expected_version)
            except Exception:
                self._capacity.release()
                raise
            return dict(accepted=True, session_id=sid, status='pending')

    def cancel(self, sid, learner):
        with self._guard:
            row = self._owned(sid, learner)
            if row['state']['status'] in TERMINAL:
                return dict(status=row['state']['status'], session_id=sid)
            if sid not in self._live and row['intent'] == 'verify' and row['checkpoint']:
                # Cancellation needs no question. Reconstruct the exact immutable
                # approval only; neither evidence collection nor execution occurs.
                data, plan = decode_verification_checkpoint(row['checkpoint'])
                binding = SessionBinding(**row['binding'])
                validate_binding(data, binding, row['request_sha256'], row['state'].get('plan_fingerprint'))
                request = RunRequest(binding.project_id, binding.revision, binding.issue_id,
                                     'checkpoint-cancellation', 'verify')
                session = self._new_session(binding, request, row['mode'], plan, data['gateway_plan_hash'])
                if approval_data(session._loop.verification, plan) != data['approval']:
                    raise ValueError('approval_binding_mismatch')
                session._loop._state.run_id = data['state']['run_id']
                session._loop.verification.bind_run(session.state.run_id)
                self._live[sid] = (session, binding, request)
            if sid in self._live:
                loop = self._live[sid][0]._loop
                fence = getattr(loop.verification, 'request_cancel', None)
                if loop.plan is not None and callable(fence):
                    fence(loop.plan)  # Disk-only fence before acknowledging teaching cancellation.
            self.store.cancel_session(sid)
            if self._references is not None:
                self._references.journal.invalidate(SessionBinding(**row['binding']), row['state']['run_id'])
                if sid in self._live:
                    # Do not read session.state here: an external operation can
                    # hold its lock. The committed row already provides scope.
                    self._live[sid][0]._reference_state = dict(status='cancelled')
            scheduled = False
            if sid in self._live and sid not in self._cancellers:
                loop = self._live[sid][0]._loop
                callback = getattr(loop.verification, 'cancel_owned', None)
                if loop.plan is not None and callable(callback):
                    def cancel_job():
                        try:
                            callback(loop.plan)
                        except Exception:
                            pass  # Job status remains a Gateway fact, never inferred from ACK.
                    thread = threading.Thread(target=cancel_job, daemon=True)
                    self._cancellers[sid] = thread
                    thread.start()
                    scheduled = True
            return dict(status='cancel_requested', session_id=sid, teaching_status='Cancelled',
                        eda_cancel_scheduled=scheduled)

    def _commit_cancel(self, sid, row):
        if row['state']['status'] in TERMINAL:
            return
        self.store.cancel_session(sid)

    def _start(self, sid, action_id, kind, expected_version, grant_ref='',prediction=None,reference=None):
        thread = threading.Thread(target=self._work, args=(sid, action_id, kind, expected_version, grant_ref,prediction,reference), daemon=True)
        self._threads[sid] = thread
        thread.start()

    def _work(self, sid, action_id, kind, expected_version, grant_ref,prediction=None,reference=None):
        session, binding, request = self._live[sid]
        try:
            reference_draft = None
            if self._references is not None and kind not in (*REFERENCE_KINDS, 'recover', 'create'):
                self._references.invalidate(session, binding)
            if kind == 'recover':
                session.reconcile(expected_version)
                if self._references is not None:
                    self._references.recovered(session, binding)
            elif kind in REFERENCE_KINDS:
                reference_draft = self._references.apply(session,binding,kind,reference,expected_version)
                invalidate_prediction(session,'context_changed')
            elif kind in PREDICTION_KINDS:
                apply_prediction(session,binding,self.factory.waveform_reader,kind,prediction,expected_version)
            elif kind != 'create':
                session.host_action(kind, action_id, expected_version, grant_ref=grant_ref)
                invalidate_prediction(session,'context_changed')
            while session.state.status == Status.RUNNING and not self._closed and not self.store.cancel_requested(sid):
                if request.intent == 'verify' and session.state.stage == 'E6':
                    checkpoint = export_verification_checkpoint(session._loop, binding, session._gateway_plan_hash)
                    self.store.stage_verification(sid, action_id, self._wire(session, binding), checkpoint)
                session.advance(session.state.version)
            wire = self._wire(session, binding)
            checkpoint = None
            if self._closed or self.store.cancel_requested(sid):
                wire.update(status='Cancelled', cards=[], reason='cancelled_by_user',
                            state_version=str(int(wire['state_version']) + 1))
            elif kind == 'recover' and session.state.status == Status.RECOVERY_REQUIRED:
                checkpoint = self.store.get(sid)['checkpoint']
                if request.intent == 'verify' and session._loop._evidence is not None:
                    checkpoint = export_verification_checkpoint(session._loop, binding, session._gateway_plan_hash)
            elif request.intent == 'verify' and session._loop._evidence is not None:
                checkpoint = export_verification_checkpoint(session._loop, binding, session._gateway_plan_hash)
            elif request.intent != 'verify' and session.state.status in (Status.WAITING_STUDENT, Status.COMPLETED):
                checkpoint = session.checkpoint(binding)
            if reference_draft is not None:
                self._references.journal.commit_request(binding, session.state.run_id, reference_draft,
                                                        action_id, wire, checkpoint)
            else:
                self.store.commit(sid, action_id, wire, checkpoint, wire)
        except Exception:
            # Uncommitted candidates never reach GET state or an action replay.
            try:
                wire = self._wire(session, binding)
                cancelled = self.store.cancel_requested(sid)
                wire.update(status='Cancelled' if cancelled else 'Failed', cards=[],
                            reason='cancelled_by_user' if cancelled else 'host_operation_failed',
                            state_version=str(int(wire['state_version']) + 1))
                wire['reference'] = project_reference(wire, cancelled=cancelled)
                self.store.commit(sid, action_id, wire, None, wire)
            except Exception:
                pass  # Store remains pending/unavailable; recovery cannot rerun it.
            with self._guard:
                self._live.pop(sid, None)
        finally:
            with self._guard:
                if self._threads.get(sid) is threading.current_thread():
                    self._threads.pop(sid, None)
                    try:
                        row = self.store.get(sid)
                        if row['cancel_requested']:
                            self._commit_cancel(sid, row)
                    except ValueError:
                        pass
            self._capacity.release()

    def close(self, timeout=2):
        with self._guard:
            self._closed = True
            threads = list(self._threads.values()) + list(self._cancellers.values())
        deadline = time.monotonic() + timeout
        for thread in threads:
            thread.join(max(0, deadline - time.monotonic()))
        if self._teaching_memory is not None:
            self._teaching_memory.close(max(0,deadline-time.monotonic()))
