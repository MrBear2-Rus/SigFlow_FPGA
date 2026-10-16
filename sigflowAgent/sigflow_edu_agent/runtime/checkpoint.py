"""Trusted-host read-only pause checkpoints, not a persistence/auth service.

No Provider, request/evidence body, grant or executable plan is serialized.
Card bodies are teaching records and require host access/retention controls.
The host must commit checkpoints with actions and reserve model budgets before
calls. A digest detects a changed observation; it is not a signature or receipt.
"""

from dataclasses import asdict
import hashlib
import json
import re

from ..domain import CardContext, EvidenceError, RunRequest, RunState, Status, Trace
from ..loop import TeachingLoop
from ..patterns import PATTERN_VERSION, POLICY_VERSION
from ..pedagogy import checked_card, valid_evidence, RuleProvider
from .gateway_session import Occurrence, SessionBinding
from .session import TeachingSession


SCHEMA = 'readonly-checkpoint.v1'
VERSIONS = {'workflow': 'u2-readonly-recovery.v1', 'policy': POLICY_VERSION,
            'patterns': PATTERN_VERSION}
MAX_BYTES = 128 * 1024


def _invalid():
    raise ValueError('invalid_checkpoint')


def _keys(value, names):
    if type(value) is not dict or set(value) != set(names.split()):
        _invalid()


def _integer(value, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        _invalid()


def _text(value, maximum):
    if type(value) is not str or not value.strip() or len(value) > maximum:
        _invalid()
    try:
        value.encode('utf-8')
    except UnicodeError:
        _invalid()


def _canonical(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':'),
                      allow_nan=False)


def _digest(value):
    return hashlib.sha256(_canonical(value).encode('ascii')).hexdigest()


def _scope(binding, request):
    if not isinstance(binding, SessionBinding) or not isinstance(request, RunRequest):
        raise ValueError('checkpoint_scope_mismatch')
    if (request.project_id, request.revision, request.issue_id) != (
            binding.project_id, binding.revision, binding.issue_id):
        raise ValueError('checkpoint_scope_mismatch')
    if request.intent not in ('explain', 'diagnose'):
        raise ValueError('checkpoint_not_available')
    # Request text is hashed only, and must be valid UTF-8 before hashing.
    _text(request.question, 4096)


def _bound_evidence(evidence, binding, request):
    if not valid_evidence(evidence, request) or evidence.job_id != binding.job_id:
        return False
    if evidence.anchor is None:
        return evidence.origin == 'fixture'
    return evidence.anchor.snapshot_id == binding.snapshot_id


def _duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            _invalid()
        result[key] = value
    return result


def _decode(payload):
    if type(payload) is not str or len(payload) > MAX_BYTES:
        _invalid()
    try:
        if len(payload.encode('utf-8')) > MAX_BYTES:
            _invalid()
        data = json.loads(payload, object_pairs_hook=_duplicates,
                          parse_constant=lambda _: _invalid())
        _validate(data)
    except (UnicodeError, ValueError, TypeError, RecursionError, OverflowError):
        _invalid()
    return data


