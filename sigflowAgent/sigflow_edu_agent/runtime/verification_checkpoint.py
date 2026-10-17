"""Credential-free verification recovery. Stored plans never confer execution authority.

The host owns this local journal. Digests detect changed bindings/observations;
they do not authenticate a maliciously replaced database or grant permissions.
No question, model output, raw report, grant or token is serialized.
"""
from dataclasses import asdict
import json
import re

from ..contracts.plan import build_plan_card
from ..domain import CardContext, RunState, Status, Trace, VerificationResult
from ..loop import TeachingLoop
from ..patterns import PATTERN_VERSION, POLICY_VERSION
from ..pedagogy import RuleProvider, checked_card, valid_evidence
from .checkpoint import (_canonical, _digest, _duplicates, _integer, _keys, _text,
                         _bound_evidence, _FixedObservationSource)
from .gateway_session import SessionBinding
from .session import TeachingSession

SCHEMA = 'verification-checkpoint.v1'
VERSIONS = dict(workflow='u1-verification-recovery.v1', policy=POLICY_VERSION, patterns=PATTERN_VERSION)
MAX_BYTES = 64 * 1024
_PENDING = ('verification_status_pending', 'verification_evidence_pending')


def plan_data(plan):
    return build_plan_card(plan, 'plan-1', 0).binding['plan']


def approval_data(port, plan):
    approved = getattr(port, 'approval', None)
    package = approved.package if approved is not None else build_plan_card(plan, 'plan-1', 0)
    return dict(plan_id=package.binding['plan_id'], state_version=package.binding['state_version'],
                digest=package.digest)


def _invalid():
    raise ValueError('invalid_verification_checkpoint')


def decode_verification_checkpoint(payload):
    """Strict bounded decode, with no network, models or execution."""
    from ..host.runtime import parse_plan
    try:
        if type(payload) is not str or len(payload.encode('utf-8')) > MAX_BYTES:
            _invalid()
        data = json.loads(payload, object_pairs_hook=_duplicates, parse_constant=lambda _: _invalid())
        _keys(data, 'schema versions binding request_sha256 request_binding_sha256 plan plan_sha256 '
                    'gateway_plan_hash approval budgets state baseline_sha256 results index pending last')
        if data['schema'] != SCHEMA or data['versions'] != VERSIONS:
            _invalid()
        SessionBinding(**data['binding'])
        plan = parse_plan(data['plan'])
        if plan.fingerprint != data['plan_sha256']:
            _invalid()
        if (plan.project_id, plan.revision, plan.snapshot_id) != tuple(data['binding'][k] for k in
                ('project_id', 'revision', 'snapshot_id')):
            _invalid()
        for name in ('request_sha256', 'request_binding_sha256', 'baseline_sha256', 'plan_sha256'):
            if type(data[name]) is not str or re.fullmatch('[a-f0-9]{64}', data[name]) is None:
                _invalid()
        _text(data['gateway_plan_hash'], 4096)
        approval = data['approval']
        _keys(approval, 'plan_id state_version digest')
        if (type(approval['state_version']) is not str or
                re.fullmatch('0|[1-9][0-9]{0,18}', approval['state_version']) is None):
            _invalid()
        _integer(int(approval['state_version']), 0, 2**63 - 3)
        expected = build_plan_card(plan, approval['plan_id'], int(approval['state_version']))
        if approval['digest'] != expected.digest:
            _invalid()
        budgets, state = data['budgets'], data['state']
        _keys(budgets, 'max_steps max_model_calls')
        _integer(budgets['max_steps'], 1, 1000)
        _integer(budgets['max_model_calls'], 0, 100)
        _keys(state, 'run_id stage status level version steps model_calls reason')
        if type(state['run_id']) is not str or re.fullmatch('[a-f0-9]{32}', state['run_id']) is None:
            _invalid()
        if state['stage'] not in ('E5', 'E6', 'E7') or state['status'] not in (
                'Running', 'WaitingApproval', 'RecoveryRequired', 'WaitingEvidence',
                'Completed', 'Failed', 'BudgetExceeded'):
            _invalid()
        _integer(state['version'], 5, 2**63 - 3)
        _integer(state['steps'], 5, budgets['max_steps'])
        _integer(state['model_calls'], 0, budgets['max_model_calls'])
        _integer(state['level'], 1, 3)
        if type(state['reason']) is not str or re.fullmatch('[a-z0-9_]{0,128}', state['reason']) is None:
            _invalid()
        _integer(data['index'], 0, len(plan.steps))
        if type(data['pending']) is not bool or data['pending'] and data['index'] >= len(plan.steps):
            _invalid()
        if type(data['results']) is not list or len(data['results']) != data['index']:
            _invalid()
        for result in data['results']:
            _keys(result, 'evidence_sha256 job_id matches_expected')
            if (type(result['evidence_sha256']) is not str or
                    re.fullmatch('[a-f0-9]{64}', result['evidence_sha256']) is None):
                _invalid()
            if type(result['job_id']) is not str or re.fullmatch('[A-Za-z0-9_-]{1,128}', result['job_id']) is None:
                _invalid()
            if result['matches_expected'] is not None and type(result['matches_expected']) is not bool:
                _invalid()
        last = data['last']
        if last is not None:
            _keys(last, 'state job_id')
            if last['state'] not in ('Created', 'Validating', 'Queued', 'Running', 'ValidatingArtifact',
                    'Cancelling', 'Succeeded', 'Unknown', 'Failed', 'Cancelled', 'TimedOut', 'Rejected'):
                _invalid()
            if last['job_id'] is not None and (type(last['job_id']) is not str or
                    re.fullmatch('[A-Za-z0-9_-]{1,128}', last['job_id']) is None):
                _invalid()
        if state['status'] == 'Running' and not data['pending'] and state['stage'] != 'E7':
            _invalid()
        if state['status'] == 'WaitingApproval' and (data['pending'] or data['index'] >= len(plan.steps)):
            _invalid()
        return data, plan
    except (ValueError, TypeError, KeyError, AttributeError, UnicodeError, RecursionError, OverflowError):
        _invalid()


