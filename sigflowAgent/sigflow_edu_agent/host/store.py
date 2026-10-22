"""Bounded, single-owner SQLite journal for trusted teaching-host state.

The lock file is persistent: deleting it could let another owner lock a different
inode. No PID probing or credentials are involved. Callers supply curated wire
state and read-only checkpoints; student requests are represented only by hash.
"""

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
from threading import RLock

from ..runtime.gateway_session import SessionBinding
from .teaching_memory import enqueue_facts, validate_fact, ACTION_KINDS


MAX_SESSIONS = 32
MAX_ACTIONS = 256
MAX_EVENTS = 1000
MAX_CHECKPOINT_BYTES = 128 * 1024
MAX_JSON_BYTES = 128 * 1024
MAX_VERSION = 2**63 - 2
TERMINAL = frozenset(('Completed', 'Cancelled', 'Failed', 'BudgetExceeded'))
STATUSES = TERMINAL | frozenset(('Running', 'WaitingStudent', 'WaitingEvidence',
                                'WaitingApproval', 'RecoveryRequired'))
_SENSITIVE = frozenset(('grant', 'grant_ref', 'grant_token', 'receipt', 'token',
                        'access_token', 'api_key', 'apikey', 'authorization',
                        'credential', 'credentials', 'request', 'request_body',
                        'student_question', 'source_code'))


class StoreError(ValueError):
    """Stable lowercase error code, never a raw filesystem/SQLite diagnostic."""

    @property
    def code(self):
        return str(self)


def _text(value, maximum=256):
    if type(value) is not str or not value.strip() or len(value) > maximum:
        raise StoreError('invalid_input')
    try:
        value.encode('utf-8')
    except UnicodeError:
        raise StoreError('invalid_input') from None
    if any(ord(char) < 32 for char in value):
        raise StoreError('invalid_input')
    return value


def _integer(value, minimum=0, maximum=MAX_VERSION):
    if type(value) is not int or not minimum <= value <= maximum:
        raise StoreError('invalid_input')
    return value


def _safe(value, depth=0, context=None):
    if depth > 32:
        raise StoreError('invalid_input')
    if type(value) is dict:
        for key, item in value.items():
            if (type(key) is not str or key.lower() in _SENSITIVE
                    or (key.lower() == 'question' and context != 'card')):
                raise StoreError('invalid_input')
            _safe(item, depth + 1, key)
    elif type(value) in (list, tuple):
        for item in value:
            _safe(item, depth + 1, 'card' if context == 'cards' else None)
    elif value is not None and type(value) not in (str, int, float, bool):
        raise StoreError('invalid_input')


def _json(value, maximum=MAX_JSON_BYTES):
    if type(value) is not dict:
        raise StoreError('invalid_input')
    try:
        _safe(value)
        text = json.dumps(value, sort_keys=True, ensure_ascii=False,
                          separators=(',', ':'), allow_nan=False)
        if len(text.encode('utf-8')) > maximum:
            raise StoreError('invalid_input')
        return text
    except (UnicodeError, TypeError, ValueError, RecursionError, OverflowError) as exc:
        if isinstance(exc, StoreError):
            raise
        raise StoreError('invalid_input') from None


def _state(value):
    payload = _json(value)
    version = value.get('state_version')
    if (type(version) is not str or len(version) > 19
            or re.fullmatch(r'0|[1-9][0-9]*', version) is None
            or int(version) > MAX_VERSION
            or type(value.get('status')) is not str
            or value.get('status') not in STATUSES
            or type(value.get('cards')) is not list):
        raise StoreError('invalid_input')
    return payload, int(version)


def _checkpoint(payload):
    if payload is None:
        return None, None
    if type(payload) is not str:
        raise StoreError('invalid_input')
    try:
        encoded = payload.encode('utf-8')
    except UnicodeError:
        raise StoreError('invalid_input') from None
    if len(encoded) > MAX_CHECKPOINT_BYTES:
        raise StoreError('checkpoint_too_large')
    # Real checkpoints are JSON. Keep this storage primitive independent of the
    # checkpoint schema while rejecting accidental credential/request fields.
    try:
        decoded = json.loads(payload)
    except (ValueError, RecursionError):
        decoded = None
    if decoded is not None:
        _safe(decoded)
    return payload, hashlib.sha256(encoded).hexdigest()


