"""合成工具元数据、真实 Runtime/outbox/SQLite，模型全为 Fake。"""
from dataclasses import replace
from pathlib import Path
import json
import tempfile
from threading import Event
import time
import unittest

from sigflow_teaching_core.domain import ProviderError, Evidence
from sigflow_teaching_core.context.models import EvidenceAnchor
from sigflow_teaching_core.host.runtime import Runtime
from sigflow_teaching_core.host.store import DurableStore
from sigflow_teaching_core.host.ua10 import U2TeachingMemoryPort, BudgetedJSONService
from sigflow_teaching_core.loop import TeachingLoop
from sigflow_teaching_core.runtime.session import TeachingSession
from ua10 import MemoryConfig, TeachingMemory


class ReintegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config = MemoryConfig(Path(self.tmp.name)/'memory')
        self.store = DurableStore(Path(self.tmp.name)/'host.sqlite', 'inst')
        self.contexts = []
        self.port = U2TeachingMemoryPort(self.config, instance_id='inst', concepts=('latch',), evidence_origin='tool')
        class SyntheticToolSource:
            def collect(self, project, revision):
                anchor=EvidenceAnchor('synthetic-evidence',project,revision,'synthetic-job','snap','synthetic-artifact','a'*64)
                return Evidence('synthetic-evidence',project,revision,'synthetic-job','latch',True,
                    '合成工具契约输入，无真实学生或工程',True,'tool',anchor=anchor)
        source = SyntheticToolSource()
        owner = self
        class Fake:
            def generate(self, context):
                owner.contexts.append(context)
                return dict(level=context.level, hint='核对目标', question='需要保持旧值吗？', evidence_ids=[context.evidence.evidence_id])
        def factory(binding, request, mode, reserve, **kw):
            return TeachingSession(TeachingLoop(request, source, Fake()))
        factory.model_available = True
        self.factory = factory
        self.runtime = Runtime(self.store, 'inst', factory, evidence_origin='tool', teaching_memory=self.port)
        self.addCleanup(self.close)
        self.body = dict(learner_id='learner', project_id='p', revision='r', snapshot_id='snap',
            job_id='synthetic-job', diagnostic_id='diag', issue_id='issue', question='锁存器历史提示', mode='deepseek')

    def close(self):
        self.runtime.close(); self.store.close(); self.port.close()

    def wait(self, sid, action='create'):
        end = time.monotonic()+5
        while time.monotonic()<end:
            if self.store.action(sid,action)['status']=='committed': return self.runtime.snapshot(sid,'learner')
            time.sleep(.02)
        self.fail('工作流未提交')

    def drain(self):
        end = time.monotonic()+5
        while self.store.pending_teaching_facts(32) and time.monotonic()<end: time.sleep(.03)
        self.assertEqual(self.store.pending_teaching_facts(32), [])

    def create(self, key):
        sid = self.runtime.create(self.body,key)['session_id']
        return sid,self.wait(sid)

    def query(self, **changes):
        args = dict(learner_id='learner', project_id='p', instance_id='inst', session_id='query',
                    revision='r2', concept_ids=['latch'], max_bytes=1536, as_of='2099-01-01T00:00:00Z')
        args.update(changes)
        return self.port.get_teaching_context(**args)['summary']

    def test_outbox_cross_session_and_actual_model_context(self):
        sid,state = self.create('one'); self.drain()
        self.runtime.action(sid,'learner','hint_next','hint',int(state['state_version']))
        state=self.wait(sid,'hint'); self.drain()
        self.runtime.action(sid,'learner','finish','finish',int(state['state_version']))
        self.wait(sid,'finish'); self.drain()
        self.contexts.clear()
        self.create('two'); self.drain()
        text=self.contexts[0].learning_history
        self.assertTrue(text)
        self.assertIn('u2_card_generated',text)
        self.assertNotIn('retry_passed',text)
        self.assertLessEqual(len(text.encode()),1024)
        self.assertEqual(self.contexts[0].evidence.origin,'tool')

    def test_scope_time_and_reopen(self):
        self.create('one'); self.drain()
        self.assertTrue(self.query())
        self.assertEqual(self.query(learner_id='other',session_id='other-learner-query'), '')
        self.assertEqual(self.query(project_id='other',session_id='other-project-query'), '')
        self.assertEqual(self.query(as_of='2000-01-01T00:00:00Z'), '')
        with self.assertRaises(ValueError): self.query(instance_id='other')
        self.port.close()
        self.port=U2TeachingMemoryPort(self.config, instance_id='inst', concepts=('latch',), evidence_origin='tool')
        self.assertTrue(self.query())

    def test_local_projection_latency_does_not_silently_drop_history(self):
        sid, state = self.create('one')
        self.runtime.action(sid, 'learner', 'finish', 'finish', int(state['state_version']))
        self.wait(sid, 'finish')
        self.drain()
        def local_projection(**scope):
            time.sleep(.3)
            return dict(learner_id=scope['learner_id'], project_id=scope['project_id'],
                        concept_ids=scope['concept_ids'], summary='u2_card_generated: prior bounded teaching fact')
        self.port.get_teaching_context = local_projection
        self.contexts.clear()
        self.create('two')
        self.assertIn('u2_card_generated', self.contexts[0].learning_history)

    def test_lost_ack_retries_same_id_and_does_not_duplicate(self):
        original=self.port.record_teaching_fact
        calls=[]
        def lost(fact):
            result=original(fact); calls.append(fact['event_id'])
            if len(calls)==1: raise OSError('synthetic lost ack after durable commit')
            return result
        self.port.record_teaching_fact=lost
        sid,_=self.create('one');self.drain()
        self.assertEqual(calls[0],calls[1])
        binding=dict(learner_id='learner',project_id='p',instance_id='inst',session_id=sid)
        with TeachingMemory(self.config,trusted_binding=binding,concepts=('latch',),execution_mode='real') as m:
            rows=m._host.authority.v1.repo.events('learner',session_id=sid)
            self.assertEqual(len(rows),2)

    def test_rejected_delivery_does_not_block_other_sessions(self):
        original = self.port.record_teaching_fact
        rejected = Event()
        def reject_one(fact):
            if fact['learner_id'] == 'rejected':
                rejected.set()
                raise ValueError('synthetic receiver rejection')
            return original(fact)
        self.port.record_teaching_fact = reject_one
        blocked = self.runtime.create(dict(self.body, learner_id='rejected'), 'blocked')['session_id']
        self.assertTrue(rejected.wait(4))
        self.runtime.cancel(blocked, 'rejected')
        sid, _ = self.create('healthy')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            rows = self.store.pending_teaching_facts(32)
            if rows and all(f['learner_id'] == 'rejected' for f in rows):
                break
            time.sleep(.03)
        self.assertTrue(rows)
        self.assertTrue(all(f['learner_id'] == 'rejected' for f in rows))
        result = self.runtime.learning_records(sid, 'learner')
        self.assertEqual(result['status'], 'ready')
        self.assertIn('u2_card_generated', [row['event_type'] for row in result['records']])

    def test_u2_learner_id_with_leading_hyphen_can_read_own_records(self):
        sid = self.runtime.create(dict(self.body, learner_id='-learner'), 'one')['session_id']
        deadline = time.monotonic() + 5
        while self.store.action(sid, 'create')['status'] != 'committed' and time.monotonic() < deadline:
            time.sleep(.02)
        self.drain()
        result = self.runtime.learning_records(sid, '-learner')
        self.assertEqual(result['status'], 'ready')
        self.assertIn('u2_card_generated', [row['event_type'] for row in result['records']])
        self.runtime.cancel(sid, '-learner')
        other, _ = self.create('other')
        self.drain()
        other_result = self.runtime.learning_records(other, 'learner')
        self.assertTrue(other_result['records'])
        self.assertTrue({r['event_id'] for r in result['records']}.isdisjoint(
            r['event_id'] for r in other_result['records']))

    def test_slow_records_are_delivered_once_with_current_authority(self):
        sid, _ = self.create('one')
        self.drain()
        original = self.port.get_learning_records
        entered, release = Event(), Event()
        reads = []
        def delayed(binding):
            data = original(binding)
            reads.append(data)
            entered.set()
            release.wait(4)
            return data
        self.port.get_learning_records = delayed
        try:
            self.assertEqual(self.runtime.learning_records(sid, 'learner')['status'], 'pending')
            self.assertTrue(entered.wait(2))
            release.set()
            self.runtime._teaching_memory._records_pending.result(timeout=2)
            self.assertEqual(self.runtime.learning_records(sid, 'learner'), reads[0])
            self.assertEqual(len(reads), 1)
        finally:
            release.set()

    def test_completed_records_fail_authority_check_after_event_revocation(self):
        from sigflow_teaching_core.runtime.gateway_session import SessionBinding
        from ua10 import MemoryHost
        from memory import Scope
        sid, _ = self.create('one')
        self.drain()
        binding = SessionBinding(**self.store.get(sid)['binding'])
        records = self.port.get_learning_records(binding)
        self.assertTrue(records['records'])
        self.assertTrue(self.port.learning_records_current(binding, records))
        scope = Scope(binding.learner_id, binding.project_id, binding.instance_id, sid)
        with MemoryHost(self.config) as host:
            host.bind(scope, source_kind='real').revoke_learning_event(records['records'][0]['event_id'])
        self.assertFalse(self.port.learning_records_current(binding, records))

    def test_final_records_wait_for_outbox_delivery(self):
        sid, state = self.create('one')
        self.drain()
        original = self.port.record_teaching_fact
        release = Event()
        def delayed(fact):
            release.wait(4)
            return original(fact)
        self.port.record_teaching_fact = delayed
        try:
            self.runtime.action(sid, 'learner', 'finish', 'finish', int(state['state_version']))
            self.wait(sid, 'finish')
            self.assertEqual(self.runtime.learning_records(sid, 'learner')['status'], 'pending')
        finally:
            release.set()
        self.drain()
        records = self.runtime.learning_records(sid, 'learner')
        self.assertIn('u2_run_ended', [row['event_type'] for row in records['records']])

    def test_record_limit_keeps_the_current_end_event(self):
        for index in range(4):
            sid, state = self.create(f'run-{index}')
            self.runtime.action(sid, 'learner', 'finish', 'finish', int(state['state_version']))
            self.wait(sid, 'finish')
            self.drain()
        with self.store._transaction() as db:
            last_fact = json.loads(db.execute('SELECT payload FROM teaching_outbox WHERE session_id=? '
                                              'ORDER BY rowid DESC LIMIT 1', (sid,)).fetchone()[0])
        self.assertEqual(last_fact['event_type'], 'run_finished')
        records = self.runtime.learning_records(sid, 'learner')
        deadline = time.monotonic() + 8
        while records['status'] == 'pending' and time.monotonic() < deadline:
            records = self.runtime.learning_records(sid, 'learner')
            time.sleep(.05)
        self.assertEqual(records['status'], 'ready')
        self.assertLessEqual(len(records['records']), 12)
        self.assertTrue(records['truncated'])
        self.assertIn(last_fact['event_id'], [row['event_id'] for row in records['records']])

    def test_host_json_service_denies_before_call_and_never_retries(self):
        class Provider:
            calls=0
            def complete_json(self,messages,*,max_tokens,reserve):
                reserve(input_bytes=12,max_output_tokens=max_tokens)
                self.calls+=1
                return {'ok':True}
        p=Provider(); reservations=[]
        service=BudgetedJSONService(p,lambda **kw:reservations.append(kw),authorized=False)
        with self.assertRaises(PermissionError): service.complete_json([],max_tokens=128)
        self.assertEqual(p.calls,0)
        service=BudgetedJSONService(p,lambda **kw:reservations.append(kw),authorized=True)
        self.assertTrue(service.complete_json([],max_tokens=128)['ok'])
        self.assertEqual((p.calls,len(reservations)),(1,1))
        def deny(**kw):raise ProviderError('model_budget_exceeded')
        with self.assertRaises(ProviderError): BudgetedJSONService(p,deny,authorized=True).complete_json([],max_tokens=128)
        self.assertEqual(p.calls,1)

    def test_budget_512_1024_1536_and_no_partial_fact_slicing(self):
        sid,state=self.create('one');self.drain()
        for action in ('hint-one','hint-two','finish'):
            kind='finish' if action=='finish' else 'hint_next'
            self.runtime.action(sid,'learner',kind,action,int(state['state_version']))
            state=self.wait(sid,action);self.drain()
        results=[]
        for budget in (512,1024,1536):
            port=U2TeachingMemoryPort(replace(self.config,default_token_budget=budget),instance_id='inst',concepts=('latch',),evidence_origin='tool')
            text=port.get_teaching_context(learner_id='learner',project_id='p',instance_id='inst',session_id='budget-query',
                revision='r2',concept_ids=['latch'],max_bytes=1536,as_of='2099-01-01T00:00:00Z')['summary']
            self.assertLessEqual(len(text.encode()),budget)
            dto=json.loads(text) if text else {}
            self.assertTrue(all(len(row)==len(dto['columns']) for row in dto.get('facts',[])))
            results.append(dict(budget=budget,characters=len(text),estimated_tokens=len(text.encode()),
                facts=len(dto.get('facts',[])),truncated=dto.get('truncated',False)))
            port.close()
        print('MEMORY_BUDGET_RESULTS='+json.dumps(results))
        with self.assertRaises(ValueError):
            U2TeachingMemoryPort(replace(self.config,default_token_budget=1537),instance_id='inst',concepts=('latch',))

    def test_source_mutation_and_deleted_session_cannot_replay(self):
        sid,_=self.create('one');self.drain()
        with self.store._transaction() as db:
            fact=json.loads(db.execute('SELECT payload FROM teaching_outbox WHERE session_id=? ORDER BY rowid LIMIT 1',(sid,)).fetchone()[0])
        changed=dict(fact,status='WaitingStudent')
        with self.assertRaises(ValueError):self.port.record_teaching_fact(changed)
        changed=dict(fact,evidence_origin='fixture')
        with self.assertRaises(ValueError):self.port.record_teaching_fact(changed)
        with TeachingMemory(self.config,trusted_binding=fact,concepts=('latch',),execution_mode='real') as memory:
            memory._api.delete_session()
        with self.assertRaises(ValueError):self.port.record_teaching_fact(fact)

    def test_factory_pattern_provider_reuses_actual_config_and_host_reservation(self):
        from unittest.mock import patch
        from types import SimpleNamespace
        from sigflow_teaching_core.deepseek import DeepSeekProvider, DeepSeekConfig
        from sigflow_teaching_core.host.factory import make_factory
        from sigflow_teaching_core.clients.gateway import GatewayConfig
        provider=DeepSeekProvider(DeepSeekConfig('synthetic-placeholder','host-selected-model',max_tokens=256))
        reservations=[];bodies=[]
        class Connection:
            def __init__(self,*a,**kw):pass
            def request(self,method,path,body,headers):bodies.append(json.loads(body))
            def getresponse(self):
                raw=json.dumps(dict(choices=[dict(finish_reason='stop',message=dict(content='{"ok":true}'))])).encode()
                return SimpleNamespace(status=200,read=lambda *a:raw)
            def close(self):pass
        with patch('sigflow_teaching_core.host.factory.DeepSeekProvider.from_settings',return_value=provider):
            factory=make_factory(GatewayConfig(12345,'synthetic-gateway','inst'))
        with patch('sigflow_teaching_core.deepseek.http.client.HTTPSConnection',Connection):
            denied=factory.pattern_provider(reserve=lambda **kw:reservations.append(kw))
            with self.assertRaises(PermissionError):denied.generate([],max_tokens=128)
            self.assertEqual(bodies,[])
            p=factory.pattern_provider(reserve=lambda **kw:reservations.append(kw),authorized=True)
            self.assertTrue(p.generate([],max_tokens=128)['ok'])
        self.assertEqual((len(bodies),len(reservations)),(1,1))
        self.assertEqual(bodies[0]['model'],provider.config.model)
        self.assertEqual(bodies[0]['thinking'],{'type':'disabled'})


if __name__=='__main__':unittest.main(verbosity=2)
