"""薄教学入口的离线、身份、幂等和生命周期测试。输入全部为合成事实。"""
import copy
import tempfile
import unittest
import json
import subprocess
import sys
from pathlib import Path

from ua10 import MemoryConfig, TeachingMemory, LearningEvent, JobEvidence, Observation
from ua10.errors import ScopeError, ClosedServiceError, DeletedScopeError


def binding(learner='learner', project='project', instance='board', session='session'):
    return dict(learner_id=learner, project_id=project, instance_id=instance, session_id=session,
                revision='r1', snapshot_id='snapshot', job_id='job', diagnostic_id='diag', issue_id='issue',
                policy='l1-l3-l4-denied')


def fact(bound, kind='start', *, mode='real', run='run'):
    return dict(schema='ua10-host-fact.v1', kind=kind, binding=copy.deepcopy(bound), concept_id='latch',
        execution_mode=mode, state=dict(run_id=run, stage='E0' if kind == 'start' else 'E5',
            status='Running' if kind == 'start' else 'WaitingStudent', version=0 if kind == 'start' else 1,
            level=1, cards=[], trace=[]), occurrences=[] if kind == 'start' else [dict(
                binding=copy.deepcopy(bound), evidence_id='evidence', evidence_sha256='0'*64,
                origin='tool' if mode == 'real' else 'fixture')])


class TeachingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.config = MemoryConfig(Path(self.temp.name))
        self.bound = binding(); self.memory = TeachingMemory(self.config, trusted_binding=self.bound,
            concepts=('latch',), execution_mode='real')

    def tearDown(self):
        self.memory.close(); self.temp.cleanup()

    def context(self, memory=None, **kw):
        return (memory or self.memory).get_teaching_context('历史锁存器诊断', 'latch', 'current', 'r2', **kw)

    def write(self):
        self.memory.record_event(fact(self.bound))
        return self.memory.record_event(fact(self.bound, 'transition'))

    def test_record_context_reopen_without_key_or_model(self):
        self.write(); context = self.context()
        self.assertTrue(context.text); self.assertTrue(context.references['event_ids'])
        self.assertLessEqual(self.memory._host.encoder.tokens(context.text),self.config.default_token_budget)
        self.assertEqual(context.evidence_kinds, ('real',))
        self.assertEqual(self.context(as_of='2020-01-01T00:00:00Z').text, '')
        self.memory.close()
        with self.assertRaises(ClosedServiceError):self.context()
        self.memory = TeachingMemory(self.config, trusted_binding=self.bound, concepts=('latch',), execution_mode='real')
        self.assertEqual(context.references, self.context().references)

    def test_duplicate_outbox_delivery_is_idempotent(self):
        self.write(); first = self.context().references
        self.write(); self.assertEqual(first, self.context().references)

    def test_process_exit_after_v1_commit_replays_original_outbox(self):
        child = '''
import sys,json,os
from ua10 import MemoryConfig,TeachingMemory
with TeachingMemory(MemoryConfig(sys.argv[1]),trusted_binding=json.loads(sys.argv[2]),concepts=('latch',),execution_mode='real') as memory:
 memory._host._refresh=lambda scope:os._exit(73)
 memory.record_event(json.loads(sys.argv[3]))
'''
        result=subprocess.run([sys.executable,'-c',child,str(self.config.data_dir),
            json.dumps(self.bound),json.dumps(fact(self.bound))],capture_output=True)
        self.assertEqual(result.returncode,73,result.stderr.decode(errors='replace'))
        self.write()
        self.assertTrue(self.context().references['event_ids'])
        rows=self.memory._host.authority.v1.db.connection.execute(
            "SELECT count(*) FROM learning_events WHERE event_type='u2_run_started'").fetchone()
        self.assertEqual(rows[0],1)

    def test_bound_identity_and_concept_cannot_be_overridden(self):
        self.write()
        for field in ('learner_id', 'project_id', 'instance_id', 'session_id'):
            changed = fact(self.bound); changed['binding'][field] = 'foreign'
            with self.subTest(field=field), self.assertRaises(ScopeError):self.memory.record_event(changed)
        changed = fact(self.bound); changed['concept_id'] = 'reset'
        with self.assertRaises(ScopeError):self.memory.record_event(changed)
        with self.assertRaises(ScopeError):self.memory.get_teaching_context('复位', 'reset', 'current', 'r2')
        with TeachingMemory(self.config,trusted_binding=self.bound,concepts=('reset',),execution_mode='real') as other:
            changed=fact(self.bound,'transition');changed['concept_id']=None
            with self.assertRaises(ScopeError):other.record_event(changed)
        job=JobEvidence('mixed-job','project','board','r1','snapshot','mixed-report','0'*64,
            kind='real',outcome='failed')
        self.memory._api.append_learning_event(LearningEvent('mixed','run','issue_observed',('latch','reset'),job=job),
            observation=Observation('MIXED_DIAG','mixed-exercise'),synthetic=True)
        with self.assertRaises(ScopeError):self.context()

    def test_cross_learner_project_instance_and_mock_are_isolated(self):
        self.write()
        for field in ('learner_id', 'project_id', 'instance_id'):
            other = dict(self.bound, session_id='other-'+field, **{field:'other'})
            with TeachingMemory(self.config, trusted_binding=other, concepts=('latch',), execution_mode='real') as m:
                self.assertEqual(self.context(m).text, '')
        other = dict(self.bound, session_id='mock-session')
        with TeachingMemory(self.config, trusted_binding=other, concepts=('latch',)) as m:
            m.record_event(fact(other, mode='mock', run='mock-run'))
            m.record_event(fact(other, 'transition', mode='mock', run='mock-run'))
            self.assertEqual(self.context(m).text, '')

    def test_administration_stays_in_core_and_deleted_binding_cannot_revive(self):
        self.write(); api = self.memory._api
        for eid in self.context().references['event_ids']:api.revoke_learning_event(eid)
        self.assertEqual(self.context().text, '')
        api.complete_run('run'); self.assertEqual(api.delete_session()['status'], 'completed')
        self.memory.close()
        with self.assertRaises(DeletedScopeError):TeachingMemory(self.config, trusted_binding=self.bound,
            concepts=('latch',), execution_mode='real')


if __name__ == '__main__':unittest.main(verbosity=2)
