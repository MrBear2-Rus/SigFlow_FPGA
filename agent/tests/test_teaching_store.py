"""U1 association journal never stores teaching content or authority tokens."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

from sigflow_edu_agent.errors import AgentError


class TeachingStoreTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.teaching_store'),
                             'U1 teaching association store missing')
        from sigflow_edu_agent.teaching_store import BindingStore
        self.type=BindingStore
        root=Path(__file__).resolve().parents[2]/'.superpowers/sdd/u1-teaching-tests/store'
        root.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'bindings.sqlite'
        self.store=self.type(self.path,'inst')
        self.addCleanup(lambda:self.store.close())
        self.scope=dict(job_id='job',snapshot_id='snapshot',diagnostic_id='diag',
                        issue_id='issue',intent='diagnose',mode='rule')

    def session(self,key='session-create'):
        return self.store.create_session('learner','project','revision',key)

    def reserve(self,sid,key='run-create',digest='a'*64):
        return self.store.reserve_run(sid,'learner',key,digest,self.scope)

    def handle(self,sid,rid):
        return dict(context=dict(session_id=sid,run_id=rid,instance_id='inst',learner_id='learner',
            project_id='project',revision='revision',snapshot_id='snapshot',job_id='job',
            diagnostic_id='diag',issue_id='issue'),teaching_session_id='teaching-internal',kind='report_review')

    def test_creation_and_association_survive_reopen_without_reusing_ids(self):
        sid=self.session()['session_id']
        run=self.reserve(sid)
        handle=self.handle(sid,run['run_id'])
        self.store.attach(run['run_id'],'learner',handle)
        self.store.close();self.store=self.type(self.path,'inst')
        self.assertEqual(self.session()['session_id'],sid)
        self.assertEqual(self.reserve(sid)['handle'],handle)
        self.assertEqual(self.store.run(run['run_id'],'learner')['handle'],handle)
        self.assertNotEqual(self.session('other')['session_id'],sid)
        self.assertNotEqual(self.reserve(sid,'other')['run_id'],run['run_id'])

    def test_changed_request_is_conflict_and_other_learner_cannot_read(self):
        sid=self.session()['session_id'];run=self.reserve(sid)
        for invoke,code in (
            (lambda:self.store.create_session('learner','other','revision','session-create'),'IDEMPOTENCY_CONFLICT'),
            (lambda:self.reserve(sid,digest='b'*64),'IDEMPOTENCY_CONFLICT'),
            (lambda:self.store.session(sid,'foreign'),'POLICY_DENIED'),
            (lambda:self.store.run(run['run_id'],'foreign'),'POLICY_DENIED')):
            with self.subTest(code=code),self.assertRaises(AgentError) as error:invoke()
            self.assertEqual(error.exception.code,code)

    def test_handle_cannot_be_rebound_to_foreign_scope_or_different_internal_session(self):
        sid=self.session()['session_id'];run=self.reserve(sid)
        handle=self.handle(sid,run['run_id'])
        other=json.loads(json.dumps(handle));other['context']['project_id']='foreign'
        with self.assertRaises(AgentError):self.store.attach(run['run_id'],'learner',other)
        self.store.attach(run['run_id'],'learner',handle)
        other=json.loads(json.dumps(handle));other['teaching_session_id']='other'
        with self.assertRaises(AgentError):self.store.attach(run['run_id'],'learner',other)
        self.assertEqual(self.store.run(run['run_id'],'learner')['handle'],handle)

    def test_scope_is_metadata_only_and_instance_mismatch_is_rejected(self):
        sid=self.session()['session_id']
        self.scope['question']='private original question'
        with self.assertRaises(AgentError):self.reserve(sid)
        self.assertNotIn(b'private original question',self.path.read_bytes())
        self.store.close()
        with self.assertRaises(AgentError):self.type(self.path,'other-instance')

    def test_limits_reject_without_evicting_unfinished_associations(self):
        sid=self.session()['session_id']
        from unittest.mock import patch
        with patch('sigflow_edu_agent.teaching_store.MAX_SESSIONS',1):
            with self.assertRaises(AgentError) as error:self.session('other')
            self.assertEqual(error.exception.code,'RESOURCE_EXHAUSTED')
        with patch('sigflow_edu_agent.teaching_store.MAX_RUNS',1):
            run=self.reserve(sid)
            with self.assertRaises(AgentError):self.reserve(sid,'other')
            self.assertEqual(self.store.run(run['run_id'],'learner')['handle'],None)
