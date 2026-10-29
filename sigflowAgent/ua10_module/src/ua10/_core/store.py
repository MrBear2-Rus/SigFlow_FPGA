"""Independent derived layer: one SQLite transaction owns events and dependencies.

Native LightMem remains a separate recoverable projection; it never owns facts.
An outbox revision invalidates a projection; reads check authority and fail closed.
"""
from .util import *
import sqlite3, threading, copy
from datetime import datetime, timezone, timedelta
def utc(value):
    d=datetime.fromisoformat(value.replace('Z','+00:00'))
    if d.tzinfo is None: raise ValueError('timezone required')
    return d.astimezone(timezone.utc).isoformat(timespec='microseconds')
def after(value, seconds=1): return (datetime.fromisoformat(utc(value))+timedelta(seconds=seconds)).isoformat()
class ScopeViolation(PermissionError): pass
class EvidenceViolation(ValueError): pass
def synchronized(fn):
    def call(self,*args,**kwargs):
        with self.lock: return fn(self,*args,**kwargs)
    return call
class Store:
    def __init__(self,path):
        self.path=Path(path); self.path.parent.mkdir(parents=True,exist_ok=True)
        self.db=sqlite3.connect(self.path,check_same_thread=False)
        self.db.execute('PRAGMA journal_mode=WAL'); self.db.execute('PRAGMA synchronous=FULL')
        self.db.execute('PRAGMA foreign_keys=ON'); self.db.execute('PRAGMA secure_delete=ON')
        self.db.executescript('''
          CREATE TABLE IF NOT EXISTS events(id TEXT PRIMARY KEY,scope TEXT,kind TEXT,concept TEXT,
            occurred TEXT,available TEXT,data TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS scoped_time ON events(scope,kind,available,occurred);
          CREATE TABLE IF NOT EXISTS patterns(id TEXT PRIMARY KEY,scope TEXT,kind TEXT,concept TEXT,
            created TEXT,valid_until TEXT,expires TEXT,data TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS dependencies(pattern TEXT REFERENCES patterns(id) ON DELETE CASCADE,
            event TEXT REFERENCES events(id) ON DELETE CASCADE,PRIMARY KEY(pattern,event));
          CREATE TABLE IF NOT EXISTS tombstones(id TEXT PRIMARY KEY,deleted_at TEXT);
          CREATE TABLE IF NOT EXISTS projection_outbox(event TEXT PRIMARY KEY,operation TEXT);
          CREATE TABLE IF NOT EXISTS queries(id TEXT PRIMARY KEY,data TEXT);
        ''')
        self.lock=threading.RLock()
    def _tx(self): self.db.execute('BEGIN IMMEDIATE')
    def append(self,e):
        e=copy.deepcopy(e)
        if len(e['scope'])!=3 or e['source_kind'] not in ('real','mock','imported_unverified'): raise EvidenceViolation('invalid trust')
        if not 0<=len(e['concept_ids'])<=16: raise EvidenceViolation('at most sixteen concepts required')
        e['created_at']=utc(e['created_at']); e['available_at']=utc(e['available_at'])
        if e['created_at']>e['available_at']: raise EvidenceViolation('available before occurrence')
        scope=serial(e['scope']); kind=e['source_kind']; concept=e['concept_ids'][0] if e['concept_ids'] else '__unmapped__'; raw=serial(e)
        with self.lock:
            try:
                self._tx()
                if self.db.execute('SELECT 1 FROM tombstones WHERE id=?',(e['event_id'],)).fetchone(): raise EvidenceViolation('deleted event cannot resurrect')
                old=self.db.execute('SELECT data FROM events WHERE id=?',(e['event_id'],)).fetchone()
                if old:
                    if old[0]!=raw: raise EvidenceViolation('append-only event conflict; append a correction with a new ID')
                    self.db.rollback(); return False
                # An arriving event invalidates live summaries at availability time, including late reports.
                self.db.execute('UPDATE patterns SET valid_until=? WHERE scope=? AND kind=? AND concept=? AND valid_until IS NULL',
                  (e['available_at'],scope,kind,concept))
                self.db.execute('INSERT INTO events VALUES (?,?,?,?,?,?,?)',
                  (e['event_id'],scope,kind,concept,e['created_at'],e['available_at'],raw))
                self.db.execute('INSERT OR REPLACE INTO projection_outbox VALUES (?,?)',(e['event_id'],'append'))
                self.db.commit(); return True
            except BaseException: self.db.rollback(); raise
    @synchronized
    def events(self,scope,as_of,source_kind='real'):
        t=utc(as_of)
        return [json.loads(r[0]) for r in self.db.execute(
          'SELECT data FROM events WHERE scope=? AND kind=? AND occurred<=? AND available<=? ORDER BY occurred,id',
          (serial(list(scope)),source_kind,t,t))]
    @synchronized
    def event(self,eid):
        r=self.db.execute('SELECT data FROM events WHERE id=?',(eid,)).fetchone()
        return json.loads(r[0]) if r else None
    def delete(self,scope,eid,now):
        with self.lock:
            try:
                self._tx(); e=self.event(eid)
                if e and tuple(e['scope'])!=tuple(scope): raise ScopeViolation('delete outside binding')
                # Remove whole derived records, not just their orphaned dependency rows.
                self.db.execute('DELETE FROM patterns WHERE id IN (SELECT pattern FROM dependencies WHERE event=?)',(eid,))
                self.db.execute('DELETE FROM events WHERE id=?',(eid,))
                self.db.execute('INSERT OR REPLACE INTO tombstones VALUES (?,?)',(eid,utc(now)))
                self.db.execute('INSERT OR REPLACE INTO projection_outbox VALUES (?,?)',(eid,'delete'))
                self.db.commit()
            except BaseException: self.db.rollback(); raise
    def derive(self,scope,now,source_kind='real',ttl_days=90):
        now=utc(now)
        if not 1<=ttl_days<=365: raise ValueError('bounded expiry')
        with self.lock:
            try:
                self._tx(); events=self.events(scope,now,source_kind); groups={}
                for e in events: groups.setdefault((e['concept_ids'][0],e['diagnostic_type']),[]).append(e)
                made=[]
                for (concept,diag),group in groups.items():
                    observed=[e for e in group if e['event_type']=='issue_observed' and e['diagnostic_present']]
                    if len({e['exercise_id'] for e in observed})<2: continue
                    existing=self.db.execute('SELECT id FROM patterns WHERE scope=? AND kind=? AND concept=? AND valid_until IS NULL AND expires>?',
                      (serial(list(scope)),source_kind,concept,now)).fetchone()
                    refs=sorted(e['event_id'] for e in group)
                    if existing:
                        previous=json.loads(self.db.execute('SELECT data FROM patterns WHERE id=?',existing).fetchone()[0])
                        if previous['supporting_event_ids']==refs: made.append(previous); continue
                    shown=sum(e['event_type']=='hint_presented' for e in group)
                    generated=sum(e['event_type']=='u2_card_generated' for e in group)
                    passes=sum(e['event_type']=='retry_passed' for e in group)
                    fails=sum(e['event_type']=='retry_failed' for e in group)
                    summary=(f'synthetic 派生总结：{concept}/{diag} 在 {len({e["exercise_id"] for e in observed})} 个不同练习中有诊断观察；'
                      f'记录提示生成 {generated} 次、UI 展示回执 {shown} 次、同范围复测通过 {passes} 次、失败 {fails} 次。'
                      '各 run、issue 和 revision 保持独立；历史通过不代表当前版本通过。没有帮助记录不证明独立完成；不能据此推断掌握。')
                    if source_kind!='real': summary+=' 此来源未核验或模拟，不能作为真实学习成功。'
                    pid='pattern-'+digest([scope,source_kind,refs,now])[:20]
                    p=dict(pattern_id=pid,trusted_scope=list(scope),concept_ids=[concept],diagnostic_types=[diag],
                      supporting_event_ids=refs,supporting_run_ids=sorted({e['run_id'] for e in group}),
                      time_range=[min(e['created_at'] for e in group),max(e['created_at'] for e in group)],
                      derived_summary=summary,uncertainty=['mastery unknown','independence unknown','relation only; not issue identity'],
                      model_id='deterministic-evidence-counts-v1',prompt_version='no-llm-pattern-v1',source_kind=source_kind,
                      created_at=now,updated_at=now,expires_at=after(now,ttl_days*86400),synthetic=True,
                      counts=dict(distinct_exercises=len({e['exercise_id'] for e in observed}),shown=shown,generated=generated,passes=passes,fails=fails))
                    self.db.execute('INSERT OR IGNORE INTO patterns VALUES (?,?,?,?,?,NULL,?,?)',
                      (pid,serial(list(scope)),source_kind,concept,now,p['expires_at'],serial(p)))
                    self.db.executemany('INSERT OR IGNORE INTO dependencies VALUES (?,?)',[(pid,eid) for eid in refs])
                    made.append(p)
                self.db.commit(); return made
            except BaseException: self.db.rollback(); raise
    @synchronized
    def patterns(self,scope,as_of,source_kind='real',concept=None):
        t=utc(as_of); result=[]
        for raw in self.db.execute('''SELECT data,created,expires,valid_until FROM patterns WHERE scope=? AND kind=? AND created<=?
            AND (valid_until IS NULL OR valid_until>?) AND expires>?''',(serial(list(scope)),source_kind,t,t,t)):
            p=json.loads(raw[0])
            # 派生内容自身也有时间边界，不能仅检查它引用的原始事件。
            # 旧版本完整记录以创建时间作为首次生效/可用时间；缺失或冲突
            # 的时间无法证明历史可用性，直接隔离，不猜测、不写回补值。
            try:
                created=utc(p['created_at']);updated=utc(p['updated_at']);expires=utc(p['expires_at'])
                available=utc(p.get('available_at',created));valid_from=utc(p.get('valid_from',created))
                if created!=utc(raw[1]) or expires!=utc(raw[2]):continue
                if raw[3] is not None and utc(raw[3])<=created:continue
                if min(updated,available,valid_from)<created or expires<=created:continue
                if max(created,updated,available,valid_from)>t:continue
            except (KeyError,TypeError,ValueError,AttributeError):
                continue
            if concept and concept not in p['concept_ids']: continue
            allowed={e['event_id'] for e in self.events(scope,t,source_kind)}
            if not set(p['supporting_event_ids'])<=allowed: raise EvidenceViolation('derived dependency invalid')
            result.append(p)
        return sorted(result,key=lambda p:p['pattern_id'])
    def rebuild_patterns(self,scope,now,source_kind='real'):
        # Deliberate rebuild discards historical summaries. It does NOT backdate a new summary.
        with self.db: self.db.execute('DELETE FROM patterns WHERE scope=? AND kind=?',(serial(list(scope)),source_kind))
        return self.derive(scope,now,source_kind)
    def close(self):
        self.db.execute('PRAGMA wal_checkpoint(TRUNCATE)'); self.db.close()
