"""Synthetic receiver contracts against the real U2 outbox and history bridge."""
from contextlib import contextmanager
from collections import Counter
from copy import deepcopy
from pathlib import Path
import sqlite3
import tempfile
from threading import Event, Thread, get_ident
import time
import unittest

from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession


class MemoryAdapterTests(unittest.TestCase):
    def setUp(self):
        from importlib.util import find_spec
        self.assertIsNotNone(find_spec('sigflow_edu_agent.host.memory_adapter'), 'memory adapter missing')
        from sigflow_edu_agent.host import memory_adapter
        self.Adapter = memory_adapter.MemoryAdapter
        self.temp = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(self.temp.cleanup)
        self.store = DurableStore(Path(self.temp.name)/'host.sqlite', 'inst')
        self.addCleanup(self.store.close)
        self.allowed = True
        self.scopes = []
        self.calls = []
        self.threads = []
        self.reply = None
        self.after = lambda: None
        owner = self
        @contextmanager
        def factory():
            # Default SQLite thread affinity proves construction/use/close occur
            # in one operation thread, including Runtime delivery/history workers.
            connection = sqlite3.connect(':memory:')
            owner.threads.append(('open', get_ident()))
            class Receiver:
                def record_teaching_fact(self, fact):
                    connection.execute('SELECT 1')
                    owner.calls.append(deepcopy(fact))
                    owner.after()
                    result = dict(event_id=fact['event_id'], accepted=True)
                    fact['concept_ids'].append('receiver-mutated')
                    return owner.reply if owner.reply is not None else result
                def get_teaching_context(self, **scope):
                    connection.execute('SELECT 1')
                    owner.calls.append(deepcopy(scope))
                    owner.after()
                    result = {k: scope[k] for k in ('learner_id','project_id','concept_ids')}
                    return owner.reply if owner.reply is not None else dict(result,summary='synthetic history')
            try:
                yield Receiver()
            finally:
                connection.close()
                owner.threads.append(('close',get_ident()))
        self.factory = factory
        def allowed(scope):
            self.scopes.append(deepcopy(scope))
            matches = scope.get('learner_id')=='learner' and scope.get('project_id')=='p'
            scope.clear()  # Trusted callbacks also cannot mutate validated inputs.
            return self.allowed and matches
        self.adapter = self.Adapter(factory, allowed)
        self.addCleanup(self.adapter.close)
        self.last_history = None
        class Provider:
            def generate(_, context):
                self.last_history = context.learning_history
                return dict(level=context.level,hint='Inspect latch.',question='Why?',
                            evidence_ids=[context.evidence.evidence_id])
        def session_factory(binding,request,mode,reserve):
            return TeachingSession(TeachingLoop(request,FixtureSource(),Provider()))
        self.session_factory = session_factory
        self.runtime = Runtime(self.store,'inst',session_factory,evidence_origin='fixture')
        self.addCleanup(lambda: self.runtime.close())
        self.body = dict(learner_id='learner',project_id='p',revision='r',snapshot_id='snap',
            job_id='fixture-job-1',diagnostic_id='diag',issue_id='issue',
            question='Explain latch',intent='diagnose',mode='rule')

    def create(self):
        sid = self.runtime.create(self.body,'create')['session_id']
        until=time.monotonic()+4
        while time.monotonic()<until:
            if self.store.action(sid,'create')['status']=='committed':return sid
            time.sleep(.01)
        self.fail('create did not commit')

    def fact(self):
        self.create()
        return self.store.pending_teaching_facts(32)[0]

    def test_operation_factory_handles_real_runtime_workers_and_history(self):
        self.runtime.close()
        self.runtime=Runtime(self.store,'inst',self.session_factory,evidence_origin='fixture',teaching_memory=self.adapter)
        self.create()
        until=time.monotonic()+3
        while self.store.pending_teaching_facts(32) and time.monotonic()<until:time.sleep(.02)
        self.assertEqual(self.store.pending_teaching_facts(32),[])
        self.assertEqual(self.last_history,'synthetic history')
        self.assertTrue(self.threads)
        self.assertEqual(Counter(v for kind,v in self.threads if kind=='open'),
                         Counter(v for kind,v in self.threads if kind=='close'))
        self.assertTrue(all(v!=get_ident() for _,v in self.threads))

    def test_fact_and_ack_are_bounded_and_copied(self):
        fact=self.fact();before=deepcopy(fact)
        self.assertEqual(self.adapter.record_teaching_fact(fact),dict(accepted=True,event_id=fact['event_id']))
        self.assertEqual(fact,before)
        for bad in (fact|dict(question='private'),fact|dict(event_id='wrong')):
            with self.subTest(bad=bad),self.assertRaisesRegex(ValueError,'invalid_teaching_fact'):
                self.adapter.record_teaching_fact(bad)
        with self.assertRaisesRegex(ValueError,'memory_scope_denied'):
            self.adapter.record_teaching_fact(fact|dict(project_id='foreign'))
        count=len(self.calls)
        for bad in (dict(accepted=True,event_id='wrong'),dict(accepted=1,event_id=fact['event_id']),
                    dict(accepted=True,event_id=fact['event_id'],summary='secret')):
            self.reply=bad
            with self.assertRaisesRegex(ValueError,'memory_unavailable'):self.adapter.record_teaching_fact(fact)
        self.assertEqual(len(self.calls),count+3)

    def test_history_rejects_foreign_scope_and_oversize_or_unrequested_concepts(self):
        scope=dict(learner_id='learner',project_id='p',concept_ids=['latch'],max_bytes=1024)
        reply=self.adapter.get_teaching_context(**scope)
        self.assertEqual(reply['summary'],'synthetic history')
        for change in (dict(learner_id='other'),dict(project_id='other'),dict(concept_ids=['job_observation']),
                       dict(summary='x'*1025),dict(summary='bad\x00text')):
            self.reply=reply|change
            with self.subTest(change=change),self.assertRaisesRegex(ValueError,'memory_unavailable'):
                self.adapter.get_teaching_context(**scope)
        for change in (dict(concept_ids=['private']),dict(concept_ids=['latch','latch']),dict(max_bytes=True)):
            with self.assertRaisesRegex(ValueError,'invalid_memory_scope'):
                self.adapter.get_teaching_context(**(scope|change))

    def test_revocation_before_and_during_call_and_close_block_delivery(self):
        fact=self.fact()
        self.allowed=False
        with self.assertRaisesRegex(ValueError,'memory_scope_denied'):self.adapter.record_teaching_fact(fact)
        self.assertEqual(self.calls,[])
        self.allowed=True;self.after=lambda:setattr(self,'allowed',False)
        with self.assertRaisesRegex(ValueError,'memory_scope_denied'):self.adapter.record_teaching_fact(fact)
        self.assertEqual(self.threads[-1][0],'close')
        self.allowed=True;entered,release=Event(),Event()
        self.after=lambda:(entered.set(),release.wait(2))
        errors=[]
        def invoke():
            try:self.adapter.record_teaching_fact(fact)
            except ValueError as error:errors.append(str(error))
        thread=Thread(target=invoke);thread.start()
        try:
            self.assertTrue(entered.wait(1));self.adapter.close();release.set();thread.join(2)
            self.assertEqual(errors,['memory_closed'])
            with self.assertRaisesRegex(ValueError,'memory_closed'):self.adapter.record_teaching_fact(fact)
        finally:release.set();thread.join(2)

    def test_receiver_factory_and_authorizer_errors_are_sanitized(self):
        fact=self.fact()
        def fail(*args):raise OSError('private token and receiver path')
        for adapter in (self.Adapter(fail,lambda scope:True),self.Adapter(self.factory,fail)):
            with self.assertRaisesRegex(ValueError,'^memory_unavailable$'):adapter.record_teaching_fact(fact)
            adapter.close()
