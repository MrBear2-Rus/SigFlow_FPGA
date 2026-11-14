"""U2 metadata handoff and history port, not a UA-10 learning database.

The receiver owns learning persistence, deduplication, profiles and deletion.
Only trusted composition can inject the receiver; no HTTP or model tool does.
"""
from concurrent.futures import Future, TimeoutError
from copy import deepcopy
import hashlib
import json
import re
from threading import Event, Lock, Thread
from time import monotonic

from ..learning import MAX_HISTORY_BYTES, bounded_history
from ..patterns import PATTERNS

SCHEMA = 'edu.u2.teaching-fact.v1'
CONCEPTS = frozenset(PATTERNS) | {'job_observation'}
ACTION_KINDS = frozenset(('hint_next','finish','refresh_evidence','execute_plan',
    'observe_verification','prediction_request','prediction_submit',
    'reference_request','reference_confirm','reference_reconcile'))
TERMINAL = frozenset(('Completed','Cancelled','Failed','BudgetExceeded'))


def concept_ids(value):
    return [value] if type(value) is str and value in CONCEPTS else []


def _id(value):
    return type(value) is str and re.fullmatch(r'[A-Za-z0-9_.:-]{1,256}',value) is not None


def _evidence_id(value):
    if type(value) is not str or not value.strip() or len(value)>8192:return False
    try:return len(value.encode('utf-8'))<=8192
    except UnicodeError:return False


_SCOPE=('instance_id','session_id','learner_id','project_id','revision','snapshot_id',
        'job_id','diagnostic_id','issue_id','policy')


def validate_fact(fact,binding):
    """Corrupt pending JSON is a storage failure, never receiver-authored data."""
    common={'schema_version','run_id','state_version','status','evidence_origin',
            'concept_ids','event_id','event_type',*_SCOPE}
    if type(fact) is not dict or fact.get('schema_version')!=SCHEMA:
        raise ValueError('invalid_teaching_fact')
    kind=fact.get('event_type')
    extra={'run_created':set(),'card_generated':{'card_index','level','producer','evidence_ids'},
           'student_action_accepted':{'action_id','action_kind'},'run_finished':{'level'}}
    if kind not in extra or set(fact)!=common|extra[kind]:raise ValueError('invalid_teaching_fact')
    if (any(fact.get(k)!=binding[k] or not _id(fact.get(k)) for k in _SCOPE)
            or not _id(fact.get('run_id')) or fact.get('evidence_origin') not in ('fixture','tool')
            or fact.get('status') not in TERMINAL|{'Running','WaitingStudent','WaitingEvidence','WaitingApproval','RecoveryRequired'}
            or type(fact.get('state_version')) is not str
            or not re.fullmatch(r'0|[1-9][0-9]{0,18}',fact['state_version'])
            or int(fact['state_version'])>=2**63):raise ValueError('invalid_teaching_fact')
    concepts=fact.get('concept_ids')
    if (type(concepts) is not list or len(concepts)>8
            or any(type(c) is not str or c not in CONCEPTS for c in concepts)
            or len(set(concepts))!=len(concepts)):raise ValueError('invalid_teaching_fact')
    subject='run'
    if kind=='student_action_accepted':
        if not _id(fact['action_id']) or fact['action_kind'] not in ACTION_KINDS:raise ValueError('invalid_teaching_fact')
        subject=fact['action_id']
    if kind=='card_generated':
        if (type(fact['card_index']) is not int or not 0<=fact['card_index']<256
                or type(fact['level']) is not int or not 1<=fact['level']<=3
                or fact['producer'] not in ('rule','model') or type(fact['evidence_ids']) is not list
                or not 1<=len(fact['evidence_ids'])<=4
                or not all(_evidence_id(i) for i in fact['evidence_ids'])):raise ValueError('invalid_teaching_fact')
        subject=str(fact['card_index'])
    if kind=='run_finished' and (fact['status'] not in TERMINAL or type(fact['level']) is not int
                                 or not 0<=fact['level']<=3):raise ValueError('invalid_teaching_fact')
    raw=json.dumps([fact['instance_id'],fact['session_id'],fact['run_id'],kind,subject],
                   separators=(',',':'),ensure_ascii=True).encode('ascii')
    if fact['event_id']!='u2fact-'+hashlib.sha256(raw).hexdigest():raise ValueError('invalid_teaching_fact')
    return fact