def _validate(data):
    _keys(data, 'schema versions binding request_sha256 evidence_sha256 budgets state actions')
    if data['schema'] != SCHEMA or data['versions'] != VERSIONS:
        _invalid()
    _keys(data['binding'], 'instance_id session_id learner_id project_id revision snapshot_id '
                          'job_id diagnostic_id issue_id policy')
    SessionBinding(**data['binding'])
    for name in ('request_sha256', 'evidence_sha256'):
        if type(data[name]) is not str or re.fullmatch('[a-f0-9]{64}', data[name]) is None:
            _invalid()
    budgets, state, actions = data['budgets'], data['state'], data['actions']
    _keys(budgets, 'max_steps max_model_calls')
    _integer(budgets['max_steps'], 1, 1000)
    _integer(budgets['max_model_calls'], 0, 100)
    _keys(state, 'run_id stage status level version steps model_calls reason cards')
    if type(state['run_id']) is not str or re.fullmatch('[a-f0-9]{32}', state['run_id']) is None:
        _invalid()
    if (state['stage'], state['status']) not in (
            ('E5', Status.WAITING_STUDENT), ('E5', Status.COMPLETED), ('E4', Status.COMPLETED)):
        _invalid()
    _integer(state['level'], 1, 3)
    _integer(state['version'], 5, 2**63 - 3)
    _integer(state['steps'], 5, budgets['max_steps'])
    _integer(state['model_calls'], 0, budgets['max_model_calls'])
    if (type(state['reason']) is not str or len(state['reason']) > 128
            or re.fullmatch('[a-z0-9_]*', state['reason']) is None):
        _invalid()
    cards = state['cards']
    if type(cards) is not list or len(cards) != state['level']:
        _invalid()
    model_cards = 0
    for index, card in enumerate(cards, 1):
        _keys(card, 'level hint question evidence_ids producer')
        if type(card['level']) is not int or card['level'] != index:
            _invalid()
        if card['producer'] not in ('rule', 'model'):
            _invalid()
        model_cards += card['producer'] == 'model'
        for field in ('hint', 'question'):
            _text(card[field], 1200)
        if type(card['evidence_ids']) is not list or len(card['evidence_ids']) != 1:
            _invalid()
        _text(card['evidence_ids'][0], 8192)
    if model_cards > state['model_calls']:
        _invalid()
    # Every accepted refresh requires at least one budgeted collection step.
    # Keep its idempotency record even when it preceded the first card.
    if type(actions) is not dict or len(actions) > budgets['max_steps'] + 3:
        _invalid()
    versions, kinds = [], []
    for action_id, fingerprint in actions.items():
        _text(action_id, 128)
        if (type(fingerprint) is not list or len(fingerprint) != 3
                or fingerprint[0] not in ('hint_next', 'finish', 'refresh_evidence') or fingerprint[2] != ''):
            _invalid()
        _integer(fingerprint[1], 1, state['version'] - 1)
        versions.append(fingerprint[1])
        kinds.append(fingerprint[0])
    if (len(set(versions)) != len(versions) or kinds.count('hint_next') != state['level'] - 1
            or kinds.count('finish') != int(state['stage'] == 'E5' and state['status'] == Status.COMPLETED)
            or state['version'] < state['steps'] + len(actions)
            or state['steps'] < 5 + 4 * (state['level'] - 1)):
        _invalid()


def _cards(data, request, evidence, course_status='disabled'):
    result = []
    for saved in data['state']['cards']:
        candidate = {name: saved[name] for name in ('level', 'hint', 'question', 'evidence_ids')}
        card = checked_card(candidate, CardContext(request.question, saved['level'], evidence,
                                                  course_status=course_status),
                            saved['producer'])
        if card is None:
            _invalid()
        result.append(card)
    return result


def export_readonly_checkpoint(loop, binding):
    """Called under the session lock, only at a successfully published pause."""
    _scope(binding, loop.request)
    state = loop.state
    if (state.status not in (Status.WAITING_STUDENT, Status.COMPLETED)
            or loop.plan is not None or loop._results or loop._grant_ref
            or not _bound_evidence(loop._evidence, binding, loop.request)):
        raise ValueError('checkpoint_not_available')
    data = {
        'schema': SCHEMA, 'versions': dict(VERSIONS), 'binding': asdict(binding),
        'request_sha256': _digest(asdict(loop.request)),
        'evidence_sha256': _digest(asdict(loop._evidence)),
        'budgets': {'max_steps': loop.max_steps, 'max_model_calls': loop.max_model_calls},
        'state': {name: getattr(state, name) for name in
                  ('run_id', 'stage', 'status', 'level', 'version', 'steps', 'model_calls', 'reason')},
        'actions': {key: list(value) for key, value in loop._actions.items()},
    }
    recovery_cards = state.cards
    if loop.course_provider is not None:
        # Course text/references and their model interpretation are deliberately
        # not replayed from a v1 checkpoint. Rebuild same-level engineering rules;
        # current background is retrieved only after engineering reconciliation.
        recovery_cards = []
        for card in state.cards:
            context = CardContext(loop.request.question, card.level, loop._evidence)
            recovery_cards.append(checked_card(RuleProvider().generate(context), context, 'rule'))
    data['state']['cards'] = [
        {**{name: getattr(card, name) for name in ('level', 'hint', 'question', 'producer')},
         'evidence_ids': list(card.evidence_ids)} for card in recovery_cards]
    # Apply the same validation to exports; no write occurs on rejection.
    payload = _canonical(data)
    data = _decode(payload)
    _cards(data, loop.request, loop._evidence)
    return payload


