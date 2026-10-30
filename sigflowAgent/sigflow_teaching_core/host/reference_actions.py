"""Internal metadata-only teaching actions; public HTTP L4 remains denied."""
from copy import deepcopy
import re
import time

from .durable_reference import DurableReferenceGate
from .reference_journal import ReferenceJournal
from ..domain import Status, Trace


KINDS = ('reference_request', 'reference_confirm', 'reference_reconcile')
_REASONS = frozenset((
    'reference_pending', 'reference_challenge_stale', 'reference_material_unavailable',
    'reference_receipt_conflict', 'reference_context_changed', 'reference_not_pending',
    'reference_budget_exceeded', 'receipt_scope_mismatch'))


def validate_payload(kind, payload):
    if type(payload) is not dict:
        raise ValueError('invalid_reference')
    if kind in ('reference_request', 'reference_reconcile'):
        if payload:
            raise ValueError('invalid_reference')
    elif kind == 'reference_confirm':
        if (set(payload) != {'challenge_id', 'receipt_id'}
                or type(payload['challenge_id']) is not str
                or re.fullmatch(r'[a-f0-9]{32}', payload['challenge_id']) is None
                or type(payload['receipt_id']) is not str
                or re.fullmatch(r'ui-receipt-[1-9][0-9]{0,19}', payload['receipt_id']) is None):
            raise ValueError('invalid_reference')
    else:
        raise ValueError('invalid_reference')


def project_reference(state, *, cancelled=False):
    """Publication fence, including stale/expired historical request results."""
    value = deepcopy(state.get('reference', dict(status='disabled')))
    if cancelled:
        return dict(status='cancelled')
    if state['status'] == 'RecoveryRequired':
        return dict(status='revalidation_required')
    if state['status'] != 'WaitingStudent' or state.get('level') != 3:
        return dict(status='expired') if value.get('status') not in ('disabled', 'idle') else value
    if value.get('status') == 'awaiting_confirmation':
        challenge = value['challenge']
        if (challenge['state_version'] != state['state_version']
                or time.time() >= int(challenge['expires_at_epoch'])):
            return dict(status='expired')
    return value


class ReferenceActions:
    def __init__(self, store, provider, port, closed):
        if not callable(provider) or any(not callable(getattr(port, name, None)) for name in ('lookup', 'consume')):
            raise ValueError('invalid_reference_config')
        config = getattr(port, 'config', None)
        if config is not None and getattr(config, 'instance_id', None) != store.instance_id:
            raise ValueError('invalid_reference_config')
        self.journal = ReferenceJournal(store)
        self.provider, self.port, self.closed = provider, port, closed

    def gate(self, binding, run_id):
        return DurableReferenceGate(binding, run_id, self.provider, self.journal, metadata_only=True)

    def precheck(self, session, binding, kind, payload, expected_version):
        validate_payload(kind, payload)
        if session._loop.plan is not None or session.state.status != Status.WAITING_STUDENT or session.state.level != 3:
            raise ValueError('action_denied')
        if session.state.steps >= session._loop.max_steps:
            raise ValueError('reference_budget_exceeded')
        if self.closed():
            raise ValueError('reference_context_changed')
        gate = self.gate(binding, session.state.run_id)
        gate._context(expected_version)
        record = self.journal.get(binding, session.state.run_id)
        if kind == 'reference_request':
            if record is not None and (record['phase'] == 'consuming'
                    or (record['phase'] == 'invalid' and record['receipt_id'] is not None)):
                raise ValueError('reference_pending')
        elif kind == 'reference_confirm':
            if (record is None or record['phase'] != 'ready'
                    or record['challenge']['challenge_id'] != payload['challenge_id']
                    or record['challenge']['state_version'] != str(expected_version)):
                raise ValueError('reference_challenge_stale')
            gate._live_challenge(record['challenge'])
        elif record is None or record['phase'] not in ('consuming', 'released'):
            raise ValueError('reference_not_pending')

    def _port(self, binding):
        parent = self
        class FencedPort:
            def _check(self):
                if parent.closed() or parent.journal.store.cancel_requested(binding.session_id):
                    raise ValueError('reference_context_changed')

            def lookup(self, receipt_id):
                self._check()
                return parent.port.lookup(receipt_id)

            def consume(self, *args):
                self._check()
                return parent.port.consume(*args)
        return FencedPort()

    def apply(self, session, binding, kind, payload, expected_version):
        """Return a draft for atomic request commit, otherwise metadata only."""
        with session._lock:
            if session.state.version != expected_version:
                raise ValueError('stale_state')
            draft = None
            try:
                self.precheck(session, binding, kind, payload, expected_version)
                gate = self.gate(binding, session.state.run_id)
                if kind == 'reference_request':
                    draft = gate.draft(expected_version, expected_version + 1)
                    metadata = dict(status='awaiting_confirmation', challenge=draft)
                else:
                    port = self._port(binding)
                    result = (gate.confirm(payload['receipt_id'], payload['challenge_id'], expected_version, port)
                              if kind == 'reference_confirm' else gate.reconcile(expected_version, port))
                    metadata = dict(status='confirmed', body_available=False, **result)
            except Exception as error:
                # Neither provider/transport messages nor submitted receipt IDs
                # are safe action results. Unknown outcomes retain their journal.
                record = self.journal.get(binding, session.state.run_id)
                pending = record is not None and record['phase'] in ('consuming', 'released')
                reason = str(error) if str(error) in _REASONS else 'reference_unavailable'
                metadata = dict(status='pending' if pending else 'unavailable', reason=reason)
            session._reference_state = metadata
            state = session._loop._state
            state.steps += 1
            state.version += 1
            state.reason = 'reference_' + metadata['status']
            state.trace.append(Trace(state.stage, state.status, state.reason, state.version))
            return draft

    def recovered(self, session, binding):
        # Revalidation reads engineering evidence only, never Core receipts.
        record = self.journal.get(binding, session.state.run_id)
        status = ('reconcile_required' if record['phase'] in ('consuming', 'released')
                  else 'new_request_required') if record else 'idle'
        session._reference_state = dict(status=status)

    def invalidate(self, session, binding):
        record = self.journal.invalidate(binding, session.state.run_id)
        if record is not None:
            session._reference_state = dict(status='expired')
