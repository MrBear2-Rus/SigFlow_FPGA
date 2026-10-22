"""U2's committed-fact boundary; the receiver below is not a UA-10 implementation."""
from contextlib import closing
from dataclasses import replace
import json
from pathlib import Path
import sqlite3
import tempfile
import time
import unittest

from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore, StoreError
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession


class TeachingMemoryTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(callable(getattr(DurableStore, 'pending_teaching_facts', None)), 'committed_fact_port_missing')
        self.directory = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)/'host.sqlite3'
        self.store = DurableStore(self.path, 'inst')
        self.source = FixtureSource()
        self.provider = None
        self.runtime = self.open_runtime()
        self.addCleanup(self.close)
        self.body = dict(learner_id='learner', project_id='p', revision='r', snapshot_id='snap',
            job_id='fixture-job-1', diagnostic_id='diag', issue_id='issue',
            question='private-student-request-4983', intent='diagnose', mode='rule')

    def open_runtime(self, port=None):
        def factory(binding, request, mode, reserve, **options):
            return TeachingSession(TeachingLoop(request, self.source, self.provider))
        return Runtime(self.store, 'inst', factory, evidence_origin='fixture', teaching_memory=port)

    def close(self):
        self.runtime.close()
        self.store.close()

    def wait(self, sid, action='create'):
        until = time.monotonic()+4
        while time.monotonic() < until:
            if self.store.action(sid,action)['status'] in ('committed','interrupted'):
                return self.runtime.snapshot(sid,'learner')
            time.sleep(.01)
        self.fail('teaching operation did not commit')

    def create(self):
        sid = self.runtime.create(self.body,'create-key')['session_id']
        return sid,self.wait(sid)

    def test_committed_facts_exclude_bodies_and_include_real_scope(self):
        sid, first = self.create()
        self.runtime.action(sid,'learner','hint_next','hint',int(first['state_version']))
        second = self.wait(sid,'hint')
        self.runtime.action(sid,'learner','finish','finish',int(second['state_version']))
        final = self.wait(sid,'finish')
        facts = self.store.pending_teaching_facts(32)
        self.assertEqual([f['event_type'] for f in facts], [
            'run_created','card_generated','student_action_accepted','card_generated',
            'student_action_accepted','run_finished'])
        self.assertEqual([f['level'] for f in facts if f['event_type']=='card_generated'], [1,2])
        self.assertEqual(facts[1]['concept_ids'],['latch'])
        self.assertEqual(facts[-1]['status'],'Completed')
        self.assertEqual(final['level'],2)
        for fact in facts:
            self.assertEqual((fact['instance_id'],fact['learner_id'],fact['project_id'],fact['revision']),
                             ('inst','learner','p','r'))
            self.assertEqual((fact['session_id'],fact['job_id'],fact['snapshot_id']),
                             (sid,'fixture-job-1','snap'))
            self.assertEqual(fact['evidence_origin'],'fixture')
        raw = json.dumps(facts,ensure_ascii=False)
        self.assertNotIn(self.body['question'],raw)
        self.assertNotIn(first['cards'][0]['hint'],raw)
        self.assertNotIn(first['cards'][0]['question'],raw)
        self.assertNotIn('mastered',raw)
        self.assertEqual(len({f['event_id'] for f in facts}),len(facts))

    def test_replayed_and_rejected_actions_do_not_create_new_facts(self):
        sid,first = self.create()
        args=(sid,'learner','hint_next','hint',int(first['state_version']))
        self.runtime.action(*args)
        self.wait(sid,'hint')
        before=self.store.pending_teaching_facts(32)
        self.runtime.action(*args)
        with self.assertRaises(ValueError):
            self.runtime.action(sid,'other','hint_next','foreign',0)
        self.assertEqual(self.store.pending_teaching_facts(32),before)

    def test_failed_state_transaction_rolls_back_facts_and_publication(self):
        sid,first = self.create()
        before=self.store.pending_teaching_facts(32)
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("CREATE TRIGGER fail_facts BEFORE INSERT ON teaching_outbox BEGIN SELECT RAISE(ABORT, 'disk fault'); END")
            db.commit()
        self.store.claim(sid,'disk-fault','fingerprint',int(first['state_version']),kind='finish')
        with self.assertRaises(StoreError):
            self.store.commit(sid,'disk-fault',first | {'status':'Completed',
                'state_version':str(int(first['state_version'])+1)},None,{})
        self.assertEqual(self.runtime.snapshot(sid,'learner'),first)
        self.assertEqual(self.store.pending_teaching_facts(32),before)
        self.assertEqual(self.store.action(sid,'disk-fault')['status'],'pending')

    def test_cancel_and_reopen_preserve_ids_and_delivery_ack(self):
        sid,first=self.create()
        self.runtime.cancel(sid,'learner')
        self.runtime.cancel(sid,'learner')
        facts=self.store.pending_teaching_facts(32)
        self.assertEqual([f['event_type'] for f in facts].count('run_finished'),1)
        self.assertEqual(facts[-1]['status'],'Cancelled')
        self.assertTrue(self.store.ack_teaching_fact(facts[0]['event_id']))
        self.assertTrue(self.store.ack_teaching_fact(facts[0]['event_id']))
        self.runtime.close();self.store.close()
        self.store=DurableStore(self.path,'inst');self.runtime=self.open_runtime()
        self.assertEqual(self.store.pending_teaching_facts(32),facts[1:])
        self.assertEqual(len(self.store.pending_teaching_facts(1)),1)
        self.assertFalse(self.store.ack_teaching_fact('u2fact-'+'0'*64))
        for bad in (0,True,33,'2'):
            with self.subTest(limit=bad),self.assertRaises(StoreError):
                self.store.pending_teaching_facts(bad)

    def test_slow_external_receiver_does_not_hold_teaching_or_store_locks(self):
        from threading import Event
        entered,release=Event(),Event()
        class SlowReceiver:
            def record_teaching_fact(self,fact):
                entered.set();release.wait(3)
                return dict(event_id=fact['event_id'],accepted=True)
            def get_teaching_context(self,**scope):
                return dict(**{k:scope[k] for k in ('learner_id','project_id','concept_ids')},summary='')
        self.runtime.close();self.runtime=self.open_runtime(SlowReceiver())
        self.addCleanup(release.set)
        sid,first=self.create()
        self.assertTrue(entered.wait(1))
        started=time.monotonic()
        self.runtime.action(sid,'learner','hint_next','hint',int(first['state_version']))
        self.assertEqual(self.wait(sid,'hint')['level'],2)
        self.runtime.cancel(sid,'learner')
        self.assertLess(time.monotonic()-started,1)
        self.assertEqual(self.runtime.snapshot(sid,'learner')['status'],'Cancelled')
        release.set()

    def test_unknown_or_lost_receiver_ack_retries_same_event(self):
        class Receiver:
            def __init__(self):self.calls=[];self.seen=set()
            def record_teaching_fact(self,fact):
                self.calls.append(fact['event_id']);self.seen.add(fact['event_id'])
                if len(self.calls)==1:
                    return dict(event_id='wrong',accepted=True)
                if len(self.calls)==2:
                    raise OSError('private receiver failure')
                return dict(event_id=fact['event_id'],accepted=True)
            def get_teaching_context(self,**scope):
                return dict(**{k:scope[k] for k in ('learner_id','project_id','concept_ids')},summary='')
        receiver=Receiver()
        self.runtime.close();self.runtime=self.open_runtime(receiver)
        sid,_=self.create()
        self.runtime.cancel(sid,'learner')
        until=time.monotonic()+4
        while self.store.pending_teaching_facts(32) and time.monotonic()<until:time.sleep(.03)
        self.assertEqual(self.store.pending_teaching_facts(32),[])
        self.assertGreaterEqual(len(receiver.calls),5)
        self.assertEqual(receiver.calls[:3],[receiver.calls[0]]*3)
        self.assertEqual(len(receiver.seen),3)

    def test_opaque_valid_evidence_ids_do_not_block_teaching(self):
        original=self.source.collect
        self.source.collect=lambda *args:replace(original(*args),evidence_id='证据 id with spaces')
        sid,state=self.create()
        self.assertEqual(state['status'],'WaitingStudent')
        fact=next(f for f in self.store.pending_teaching_facts(32) if f['event_type']=='card_generated')
        self.assertEqual(fact['evidence_ids'],['证据 id with spaces'])

    def test_history_scope_is_checked_and_never_persisted_as_a_host_fact(self):
        owner=self
        class Receiver:
            foreign=False
            def record_teaching_fact(self,fact):return dict(event_id=fact['event_id'],accepted=True)
            def get_teaching_context(self,**scope):
                return dict(learner_id='foreign' if self.foreign else scope['learner_id'],
                    project_id=scope['project_id'],concept_ids=scope['concept_ids'],summary='private-learning-summary')
        class Provider:
            def generate(self,context):
                owner.last_context=context
                return dict(level=context.level,hint='检查目标。',question='需要记忆吗？',
                            evidence_ids=[context.evidence.evidence_id])
        receiver=Receiver();self.provider=Provider()
        self.runtime.close();self.runtime=self.open_runtime(receiver)
        sid,first=self.create()
        self.assertEqual(self.last_context.learning_history,'private-learning-summary')
        self.assertEqual((first['level'],first['model_calls']),(1,1))
        receiver.foreign=True
        self.runtime.action(sid,'learner','hint_next','hint',int(first['state_version']))
        second=self.wait(sid,'hint')
        self.assertEqual(self.last_context.learning_history,'')
        self.assertEqual((second['level'],second['model_calls']),(2,2))
        self.assertNotIn(b'private-learning-summary',self.path.read_bytes())

    def test_hanging_history_is_bounded_and_does_not_spawn_per_hint(self):
        from threading import Event
        release=Event()
        class Receiver:
            reads=0
            def record_teaching_fact(self,fact):return dict(event_id=fact['event_id'],accepted=True)
            def get_teaching_context(self,**scope):
                self.reads+=1;release.wait(3)
                return dict(learner_id=scope['learner_id'],project_id=scope['project_id'],
                            concept_ids=scope['concept_ids'],summary='late summary')
        receiver=Receiver();self.addCleanup(release.set)
        self.runtime.close();self.runtime=self.open_runtime(receiver)
        started=time.monotonic()
        sid,first=self.create()
        self.runtime.action(sid,'learner','hint_next','hint',int(first['state_version']))
        self.assertEqual(self.wait(sid,'hint')['level'],2)
        self.assertLess(time.monotonic()-started,1)
        self.assertEqual(receiver.reads,1)
        self.runtime.cancel(sid,'learner');release.set()

    def test_corrupt_outbox_payload_is_not_forwarded_as_teaching_data(self):
        self.create()
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("UPDATE teaching_outbox SET payload=? WHERE rowid=1",
                       (json.dumps(dict(question='private forged body')),))
            db.commit()
        with self.assertRaises(StoreError):self.store.pending_teaching_facts(32)

    def test_existing_database_migrates_without_inventing_historic_facts(self):
        sid,_=self.create()
        self.runtime.close();self.store.close()
        with closing(sqlite3.connect(self.path)) as db:
            db.execute('DROP TABLE teaching_outbox')
            db.execute('ALTER TABLE actions DROP COLUMN kind')
            db.commit()
        self.store=DurableStore(self.path,'inst');self.runtime=self.open_runtime()
        recovered=self.runtime.snapshot(sid,'learner')
        self.assertEqual(recovered['status'],'RecoveryRequired')
        self.assertEqual(self.store.pending_teaching_facts(32),[])
        self.runtime.recover(sid,'learner','recover',int(recovered['state_version']),self.body['question'])
        restored=self.wait(sid,'recover')
        self.assertEqual(restored['status'],'WaitingStudent')
        self.assertEqual(self.store.pending_teaching_facts(32),[])
        self.runtime.action(sid,'learner','hint_next','new-hint',int(restored['state_version']))
        self.wait(sid,'new-hint')
        generated=[f for f in self.store.pending_teaching_facts(32) if f['event_type']=='card_generated']
        self.assertEqual([f['level'] for f in generated],[2])
        self.runtime.cancel(sid,'learner')
        facts=self.store.pending_teaching_facts(32)
        self.assertFalse(any(f['event_type']=='run_created' for f in facts))
        self.assertEqual(facts[-1]['event_type'],'run_finished')
