"""Bounded reference metadata using the host store's transaction and owner lock.

No reference body, model output, or authorization token is accepted. A pending
consume keeps its original receipt and approval version across host recovery.
The trusted gate must revalidate evidence and reviewed material before release.
"""
from dataclasses import asdict
import json
import re

from .store import DurableStore, StoreError
from ..runtime.gateway_session import SessionBinding


_KEYS = frozenset(('challenge_id', 'action_id', 'session_id', 'project_id',
    'revision', 'learner_id', 'issue_id', 'run_id', 'reference_id',
    'reference_sha256', 'review_sha256', 'level', 'policy', 'state_version',
    'expires_at_epoch'))
_SCOPE_KEYS = ('session_id', 'project_id', 'revision', 'learner_id', 'issue_id')
_PHASES = frozenset(('ready', 'consuming', 'released', 'invalid'))


def _matches(pattern, value):
    return type(value) is str and re.fullmatch(pattern, value) is not None


def _challenge(value, binding, run_id):
    # Validate the raw dictionary before serializing or projecting fields.
    if (type(value) is not dict or value.keys() != _KEYS
            or any(type(item) is not str for item in value.values())
            or not _matches(r'[a-f0-9]{32}', value['challenge_id'])
            or not _matches(r'reference-[a-f0-9]{32}', value['action_id'])
            or not _matches(r'[A-Za-z0-9_-]{1,128}', value['reference_id'])
            or not _matches(r'[a-f0-9]{64}', value['reference_sha256'])
            or not _matches(r'[a-f0-9]{64}', value['review_sha256'])
            or value['level'] != 'l4' or value['policy'] != 'u2-reviewed-reference.v1'
            or not _matches(r'0|[1-9][0-9]{0,18}', value['state_version'])
            or int(value['state_version']) >= 2**63
            or not _matches(r'[1-9][0-9]{0,19}', value['expires_at_epoch'])
            or int(value['expires_at_epoch']) >= 2**64):
        raise ValueError('reference_invalid_record')
    if (any(value[key] != binding[key] for key in _SCOPE_KEYS)
            or value['run_id'] != run_id):
        raise ValueError('reference_scope_mismatch')
    return dict(value)