def export_verification_checkpoint(loop, binding, gateway_plan_hash):
    state = loop.state
    if (loop.request.intent != 'verify' or not _bound_evidence(loop._evidence, binding, loop.request)
            or loop._baseline_evidence is not None and loop._baseline_evidence != loop._evidence):
        raise ValueError('checkpoint_not_available')
    data = dict(schema=SCHEMA, versions=VERSIONS, binding=asdict(binding),
        request_sha256=_digest(asdict(loop.request)),
        request_binding_sha256=_digest(dict(request=asdict(loop.request), plan=loop.plan.fingerprint,
                                            approval_hash=gateway_plan_hash)),
        plan=plan_data(loop.plan), plan_sha256=loop.plan.fingerprint, gateway_plan_hash=gateway_plan_hash,
        approval=approval_data(loop.verification, loop.plan),
        budgets=dict(max_steps=loop.max_steps, max_model_calls=loop.max_model_calls),
        state={name: getattr(state, name) for name in
               ('run_id', 'stage', 'status', 'level', 'version', 'steps', 'model_calls', 'reason')},
        baseline_sha256=_digest(asdict(loop._evidence)),
        results=[dict(evidence_sha256=_digest(asdict(r.evidence)), job_id=r.evidence.job_id,
                      matches_expected=r.matches_expected) for r in loop._results],
        index=loop._job_index,
        pending=(state.stage == 'E6' and state.status == Status.RUNNING or
                 state.status == Status.RECOVERY_REQUIRED and state.reason in _PENDING),
        last=None)
    progress = loop.verification_progress
    if progress['job_state'] is not None:
        data['last'] = dict(state=progress['job_state'], job_id=progress['job_id'])
    payload = _canonical(data)
    decode_verification_checkpoint(payload)
    return payload


def validate_binding(data, binding, request_hash, plan_fingerprint):
    if (data['binding'] != asdict(binding) or data['request_binding_sha256'] != request_hash
            or data['plan_sha256'] != plan_fingerprint):
        raise ValueError('checkpoint_scope_mismatch')