class RecoveredTeachingSession(TeachingSession):
    """A restored pause quarantined until an explicit trusted-host read."""

    def __init__(self, loop, binding, saved):
        super().__init__(loop)
        self._binding, self._saved = binding, saved

    @property
    def binding(self):
        return self._binding

    @property
    def occurrences(self):
        with self._lock:
            evidence = self._loop._evidence
            return () if evidence is None else (
                Occurrence(self._binding, evidence.evidence_id, _digest(asdict(evidence)), evidence.origin),)

    def advance(self, expected_version):
        with self._lock:
            if self._loop.state.status == Status.RECOVERY_REQUIRED:
                raise ValueError('recovery_required')
            return super().advance(expected_version)

    def host_action(self, kind, action_id, expected_version, *, grant_ref=''):
        with self._lock:
            if self._loop.state.status == Status.RECOVERY_REQUIRED and kind != 'cancel':
                raise ValueError('recovery_required')
            return super().host_action(kind, action_id, expected_version, grant_ref=grant_ref)

    def reconcile(self, expected_version):
        """Re-read the same observation. Never generates cards or submits Jobs."""
        with self._lock:
            state = self._loop._state
            if type(expected_version) is not int or expected_version != state.version:
                raise ValueError('stale_state')
            if state.status != Status.RECOVERY_REQUIRED:
                raise ValueError('recovery_not_required')
            try:
                evidence = self._loop.source.collect(self._binding.project_id, self._binding.revision)
                matched = (_bound_evidence(evidence, self._binding, self._loop.request)
                           and _digest(asdict(evidence)) == self._saved['evidence_sha256'])
            except EvidenceError:
                matched = False
                state.reason = 'recovery_evidence_mismatch'
            except Exception:
                matched = False
                state.reason = 'recovery_evidence_unavailable'
            else:
                state.reason = '' if matched else 'recovery_evidence_mismatch'
            if matched:
                self._loop._evidence = evidence
                self._loop._prepare_courses()
                cards = _cards(self._saved, self._loop.request, evidence, state.course_status)
                state.cards = cards
                state.stage = self._saved['state']['stage']
                state.status = Status(self._saved['state']['status'])
                state.reason = self._saved['state']['reason']
            state.version += 1
            state.trace.append(Trace(state.stage, state.status, state.reason, state.version))
            return self.state


class _FixedObservationSource:
    """Preserve the fixed Job/diagnostic observation through later hints."""

    def __init__(self, source, binding, request, digest):
        self.source, self.binding, self.request, self.digest = source, binding, request, digest

    def collect(self, project_id, revision):
        if (project_id, revision) != (self.binding.project_id, self.binding.revision):
            raise EvidenceError('stale_report')
        evidence = self.source.collect(project_id, revision)
        if (not _bound_evidence(evidence, self.binding, self.request)
                or _digest(asdict(evidence)) != self.digest):
            raise EvidenceError('stale_report')
        return evidence


def restore_readonly_session(payload, *, binding, request, source, provider=None,
                             course_provider=None, state_version=None):
    """Validate without I/O; source must already enforce authenticated scope.

    Does not authenticate a checkpoint or load a Provider from stored settings.
    Only the host may invoke this interface; no model tool registration exists.
    """
    _scope(binding, request)
    data = _decode(payload)
    if data['binding'] != asdict(binding) or data['request_sha256'] != _digest(asdict(request)):
        raise ValueError('checkpoint_scope_mismatch')
    saved = data['state']
    restored_version = saved['version'] + 1
    if state_version is not None:
        if type(state_version) is not int or not restored_version <= state_version <= 2**63 - 3:
            raise ValueError('invalid_recovery_version')
        restored_version = state_version
    if ((request.intent == 'explain' and (saved['stage'] != 'E4' or saved['level'] != 1))
            or (request.intent == 'diagnose' and saved['stage'] != 'E5')):
        _invalid()
    fixed = _FixedObservationSource(source, binding, request, data['evidence_sha256'])
    loop = TeachingLoop(request, fixed, provider, course_provider=course_provider, **data['budgets'])
    loop._state = RunState(saved['run_id'], stage=saved['stage'], status=Status.RECOVERY_REQUIRED,
                           level=saved['level'], version=restored_version,
                           steps=saved['steps'], model_calls=saved['model_calls'], reason='recovery_required')
    loop._actions = {key: tuple(value) for key, value in data['actions'].items()}
    return RecoveredTeachingSession(loop, binding, data)