def project_facts(binding, state, *, action_id=None, action_kind='', created=False,first_card=0):
    """Project curated committed metadata; never copy request or card bodies."""
    if not _id(state.get('run_id')):
        return []  # Legacy storage-only rows have no teaching run to describe.
    scope = {k:binding[k] for k in ('instance_id','session_id','learner_id','project_id',
                                   'revision','snapshot_id','job_id','diagnostic_id','issue_id','policy')}
    if not all(_id(v) for v in scope.values()):
        raise ValueError('invalid_teaching_fact')
    concepts = state.get('teaching_concept_ids',[])
    if (type(concepts) is not list or len(concepts)>8
            or any(type(c) is not str or c not in CONCEPTS for c in concepts)
            or len(set(concepts))!=len(concepts)):
        raise ValueError('invalid_teaching_fact')
    common = dict(schema_version=SCHEMA, **scope, run_id=state['run_id'],
        state_version=state['state_version'], status=state['status'],
        evidence_origin=state.get('evidence_origin','fixture'), concept_ids=list(concepts))
    if common['evidence_origin'] not in ('tool','fixture'):
        raise ValueError('invalid_teaching_fact')
    result=[]
    def add(kind,subject,**extra):
        identity=[scope['instance_id'],scope['session_id'],state['run_id'],kind,subject]
        raw=json.dumps(identity,separators=(',',':'),ensure_ascii=True).encode('ascii')
        result.append(dict(common,event_id='u2fact-'+hashlib.sha256(raw).hexdigest(),event_type=kind,**extra))
    if created:
        add('run_created','run')
        return result
    if action_id and action_kind in ACTION_KINDS:
        add('student_action_accepted',action_id,action_id=action_id,action_kind=action_kind)
    for index,card in enumerate(state.get('cards',[])):
        if index<first_card or action_kind=='recover':
            continue  # Revalidation restores old cards; it does not generate them.
        if (type(card) is not dict or type(card.get('level')) is not int or not 1<=card['level']<=3
                or card.get('producer') not in ('rule','model')
                or type(card.get('evidence_ids')) not in (tuple,list)
                or not 1<=len(card['evidence_ids'])<=4 or not all(_evidence_id(i) for i in card['evidence_ids'])):
            raise ValueError('invalid_teaching_fact')
        add('card_generated',str(index),card_index=index,level=card['level'],
            producer=card['producer'],evidence_ids=list(card['evidence_ids']))
    if state['status'] in TERMINAL:
        add('run_finished','run',level=state.get('level',0))
    return result


def enqueue_facts(db,binding,state,**options):
    for fact in project_facts(binding,state,**options):
        db.execute('INSERT OR IGNORE INTO teaching_outbox(event_id,session_id,payload) VALUES (?,?,?)',
            (fact['event_id'],binding['session_id'],json.dumps(fact,ensure_ascii=True,separators=(',',':'))))


