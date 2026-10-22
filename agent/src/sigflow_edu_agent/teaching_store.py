"""U1-owned metadata associations; U2 remains the sole teaching state journal."""
from contextlib import contextmanager
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import re
import sqlite3
from threading import RLock

from .errors import AgentError

MAX_SESSIONS=32
MAX_RUNS=64
SCOPE_KEYS={'job_id','snapshot_id','diagnostic_id','issue_id','intent','mode'}
CONTEXT_KEYS={'session_id','run_id','instance_id','learner_id','project_id','revision',
              'snapshot_id','job_id','diagnostic_id','issue_id'}


def identifier(value):
    if type(value) is not str or re.fullmatch(r'[A-Za-z0-9_-]{1,128}',value) is None:
        raise AgentError('INVALID_ARGUMENT','invalid teaching identifier',http_status=400)
    return value


def fingerprint(value):
    try:
        raw=json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=True,allow_nan=False).encode()
    except (ValueError,TypeError,RecursionError):
        raise AgentError('INVALID_ARGUMENT','invalid teaching request',http_status=400) from None
    return hashlib.sha256(raw).hexdigest()


def _deny(code='IDEMPOTENCY_CONFLICT',status=409):
    raise AgentError(code,'teaching association rejected',http_status=status)


class BindingStore:
    def __init__(self,path,instance_id):
        self.instance_id=identifier(instance_id)
        self._lock=RLock()
        self._db=None
        self._closed=False
        try:
            path=Path(path)
            path.parent.mkdir(parents=True,exist_ok=True)
            self._db=sqlite3.connect(path,timeout=5,isolation_level=None,check_same_thread=False)
            self._db.row_factory=sqlite3.Row
            self._db.execute('PRAGMA foreign_keys=ON')
            self._db.execute('PRAGMA synchronous=FULL')
            self._db.execute('PRAGMA max_page_count=16384')
            with self._tx() as db:
                db.execute('CREATE TABLE IF NOT EXISTS metadata (name TEXT PRIMARY KEY,value TEXT NOT NULL)')
                row=db.execute("SELECT value FROM metadata WHERE name='instance_id'").fetchone()
                if row and row['value']!=instance_id:_deny()
                db.execute("INSERT OR IGNORE INTO metadata VALUES ('instance_id',?)",(instance_id,))
                db.execute('''CREATE TABLE IF NOT EXISTS sessions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,learner TEXT NOT NULL,project TEXT NOT NULL,
                    revision TEXT NOT NULL,key_hash TEXT NOT NULL,request_hash TEXT NOT NULL,
                    UNIQUE(learner,key_hash))''')
                db.execute('''CREATE TABLE IF NOT EXISTS runs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,sid INTEGER NOT NULL REFERENCES sessions(id),
                    key_hash TEXT NOT NULL,request_hash TEXT NOT NULL,scope TEXT NOT NULL,
                    handle TEXT,UNIQUE(sid,key_hash))''')
        except Exception:
            self.close()
            raise

    @contextmanager
    def _tx(self):
        with self._lock:
            if self._closed:_deny('SERVICE_UNAVAILABLE',503)
            try:
                self._db.execute('BEGIN IMMEDIATE')
                yield self._db
                self._db.execute('COMMIT')
            except BaseException as exc:
                if self._db.in_transaction:self._db.execute('ROLLBACK')
                if isinstance(exc,sqlite3.Error):
                    raise AgentError('SERVICE_UNAVAILABLE','teaching journal unavailable',http_status=503) from None
                raise

    @staticmethod
    def _number(value,prefix):
        if type(value) is not str or re.fullmatch(prefix+r'-[1-9][0-9]{0,18}',value) is None:
            _deny('NOT_FOUND',404)
        number=int(value[len(prefix)+1:])
        if number>=2**63:_deny('NOT_FOUND',404)
        return number

    def _session(self,db,sid,learner):
        identifier(learner)
        row=db.execute('SELECT * FROM sessions WHERE id=?',(self._number(sid,'sess'),)).fetchone()
        if row is None:_deny('NOT_FOUND',404)
        if row['learner']!=learner:_deny('POLICY_DENIED',403)
        return row

    @staticmethod
    def _session_view(row):
        return dict(session_id='sess-'+str(row['id']),learner_id=row['learner'],
                    project_id=row['project'],revision=row['revision'])

    def create_session(self,learner,project,revision,key):
        for value in (learner,project,revision,key):identifier(value)
        key_hash=fingerprint(key)
        request_hash=fingerprint([project,revision])
        with self._tx() as db:
            row=db.execute('SELECT * FROM sessions WHERE learner=? AND key_hash=?',(learner,key_hash)).fetchone()
            if row is not None:
                if row['request_hash']!=request_hash:_deny()
            else:
                if db.execute('SELECT COUNT(*) FROM sessions').fetchone()[0]>=MAX_SESSIONS:
                    _deny('RESOURCE_EXHAUSTED',429)
                cursor=db.execute('INSERT INTO sessions(learner,project,revision,key_hash,request_hash) VALUES (?,?,?,?,?)',
                                  (learner,project,revision,key_hash,request_hash))
                row=db.execute('SELECT * FROM sessions WHERE id=?',(cursor.lastrowid,)).fetchone()
            return self._session_view(row)

    def session(self,sid,learner):
        with self._tx() as db:return self._session_view(self._session(db,sid,learner))

    def _run(self,db,rid,learner):
        row=db.execute('SELECT * FROM runs WHERE id=?',(self._number(rid,'run'),)).fetchone()
        if row is None:_deny('NOT_FOUND',404)
        session=self._session(db,'sess-'+str(row['sid']),learner)
        return dict(run_id=rid,session=self._session_view(session),scope=json.loads(row['scope']),
                    handle=json.loads(row['handle']) if row['handle'] else None)

    def run(self,rid,learner):
        with self._tx() as db:return self._run(db,rid,learner)

    def reserve_run(self,sid,learner,key,request_sha256,scope):
        identifier(key)
        if (type(request_sha256) is not str or re.fullmatch(r'[a-f0-9]{64}',request_sha256) is None
                or type(scope) is not dict or set(scope)!=SCOPE_KEYS):
            _deny('INVALID_ARGUMENT',400)
        for value in scope.values():identifier(value)
        if scope['intent'] not in ('explain','diagnose','verify') or scope['mode'] not in ('rule','deepseek'):
            _deny('INVALID_ARGUMENT',400)
        with self._tx() as db:
            session=self._session(db,sid,learner)
            key_hash=fingerprint(key)
            row=db.execute('SELECT * FROM runs WHERE sid=? AND key_hash=?',(session['id'],key_hash)).fetchone()
            if row:
                if row['request_hash']!=request_sha256 or json.loads(row['scope'])!=scope:_deny()
                rid=row['id']
            else:
                if db.execute('SELECT COUNT(*) FROM runs WHERE sid=?',(session['id'],)).fetchone()[0]>=MAX_RUNS:
                    _deny('RESOURCE_EXHAUSTED',429)
                cursor=db.execute('INSERT INTO runs(sid,key_hash,request_hash,scope) VALUES (?,?,?,?)',
                    (session['id'],key_hash,request_sha256,json.dumps(scope,sort_keys=True)))
                rid=cursor.lastrowid
            return self._run(db,'run-'+str(rid),learner)

    def attach(self,rid,learner,handle):
        handle=deepcopy(handle)
        with self._tx() as db:
            record=self._run(db,rid,learner)
            session,scope=record['session'],record['scope']
            expected=dict(session_id=session['session_id'],run_id=rid,instance_id=self.instance_id,
                learner_id=learner,project_id=session['project_id'],revision=session['revision'],
                **{k:scope[k] for k in ('snapshot_id','job_id','diagnostic_id','issue_id')})
            if (type(handle) is not dict or set(handle)!={'context','kind','teaching_session_id'}
                    or handle['context']!=expected or set(handle['context'])!=CONTEXT_KEYS
                    or handle['kind']!={'explain':'explain','diagnose':'report_review','verify':'plan'}[scope['intent']]):
                _deny()
            identifier(handle['teaching_session_id'])
            if record['handle'] is not None and record['handle']!=handle:_deny()
            db.execute('UPDATE runs SET handle=? WHERE id=?',(json.dumps(handle,sort_keys=True),self._number(rid,'run')))

    def close(self):
        with self._lock:
            self._closed=True
            if self._db is not None:self._db.close();self._db=None