class DurableStore:
    def __init__(self, path, instance_id):
        self.instance_id = _text(instance_id)
        self._mutex = RLock()
        self._db = None
        self._lock_file = None
        self._locked = False
        self._closed = False
        try:
            self.path = Path(path).resolve()
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._lock_file = open(str(self.path) + '.lock', 'a+b')
            self._lock_file.seek(0, os.SEEK_END)
            if self._lock_file.tell() == 0:
                self._lock_file.write(b'\0')
                self._lock_file.flush()
            self._lock_file.seek(0)
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(self._lock_file.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self._lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                raise StoreError('store_locked') from None
            self._locked = True
            self._db = sqlite3.connect(self.path, timeout=5, isolation_level=None,
                                       check_same_thread=False)
            self._db.row_factory = sqlite3.Row
            self._db.execute('PRAGMA foreign_keys = ON')
            self._db.execute('PRAGMA journal_mode = DELETE')
            self._db.execute('PRAGMA synchronous = FULL')
            self._db.execute('PRAGMA max_page_count = 65536')
            self._initialize()
        except StoreError:
            self.close()
            raise
        except (OSError, sqlite3.Error, ValueError, TypeError):
            self.close()
            raise StoreError('storage_error') from None

    @contextmanager
    def _transaction(self):
        with self._mutex:
            if self._closed:
                raise StoreError('store_closed')
            try:
                self._db.execute('BEGIN IMMEDIATE')
                yield self._db
                self._db.execute('COMMIT')
            except BaseException as exc:
                try:
                    if self._db.in_transaction:
                        self._db.execute('ROLLBACK')
                except sqlite3.Error:
                    self._closed = True
                if isinstance(exc, sqlite3.Error):
                    raise StoreError('storage_error') from None
                raise

    def _initialize(self):
        with self._transaction() as db:
            db.execute('CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)')
            row = db.execute("SELECT value FROM metadata WHERE key = 'instance_id'").fetchone()
            if row is not None and row['value'] != self.instance_id:
                raise StoreError('instance_conflict')
            db.execute("INSERT OR IGNORE INTO metadata VALUES ('instance_id', ?)", (self.instance_id,))
            db.execute('''CREATE TABLE IF NOT EXISTS sessions (
                session_id TEXT PRIMARY KEY, binding TEXT NOT NULL,
                request_sha256 TEXT NOT NULL, mode TEXT NOT NULL, intent TEXT NOT NULL,
                state TEXT NOT NULL, state_version INTEGER NOT NULL,
                checkpoint TEXT, checkpoint_sha256 TEXT, model_used INTEGER NOT NULL DEFAULT 0,
                cancel_requested INTEGER NOT NULL DEFAULT 0, event_seq INTEGER NOT NULL DEFAULT 0)''')
            db.execute('''CREATE TABLE IF NOT EXISTS actions (
                session_id TEXT NOT NULL REFERENCES sessions(session_id),
                action_id TEXT NOT NULL, fingerprint TEXT NOT NULL,
                expected_version INTEGER NOT NULL, status TEXT NOT NULL,
                result TEXT, PRIMARY KEY(session_id, action_id))''')
            db.execute('''CREATE TABLE IF NOT EXISTS events (
                session_id TEXT NOT NULL REFERENCES sessions(session_id),
                sequence INTEGER NOT NULL, action_id TEXT, state_version TEXT NOT NULL,
                status TEXT NOT NULL, PRIMARY KEY(session_id, sequence))''')
            columns={column['name'] for column in db.execute('PRAGMA table_info(actions)')}
            if 'kind' not in columns:
                db.execute("ALTER TABLE actions ADD COLUMN kind TEXT NOT NULL DEFAULT ''")
            # U2 transport metadata only; UA-10 owns its learning schema.
            db.execute('''CREATE TABLE IF NOT EXISTS teaching_outbox (
                event_id TEXT PRIMARY KEY, session_id TEXT NOT NULL REFERENCES sessions(session_id),
                payload TEXT NOT NULL, delivered INTEGER NOT NULL DEFAULT 0)''')
            rows = db.execute('SELECT * FROM sessions ORDER BY rowid').fetchall()
            for row in rows:
                value = self._decode(row)
                state = value['state']
                if state['status'] not in TERMINAL and state['status'] != 'RecoveryRequired':
                    state.update(status='RecoveryRequired', cards=[],
                                 state_version=str(min(int(state['state_version']) + 1, MAX_VERSION)))
                    payload, version = _state(state)
                    db.execute('UPDATE sessions SET state = ?, state_version = ? WHERE session_id = ?',
                               (payload, version, row['session_id']))
                    self._event(db, row['session_id'], None, state)
            db.execute("UPDATE actions SET status = 'interrupted' WHERE status = 'pending'")

    def _row(self, db, session_id):
        row = db.execute('SELECT * FROM sessions WHERE session_id = ?', (session_id,)).fetchone()
        if row is None:
            raise StoreError('session_not_found')
        return row

    def _decode(self, row):
        checkpoint = row['checkpoint']
        try:
            if checkpoint is None:
                if row['checkpoint_sha256'] is not None:
                    raise StoreError('checkpoint_corrupt')
            elif hashlib.sha256(checkpoint.encode('utf-8')).hexdigest() != row['checkpoint_sha256']:
                raise StoreError('checkpoint_corrupt')
            state = json.loads(row['state'])
            _, version = _state(state)
            if version != row['state_version']:
                raise StoreError('state_corrupt')
            binding = json.loads(row['binding'])
            SessionBinding(**binding)
            _integer(row['model_used'], 0, 100)
            _integer(row['cancel_requested'], 0, 1)
            if binding['instance_id'] != self.instance_id or binding['session_id'] != row['session_id']:
                raise StoreError('state_corrupt')
            return dict(session_id=row['session_id'], binding=binding,
                        request_sha256=row['request_sha256'], mode=row['mode'], intent=row['intent'],
                        state=state, checkpoint=checkpoint, model_used=row['model_used'],
                        cancel_requested=bool(row['cancel_requested']),
                        event_high_watermark=row['event_seq'])
        except StoreError as exc:
            if str(exc) == 'invalid_input':
                raise StoreError('state_corrupt') from None
            raise
        except (UnicodeError, ValueError, TypeError, KeyError, RecursionError):
            raise StoreError('state_corrupt') from None

    def create(self, session_id, binding, request_sha256, mode, intent, state, *, initial_action=None):
        _text(session_id)
        payload, version = _state(state)
        scope = _json(binding, 16 * 1024)
        try:
            SessionBinding(**binding)
        except (TypeError, ValueError):
            raise StoreError('invalid_input') from None
        if binding['instance_id'] != self.instance_id or binding['session_id'] != session_id:
            raise StoreError('binding_conflict')
        if (type(request_sha256) is not str or re.fullmatch('[a-f0-9]{64}', request_sha256) is None
                or mode not in ('rule', 'deepseek') or intent not in ('explain', 'diagnose', 'verify')):
            raise StoreError('invalid_input')
        if initial_action is not None:
            if type(initial_action) is not tuple or len(initial_action) != 2:
                raise StoreError('invalid_input')
            _text(initial_action[0], 128)
            if type(initial_action[1]) is not str or re.fullmatch('[a-f0-9]{64}', initial_action[1]) is None:
                raise StoreError('invalid_input')
        with self._transaction() as db:
            if db.execute('SELECT 1 FROM sessions WHERE session_id = ?', (session_id,)).fetchone():
                raise StoreError('session_exists')
            if db.execute('SELECT COUNT(*) FROM sessions').fetchone()[0] >= MAX_SESSIONS:
                raise StoreError('session_limit')
            db.execute('''INSERT INTO sessions
                (session_id, binding, request_sha256, mode, intent, state, state_version)
                VALUES (?, ?, ?, ?, ?, ?, ?)''',
                       (session_id, scope, request_sha256, mode, intent, payload, version))
            enqueue_facts(db,binding,state,created=True)
            if initial_action is not None:
                db.execute('''INSERT INTO actions
                    (session_id, action_id, fingerprint, expected_version, status, result)
                    VALUES (?, ?, ?, ?, 'pending', NULL)''',
                    (session_id, initial_action[0], initial_action[1], version))

    def get(self, session_id):
        _text(session_id)
        with self._transaction() as db:
            return self._decode(self._row(db, session_id))

    def list_sessions(self):
        with self._transaction() as db:
            return [self._decode(row) for row in db.execute('SELECT * FROM sessions ORDER BY rowid')]

    @staticmethod
    def _action_result(row):
        try:
            result = None if row['result'] is None else json.loads(row['result'])
            if result is not None:
                _json(result)
            if row['status'] not in ('pending', 'committed', 'interrupted'):
                raise StoreError('state_corrupt')
            return {'status': row['status'], 'result': result}
        except (ValueError, TypeError, RecursionError):
            raise StoreError('state_corrupt') from None

    def has_pending(self, session_id):
        """Inspect durable admission without acquiring a teaching session lock."""
        _text(session_id)
        with self._transaction() as db:
            self._decode(self._row(db, session_id))
            return db.execute("SELECT 1 FROM actions WHERE session_id=? AND status='pending'",
                              (session_id,)).fetchone() is not None

    def claim(self, session_id, action_id, fingerprint, expected_version, *, kind=''):
        _text(session_id)
        _text(action_id, 128)
        _text(fingerprint, 256)
        _integer(expected_version)
        if kind not in ('','create','recover','cancel',*ACTION_KINDS):
            raise StoreError('invalid_input')
        with self._transaction() as db:
            session = self._decode(self._row(db, session_id))
            action = db.execute('SELECT * FROM actions WHERE session_id = ? AND action_id = ?',
                                (session_id, action_id)).fetchone()
            if action:
                if action['fingerprint'] != fingerprint:
                    raise StoreError('action_conflict')
                return self._action_result(action)
            if db.execute("SELECT 1 FROM actions WHERE session_id = ? AND status = 'pending'",
                          (session_id,)).fetchone():
                raise StoreError('session_busy')
            if int(session['state']['state_version']) != expected_version:
                raise StoreError('state_conflict')
            if db.execute('SELECT COUNT(*) FROM actions WHERE session_id = ?',
                          (session_id,)).fetchone()[0] >= MAX_ACTIONS:
                raise StoreError('action_limit')
            db.execute("INSERT INTO actions(session_id,action_id,fingerprint,expected_version,status,result,kind) VALUES (?, ?, ?, ?, 'pending', NULL, ?)",
                       (session_id, action_id, fingerprint, expected_version,kind))
            return None

    def _event(self, db, session_id, action_id, state,previous_state=None):
        db.execute('UPDATE sessions SET event_seq = event_seq + 1 WHERE session_id = ?', (session_id,))
        sequence = self._row(db, session_id)['event_seq']
        db.execute('INSERT INTO events VALUES (?, ?, ?, ?, ?)',
                   (session_id, sequence, action_id, state['state_version'], state['status']))
        db.execute('DELETE FROM events WHERE session_id = ? AND sequence <= ?',
                   (session_id, sequence - MAX_EVENTS))
        row=self._row(db,session_id)
        action=(db.execute('SELECT kind FROM actions WHERE session_id=? AND action_id=?',
                           (session_id,action_id)).fetchone() if action_id else None)
        enqueue_facts(db,json.loads(row['binding']),state,action_id=action_id,
                      action_kind=action['kind'] if action else '',
                      first_card=len((previous_state or {}).get('cards',[])))

    def pending_teaching_facts(self,limit=32):
        """Trusted composition port, never exposed to HTTP or model tools."""
        _integer(limit,1,32)
        with self._transaction() as db:
            try:
                return [validate_fact(json.loads(row['payload']),json.loads(row['binding'])) for row in db.execute(
                    '''SELECT o.payload,s.binding FROM teaching_outbox o JOIN sessions s
                       ON o.session_id=s.session_id WHERE o.delivered=0 ORDER BY o.rowid LIMIT ?''',(limit,))]
            except (ValueError,KeyError,TypeError,RecursionError):
                raise StoreError('state_corrupt') from None

    def ack_teaching_fact(self,event_id):
        if type(event_id) is not str or re.fullmatch(r'u2fact-[a-f0-9]{64}',event_id) is None:
            raise StoreError('invalid_input')
        with self._transaction() as db:
            row=db.execute('SELECT 1 FROM teaching_outbox WHERE event_id=?',(event_id,)).fetchone()
            if row is None:return False
            db.execute('UPDATE teaching_outbox SET delivered=1 WHERE event_id=?',(event_id,))
            return True

    def commit(self, session_id, action_id, state, checkpoint, result):
        with self._transaction() as db:
            self._commit(db, session_id, action_id, state, checkpoint, result)

    def _commit(self, db, session_id, action_id, state, checkpoint, result):
        """Commit within an existing store transaction, including CAS and event."""
        _text(session_id)
        _text(action_id, 128)
        payload, version = _state(state)
        checkpoint, digest = _checkpoint(checkpoint)
        result_payload = _json(result)
        row = self._row(db, session_id)
        self._decode(row)
        action = db.execute('SELECT * FROM actions WHERE session_id = ? AND action_id = ?',
                            (session_id, action_id)).fetchone()
        if action is None:
            raise StoreError('action_not_found')
        if action['status'] != 'pending':
            raise StoreError('action_conflict')
        if row['state_version'] != action['expected_version'] or version <= row['state_version']:
            raise StoreError('state_conflict')
        if row['cancel_requested'] and state['status'] not in ('Cancelled', 'Failed', 'BudgetExceeded'):
            raise StoreError('cancel_requested')
        db.execute('''UPDATE sessions SET state = ?, state_version = ?, checkpoint = ?,
            checkpoint_sha256 = ? WHERE session_id = ?''',
                   (payload, version, checkpoint, digest, session_id))
        db.execute("UPDATE actions SET status = 'committed', result = ? WHERE session_id = ? AND action_id = ?",
                   (result_payload, session_id, action_id))
        self._event(db, session_id, action_id, state,json.loads(row['state']))

    def stage_verification(self, session_id, action_id, state, checkpoint):
        """Commit the restart cursor before external execution, keeping its action pending.

        Moving the pending action's commit cursor preserves ordinary commit CAS
        and its original fingerprint, so retries still replay rather than execute.
        """
        payload, version = _state(state)
        checkpoint, checkpoint_hash = _checkpoint(checkpoint)
        with self._transaction() as db:
            row = self._row(db, session_id)
            self._decode(row)
            action = db.execute('SELECT * FROM actions WHERE session_id=? AND action_id=?',
                                (session_id, action_id)).fetchone()
            if (action is None or action['status'] != 'pending' or row['intent'] != 'verify'
                    or row['state_version'] != action['expected_version']
                    or version <= row['state_version'] or not checkpoint):
                raise StoreError('state_conflict')
            if row['cancel_requested']:
                raise StoreError('cancel_requested')
            db.execute('''UPDATE sessions SET state=?, state_version=?, checkpoint=?, checkpoint_sha256=?
                WHERE session_id=?''', (payload, version, checkpoint, checkpoint_hash, session_id))
            db.execute('UPDATE actions SET expected_version=? WHERE session_id=? AND action_id=?',
                       (version, session_id, action_id))
            self._event(db, session_id, action_id, state,json.loads(row['state']))

    def action(self, session_id, action_id):
        _text(session_id)
        _text(action_id, 128)
        with self._transaction() as db:
            self._decode(self._row(db, session_id))
            row = db.execute('SELECT * FROM actions WHERE session_id = ? AND action_id = ?',
                             (session_id, action_id)).fetchone()
            if row is None:
                raise StoreError('action_not_found')
            return self._action_result(row)

    def reserve_model(self, session_id, maximum):
        _text(session_id)
        _integer(maximum, 0, 100)
        with self._transaction() as db:
            row = self._row(db, session_id)
            self._decode(row)
            if row['cancel_requested']:
                raise StoreError('cancel_requested')
            if row['model_used'] >= maximum:
                raise StoreError('budget_exceeded')
            used = row['model_used'] + 1
            db.execute('UPDATE sessions SET model_used = ? WHERE session_id = ?', (used, session_id))
            return used

    def model_used(self, session_id):
        return self.get(session_id)['model_used']

    def request_cancel(self, session_id):
        _text(session_id)
        with self._transaction() as db:
            self._decode(self._row(db, session_id))
            db.execute('UPDATE sessions SET cancel_requested = 1 WHERE session_id = ?', (session_id,))

    def cancel_session(self, session_id):
        """Privileged host cancellation, independent of client keys and quotas.

        This commits the teaching terminal state, not an EDA Job terminal claim.
        In-flight reservations are interrupted; their eventual candidates cannot
        be committed. No teaching/provider/Job operation is retried here.
        """
        _text(session_id)
        with self._transaction() as db:
            row = self._row(db, session_id)
            decoded = self._decode(row)
            state = decoded['state']
            if state['status'] in TERMINAL:
                return state
            version = int(state['state_version'])
            if version >= MAX_VERSION:
                raise StoreError('version_limit')
            state.update(status='Cancelled', cards=[], reason='cancelled_by_user', state_version=str(version + 1))
            payload, version = _state(state)
            db.execute('''UPDATE sessions SET state=?, state_version=?, checkpoint=NULL,
                checkpoint_sha256=NULL, cancel_requested=1 WHERE session_id=?''', (payload, version, session_id))
            db.execute("UPDATE actions SET status='interrupted' WHERE session_id=? AND status='pending'", (session_id,))
            self._event(db, session_id, None, state,json.loads(row['state']))
            return state

    def cancel_requested(self, session_id):
        return self.get(session_id)['cancel_requested']

    def events(self, session_id, after, limit=100):
        _text(session_id)
        _integer(after, 0, 2**63 - 1)
        _integer(limit, 1, MAX_EVENTS)
        with self._transaction() as db:
            row = self._row(db, session_id)
            self._decode(row)
            if after > row['event_seq']:
                raise StoreError('cursor_stale')
            first = db.execute('SELECT MIN(sequence) FROM events WHERE session_id = ?',
                               (session_id,)).fetchone()[0]
            if first is not None and after < first - 1:
                raise StoreError('cursor_expired')
            rows = db.execute('''SELECT sequence, action_id, state_version, status FROM events
                WHERE session_id = ? AND sequence > ? ORDER BY sequence LIMIT ?''',
                              (session_id, after, limit))
            return {'events': [dict(item) for item in rows], 'high_watermark': row['event_seq']}

    def close(self):
        with self._mutex:
            self._closed = True
            try:
                if self._db is not None:
                    self._db.close()
                    self._db = None
            finally:
                if self._lock_file is not None:
                    try:
                        if self._locked:
                            self._lock_file.seek(0)
                            if os.name == 'nt':
                                import msvcrt
                                msvcrt.locking(self._lock_file.fileno(), msvcrt.LK_UNLCK, 1)
                            else:
                                import fcntl
                                fcntl.flock(self._lock_file.fileno(), fcntl.LOCK_UN)
                    finally:
                        self._lock_file.close()
                        self._lock_file = None
                        self._locked = False