class RecoveredVerificationSession(TeachingSession):
    def __init__(self, loop, binding, saved):
        super().__init__(loop)
        self.binding, self._saved = binding, saved

    def reconcile(self, expected_version):
        """Revalidate all completed evidence before observing the current receipt."""
        with self._lock:
            loop, saved = self._loop, self._saved
            state = loop._state
            if state.version != expected_version or state.status != Status.RECOVERY_REQUIRED:
                raise ValueError('stale_state')
            try:
                evidence = loop.source.collect(self.binding.project_id, self.binding.revision)
                if not _bound_evidence(evidence, self.binding, loop.request):
                    raise ValueError('recovery_evidence_mismatch')
                results = []
                for index, result in enumerate(saved['results']):
                    key = f'{state.run_id}:{loop.plan.fingerprint}:{index}'
                    fresh = loop.verification.observe(loop.plan, index, key)
                    if (not isinstance(fresh, VerificationResult) or fresh.state != 'Succeeded'
                            or not valid_evidence(fresh.evidence, loop.request)
                            or fresh.evidence.job_id != result['job_id']
                            or fresh.job_id not in (None, result['job_id'])
                            or _digest(asdict(fresh.evidence)) != result['evidence_sha256']
                            or fresh.matches_expected != result['matches_expected']):
                        raise ValueError('recovery_evidence_mismatch')
                    results.append(fresh)
            except Exception:
                state.cards = []
                state.reason = 'recovery_evidence_mismatch'
            else:
                loop._evidence, loop._results = evidence, results
                loop._prepare_courses()
                state.cards = [checked_card(RuleProvider().generate(CardContext(loop.request.question, state.level,
                    evidence)), CardContext(loop.request.question, state.level, evidence), 'rule')]
                if saved['pending']:
                    # A submission may or may not exist. observe validates the exact ledger
                    # entry and fails closed if the process died before reservation.
                    loop._observation_only = True
                    state.stage, state.status, state.reason = 'E6', Status.RUNNING, ''
                elif saved['state']['stage'] == 'E7' and len(results) == len(loop.plan.steps):
                    state.stage, state.status, state.reason = 'E7', Status.RUNNING, ''
                elif saved['state']['status'] == 'WaitingEvidence':
                    loop._needs_replan = True
                    state.stage, state.status, state.reason = 'E7', Status.WAITING_EVIDENCE, saved['state']['reason']
                elif results and results[-1].matches_expected is False and 'observation_mismatch' in loop.plan.stop_conditions:
                    loop._needs_replan = True
                    state.stage, state.status, state.reason = 'E7', Status.WAITING_EVIDENCE, 'recollect_and_replan'
                else:
                    state.stage, state.status = 'E5', Status.WAITING_APPROVAL
                    state.reason = 'successor_approval_required' if results else ''
            state.version += 1
            state.trace.append(Trace(state.stage, state.status, state.reason, state.version))
            return self.state


def restore_verification_session(data, *, binding, request, fresh, state_version):
    if data['request_sha256'] != _digest(asdict(request)):
        raise ValueError('request_mismatch')
    if approval_data(fresh._loop.verification, fresh._loop.plan) != data['approval']:
        raise ValueError('approval_binding_mismatch')
    saved = data['state']
    if type(state_version) is not int or not saved['version'] <= state_version <= 2**63 - 3:
        raise ValueError('invalid_recovery_version')
    source = _FixedObservationSource(fresh._loop.source, binding, request, data['baseline_sha256'])
    loop = TeachingLoop(request, source, fresh._loop.provider, plan=fresh._loop.plan,
        verification=fresh._loop.verification, course_provider=fresh._loop.course_provider, **data['budgets'])
    loop._state = RunState(saved['run_id'], stage=saved['stage'], status=Status.RECOVERY_REQUIRED,
        level=saved['level'], version=state_version, steps=saved['steps'], model_calls=saved['model_calls'],
        reason='recovery_required')
    loop._job_index = data['index']
    if data['last'] is not None:
        loop._last_verification = VerificationResult(data['last']['state'], None, None, data['last']['job_id'])
        loop._last_verification_index = (data['index'] if data['pending'] else
                                         max(0, data['index'] - 1))
    return RecoveredVerificationSession(loop, binding, data)