class TeachingMemoryBridge:
    """One delivery worker and at most one outstanding history read."""
    def __init__(self,store,port,*,secrets=()):
        if (not callable(getattr(port,'record_teaching_fact',None))
                or not callable(getattr(port,'get_teaching_context',None))):
            raise ValueError('invalid_teaching_memory_port')
        self.store,self.port,self.secrets=store,port,tuple(secrets)
        self._stop=Event()
        self._history_lock=Lock()
        self._history_pending=None
        self._records_pending=None
        self._records_scope=None
        self._thread=Thread(target=self._deliver,daemon=True,name='u2-fact-delivery')
        self._thread.start()

    def _deliver(self):
        retry_at = {}
        while not self._stop.is_set():
            now = monotonic()
            retry_at = {sid: due for sid, due in retry_at.items() if due > now}
            try:
                for fact in self.store.pending_teaching_facts(32, exclude_sessions=retry_at):
                    if self._stop.is_set():break
                    sid = fact['session_id']
                    if sid in retry_at:continue
                    try:
                        ack = self.port.record_teaching_fact(deepcopy(fact))
                        if (type(ack) is dict and ack.get('accepted') is True
                                and ack.get('event_id') == fact['event_id']
                                and self.store.ack_teaching_fact(fact['event_id'])):
                            continue
                    except Exception:
                        pass  # An unknown acknowledgement leaves the same event pending.
                    # Preserve order within a session, but let other sessions drain.
                    retry_at[sid] = monotonic() + 1.0
            except Exception:
                pass  # Store failures are retried on the next delivery pass.
            self._stop.wait(.25)

    def history(self,binding,concepts):
        concepts=tuple(concepts)
        if self._stop.is_set() or not concepts or any(c not in CONCEPTS for c in concepts):return ''
        scope=dict(learner_id=binding.learner_id,project_id=binding.project_id,
                   concept_ids=list(concepts),max_bytes=1024)
        if getattr(self.port,'scoped_history',False):
            from datetime import datetime, timezone
            scope.update(instance_id=binding.instance_id,session_id=binding.session_id,
                         revision=binding.revision,as_of=datetime.now(timezone.utc).isoformat(),max_bytes=MAX_HISTORY_BYTES)
        with self._history_lock:
            if self._history_pending is not None and not self._history_pending.done():return ''
            future=Future()
            self._history_pending=future
            port=self.port
            def read():
                try:future.set_result(port.get_teaching_context(**deepcopy(scope)))
                except Exception:future.set_result(None)
            Thread(target=read,daemon=True,name='u2-history-read').start()
        timeout = min(1.0, max(.01, getattr(self.port, 'history_timeout_seconds', .25)))
        try:value=future.result(timeout=timeout)
        except TimeoutError:return ''
        keys={'learner_id','project_id','concept_ids','summary'}
        if (type(value) is not dict or set(value)!=keys
                or any(value.get(k)!=scope[k] for k in ('learner_id','project_id','concept_ids'))):return ''
        text=bounded_history(value['summary'])
        if len(text.encode('utf-8'))>scope['max_bytes']:return ''
        return '' if any(secret and secret in text for secret in self.secrets) else text

    def close(self,timeout=1):
        self._stop.set()
        self._thread.join(max(0,timeout))

    def records(self, binding, *, state_version=None):
        read = getattr(self.port, 'get_learning_records', None)
        if not callable(read):
            return dict(status='unavailable', records=[], truncated=False)
        current = getattr(self.port, 'learning_records_current', None)
        scope = (binding, state_version)
        resumed = False
        with self._history_lock:
            if self._stop.is_set():
                return dict(status='unavailable', records=[], truncated=False)
            if self._records_pending is not None and not self._records_pending.done():
                return dict(status='pending', records=[], truncated=False)
            if self._records_pending is not None and self._records_scope == scope and callable(current):
                future = self._records_pending
                self._records_pending = self._records_scope = None
                resumed = True
            else:
                future = Future()
                self._records_pending = future
                self._records_scope = None
                def query():
                    try:
                        future.set_result(read(binding))
                    except Exception:
                        future.set_result(dict(status='unavailable', records=[], truncated=False))
                Thread(target=query, daemon=True, name='u2-records-read').start()
        try:
            result = future.result(timeout=1.0)
            with self._history_lock:
                if self._records_pending is future:
                    self._records_pending = self._records_scope = None
        except TimeoutError:
            with self._history_lock:
                if self._records_pending is future:
                    self._records_scope = scope
            return dict(status='pending', records=[], truncated=False)
        if type(result) is not dict or type(result.get('records')) is not list:
            return dict(status='unavailable', records=[], truncated=False)
        if resumed and result.get('status') == 'ready':
            try:
                if not current(binding, result):
                    return dict(status='unavailable', records=[], truncated=False)
            except Exception:
                return dict(status='unavailable', records=[], truncated=False)
        return result