class ReferenceJournal:
    def __init__(self, store):
        if not isinstance(store, DurableStore):
            raise ValueError('reference_scope_mismatch')
        self.store = store
        with store._transaction() as db:
            db.execute('''CREATE TABLE IF NOT EXISTS reference_records (
                session_id TEXT PRIMARY KEY REFERENCES sessions(session_id),
                binding TEXT NOT NULL, run_id TEXT NOT NULL,
                challenge TEXT NOT NULL, phase TEXT NOT NULL, receipt_id TEXT)''')

    def _session(self, db, binding, run_id, *, live=False, expected_version=None):
        if (not isinstance(binding, SessionBinding)
                or not _matches(r'[A-Za-z0-9_-]{1,128}', run_id)):
            raise ValueError('reference_scope_mismatch')
        scope = asdict(binding)
        if scope['instance_id'] != self.store.instance_id:
            raise ValueError('reference_scope_mismatch')
        try:
            session = self.store._decode(self.store._row(db, binding.session_id))
        except StoreError as exc:
            if str(exc) == 'session_not_found':
                raise ValueError('reference_scope_mismatch') from None
            raise
        if session['binding'] != scope or session['state'].get('run_id') != run_id:
            raise ValueError('reference_scope_mismatch')
        state = session['state']
        if live and (session['cancel_requested'] or state['status'] != 'WaitingStudent'
                     or type(state.get('level')) is not int or state['level'] != 3):
            raise ValueError('reference_context_changed')
        if expected_version is not None and (
                type(expected_version) is not int or not 0 <= expected_version < 2**63
                or int(state['state_version']) != expected_version):
            raise ValueError('reference_context_changed')
        return scope, int(state['state_version'])

    @staticmethod
    def _record(db, scope, run_id):
        row = db.execute('SELECT * FROM reference_records WHERE session_id=?',
                         (scope['session_id'],)).fetchone()
        if row is None:
            return None
        try:
            saved_scope = json.loads(row['binding'])
            if saved_scope != scope or row['run_id'] != run_id:
                raise ValueError('reference_scope_mismatch')
            challenge = _challenge(json.loads(row['challenge']), scope, run_id)
            phase, receipt = row['phase'], row['receipt_id']
            if (phase not in _PHASES
                    or (receipt is not None and not _matches(r'ui-receipt-[1-9][0-9]{0,19}', receipt))
                    or (phase == 'ready' and receipt is not None)
                    or (phase in ('consuming', 'released') and receipt is None)):
                raise ValueError('reference_invalid_record')
            return dict(challenge=challenge, phase=phase, receipt_id=receipt)
        except (TypeError, RecursionError, UnicodeError, json.JSONDecodeError):
            raise ValueError('reference_invalid_record') from None

    def get(self, binding, run_id):
        """Inspect metadata even while the durable host session is quarantined."""
        with self.store._transaction() as db:
            scope, _ = self._session(db, binding, run_id)
            return self._record(db, scope, run_id)

    def prepare(self, binding, run_id, challenge):
        with self.store._transaction() as db:
            return self._prepare(db, binding, run_id, challenge)

    def _prepare(self, db, binding, run_id, challenge):
        scope, version = self._session(db, binding, run_id, live=True)
        value = _challenge(challenge, scope, run_id)
        previous = self._record(db, scope, run_id)
        # Invalidation revokes publication, but cannot erase an operation
        # whose receipt may already have been consumed outside this store.
        if previous is not None and (previous['phase'] == 'consuming'
                or (previous['phase'] == 'invalid' and previous['receipt_id'] is not None)):
            raise ValueError('reference_pending')
        if int(value['state_version']) != version:
            raise ValueError('reference_challenge_stale')
        db.execute('''INSERT INTO reference_records
            (session_id, binding, run_id, challenge, phase, receipt_id)
            VALUES (?, ?, ?, ?, 'ready', NULL)
            ON CONFLICT(session_id) DO UPDATE SET binding=excluded.binding,
            run_id=excluded.run_id, challenge=excluded.challenge,
            phase='ready', receipt_id=NULL''',
            (scope['session_id'], json.dumps(scope, sort_keys=True, separators=(',', ':')),
             run_id, json.dumps(value, sort_keys=True, separators=(',', ':'))))
        return dict(challenge=value, phase='ready', receipt_id=None)

    def commit_request(self, binding, run_id, challenge, action_id, state, checkpoint):
        """Publish only request metadata and its challenge in one host commit."""
        with self.store._transaction() as db:
            scope, _ = self._session(db, binding, run_id, live=True)
            value = _challenge(challenge, scope, run_id)
            if (type(state) is not dict or state.get('reference') !=
                    dict(status='awaiting_confirmation', challenge=value)):
                raise ValueError('reference_invalid_record')
            self.store._commit(db, binding.session_id, action_id, state, checkpoint, state)
            return self._prepare(db, binding, run_id, value)

    def claim(self, binding, run_id, challenge_id, receipt_id, *, expected_version=None):
        with self.store._transaction() as db:
            scope, version = self._session(db, binding, run_id, live=True,
                                           expected_version=expected_version)
            record = self._record(db, scope, run_id)
            self._current(record, challenge_id)
            if not _matches(r'ui-receipt-[1-9][0-9]{0,19}', receipt_id):
                raise ValueError('reference_invalid_record')
            if record['phase'] in ('consuming', 'released'):
                if receipt_id != record['receipt_id']:
                    raise ValueError('reference_receipt_conflict')
                return record
            if int(record['challenge']['state_version']) != version:
                raise ValueError('reference_challenge_stale')
            db.execute("UPDATE reference_records SET phase='consuming', receipt_id=? WHERE session_id=?",
                       (receipt_id, scope['session_id']))
            record.update(phase='consuming', receipt_id=receipt_id)
            return record

    @staticmethod
    def _current(record, challenge_id):
        if (record is None or record['phase'] == 'invalid'
                or not _matches(r'[a-f0-9]{32}', challenge_id)
                or record['challenge']['challenge_id'] != challenge_id):
            raise ValueError('reference_challenge_stale')

    def release(self, binding, run_id, challenge_id, *, expected_version=None):
        with self.store._transaction() as db:
            scope, _ = self._session(db, binding, run_id, live=True,
                                     expected_version=expected_version)
            record = self._record(db, scope, run_id)
            self._current(record, challenge_id)
            if record['phase'] == 'ready':
                raise ValueError('reference_challenge_stale')
            if record['phase'] == 'consuming':
                db.execute("UPDATE reference_records SET phase='released' WHERE session_id=?",
                           (scope['session_id'],))
                record['phase'] = 'released'
            return record

    def invalidate(self, binding, run_id):
        """Local cleanup preserves receipt audit fields, including after cancel."""
        with self.store._transaction() as db:
            scope, _ = self._session(db, binding, run_id)
            record = self._record(db, scope, run_id)
            if record is not None:
                db.execute("UPDATE reference_records SET phase='invalid' WHERE session_id=?",
                           (scope['session_id'],))
                record['phase'] = 'invalid'
            return record
