"""V1 authority plus durable module intents/revocations; indexes are disposable.

Control commit -> V1 commit -> control acknowledgement -> projection refresh.
This is a replay protocol, NOT one transaction across the databases.
"""
from dataclasses import asdict,replace
from pathlib import Path
import sqlite3,json,threading
from memory import LearningService,LearningEvent,JobEvidence,CourseInteraction,U2TeachingFacts,Scope
from memory.models import identifier,timestamp,utc_now
from .schemas import Observation,WriteResult,RevocationResult
from ._core.util import serial,digest
from .errors import ConflictError,DeletedScopeError,ScopeError,ProjectionError
from .adapters.v1 import read_facts,fact_text

def decode_event(data):
    data=dict(data)
    for key,cls in [('job',JobEvidence),('course',CourseInteraction),('u2',U2TeachingFacts)]:
        if data.get(key):
            nested=dict(data[key])
            if key=='u2':nested['course_refs']=tuple(tuple(x) for x in nested.get('course_refs',()))
            data[key]=cls(**nested)
    for key in ('concept_ids','evidence_refs'):
        if key in data:data[key]=tuple(data[key])
    return LearningEvent(**data)

class EventAuthority:
    def __init__(self,directory,fault=None):
        self.directory=Path(directory);self.directory.mkdir(parents=True,exist_ok=True)
        self.v1=LearningService(self.directory/'events.sqlite')
        self.control=sqlite3.connect(self.directory/'control.sqlite');self.control.row_factory=sqlite3.Row
        self.control.execute('PRAGMA journal_mode=WAL');self.control.execute('PRAGMA synchronous=FULL')
        self.control.execute('PRAGMA secure_delete=ON')
        self.control.executescript('''CREATE TABLE IF NOT EXISTS intents(id TEXT PRIMARY KEY,fingerprint TEXT,scope TEXT,event TEXT,
            observation TEXT,available TEXT,synthetic INTEGER,state TEXT,result TEXT);
            CREATE TABLE IF NOT EXISTS revocations(id TEXT PRIMARY KEY,scope TEXT,revoked_at TEXT);
            CREATE TABLE IF NOT EXISTS adopted(id TEXT PRIMARY KEY,available TEXT);
            ''')
        self.fault=fault or (lambda phase:None)
        self.recover()
    def _row(self,eid):return self.control.execute('SELECT * FROM intents WHERE id=?',(eid,)).fetchone()
    def recover(self):
        for row in self.control.execute("SELECT * FROM intents WHERE state='pending'").fetchall():
            try:self._execute(row)
            except DeletedScopeError:
                with self.control:self.control.execute("UPDATE intents SET state='rejected' WHERE id=?",(row['id'],))
        self.purge_deleted()
    def purge_deleted(self):
        # Keep only replay-denial metadata for physically erased sessions. The
        # intent journal is not a second permanent copy of deleted event bodies.
        for row in self.control.execute("SELECT * FROM intents WHERE state!='erased'").fetchall():
            scope=json.loads(row['scope'])
            if self.v1.ledger.blocks(scope['profile_id'],scope['session_id']):
                actual=json.loads(row['result'])['event_id'] if row['result'] else row['id']
                with self.control:
                    self.control.execute("UPDATE intents SET event='{}',observation=NULL,result=NULL,state='erased' WHERE id=?",(row['id'],))
                    self.control.execute('DELETE FROM adopted WHERE id=?',(actual,))
    def _execute(self,row):
        scope=Scope(**json.loads(row['scope']));self.v1.repo.check_scope(scope)
        if self.control.execute('SELECT 1 FROM revocations WHERE id=?',(row['id'],)).fetchone():raise ConflictError('revoked event cannot be replayed')
        event=decode_event(json.loads(row['event']));obs=json.loads(row['observation']) if row['observation'] else None
        try:
            if obs:
                result=self.v1.observe_issue(scope,event.event_id,event.run_id,obs['diagnostic_type'],obs['exercise_id'],event.concept_ids,event.job,
                    logical_object=obs['logical_object'],observation_key=obs['observation_key'],evidence_refs=event.evidence_refs,created_at=event.created_at)
            else:result=self.v1.record_learning_event(scope,event)
        except Exception:
            with self.control:self.control.execute("UPDATE intents SET state='rejected' WHERE id=?",(row['id'],))
            raise
        self.fault('after_source_commit')
        with self.control:self.control.execute("UPDATE intents SET state='committed',result=? WHERE id=?",(serial(result),row['id']))
        return result
    def append(self,scope,event,observation=None,*,available_at=None,synthetic=False):
        self.v1.repo.check_scope(scope);self.v1.repo.check_run(scope,event.run_id)
        if not isinstance(event,LearningEvent):raise TypeError('LearningEvent required')
        if type(synthetic) is not bool:raise TypeError('synthetic must be bool')
        if (event.event_type=='issue_observed')!=(observation is not None):raise ValueError('issue_observed requires Observation; other events must not include one')
        original=event.payload();scope_data=asdict(scope)
        fingerprint=digest([scope_data,original,asdict(observation) if observation else None,available_at,synthetic])
        row=self._row(event.event_id)
        if row:
            if row['fingerprint']!=fingerprint:raise ConflictError('immutable event ID reused with different payload')
            if row['state']=='rejected':raise ConflictError('rejected intent: use a new event ID after correcting input')
        else:
            observed=timestamp(event.created_at or utc_now());available=timestamp(available_at or max(observed,utc_now()))
            if observed>available:raise ValueError('availability precedes event occurrence')
            effective=replace(event,created_at=observed)
            with self.control:
                self.control.execute('INSERT INTO intents VALUES (?,?,?,?,?,?,?,?,NULL)',(event.event_id,fingerprint,serial(scope_data),serial(effective.payload()),
                    serial(asdict(observation)) if observation else None,available,int(synthetic),'pending'))
            row=self._row(event.event_id)
        if self.control.execute('SELECT 1 FROM revocations WHERE id=?',(event.event_id,)).fetchone():raise ConflictError('revoked event cannot resurrect')
        result={**json.loads(row['result']),'recorded':False} if row['state']=='committed' else self._execute(row)
        actual=result['event_id']
        fact=next((f for f in self.facts(scope) if f['event_id']==actual),None)
        return WriteResult(actual,result['recorded'],fact.get('issue_id') if fact else result.get('issue_id'),
            fact.get('occurrence_id') if fact else result.get('occurrence_id'))
    def facts(self,scope):
        self.v1.repo.check_scope(scope);facts=read_facts(self.v1,scope);result=[]
        metadata={}
        for row in self.control.execute("SELECT * FROM intents WHERE state='committed'"):
            actual=json.loads(row['result'])['event_id'];metadata.setdefault(actual,row)
        revoked={r[0] for r in self.control.execute('SELECT id FROM revocations')}
        for f in facts:
            if f['event_id'] in revoked:continue
            meta=metadata.get(f['event_id'])
            if meta:
                f['available_at']=meta['available'];f['synthetic']=bool(meta['synthetic'])
            else:
                prior=self.control.execute('SELECT available FROM adopted WHERE id=?',(f['event_id'],)).fetchone()
                if prior is None:
                    value=max(f['created_at'],utc_now())
                    with self.control:self.control.execute('INSERT INTO adopted VALUES (?,?)',(f['event_id'],value))
                    f['available_at']=value
                else:f['available_at']=prior[0]
                f['synthetic']=False
            issue=self.v1.db.connection.execute('SELECT goal_id FROM issues WHERE issue_id=?',(f['issue_id'],)).fetchone()
            f['exercise_id']=issue[0] if issue else 'unknown'
            f['diagnostic_present']=True if f['event_type'] in ('issue_observed','u2_diagnostic_observed') else (
                False if f['validation']=='diagnostic_absence' and f['outcome']=='passed' else None)
            f['summary']=fact_text(f);result.append(f)
        return result
    def revoke(self,scope,eid):
        identifier(eid);self.v1.repo.check_scope(scope)
        f=next((f for f in read_facts(self.v1,scope) if f['event_id']==eid),None)
        if f is None:raise ScopeError('event not owned by bound learner/project/instance')
        scope_key=serial([scope.profile_id,scope.project_id,scope.instance_id])
        with self.control:self.control.execute('INSERT OR IGNORE INTO revocations VALUES (?,?,?)',(eid,scope_key,utc_now()))
        self.fault('after_revocation_commit')
        return RevocationResult(eid,True)
    def close(self):
        self.control.execute('PRAGMA wal_checkpoint(TRUNCATE)');self.control.close();self.v1.close()
