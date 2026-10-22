"""Workflow advice uses committed scoped observations, never a learner profile."""
from dataclasses import asdict, replace
import importlib
import importlib.util
from pathlib import Path
import tempfile
import time
import unittest

from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.domain import RunRequest
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.gateway_session import SessionBinding
from sigflow_edu_agent.runtime.session import TeachingSession


def scoped_evidence(job='j3'):
    evidence = FixtureSource().collect('p', 'r')
    return replace(evidence, job_id=job, origin='tool', anchor=EvidenceAnchor(
        evidence.evidence_id, 'p', 'r', job, 's', 'artifact', 'a'*64))


class RepetitionTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.find_spec('sigflow_edu_agent.host.repetition')
        self.assertIsNotNone(spec, 'repeat_error_advice_missing')
        self.module = importlib.import_module(spec.name)
        self.binding = SessionBinding('inst', 'session3', 'learner', 'p', 'r', 's', 'j3', 'diag', 'issue')
        self.current = dict(job_id='j3', concept='latch', eligible=True)

    def row(self, job, *, observation=None, **scope):
        binding = asdict(replace(self.binding, session_id='session-'+job, job_id=job, **scope))
        return dict(binding=binding, intent='diagnose', state={'teaching_observation':
            observation if observation is not None else dict(job_id=job, concept='latch', eligible=True)})

    def test_three_distinct_jobs_suggest_without_requesting_level_change(self):
        result = self.module.repetition_advice(self.binding, self.current, [self.row('j1'), self.row('j2')])
        self.assertEqual(result, dict(consecutive_occurrences=3, suggest_hint_request=True,
            message='已连续观察到同类问题，要更直接的提示吗？提示等级仅在明确请求后提升。'))

    def test_same_job_across_sessions_is_one_occurrence(self):
        result = self.module.repetition_advice(self.binding, self.current,
            [self.row('j3'), self.row('j3'), self.row('j3')])
        self.assertEqual(result['consecutive_occurrences'], 1)
        self.assertFalse(result['suggest_hint_request'])

    def test_rereading_old_job_cannot_move_it_after_an_interruption(self):
        rows = [self.row('j1'), self.row('j2', observation=dict(job_id='j2',concept='latch',eligible=False)),
                self.row('j3'), self.row('j4')]
        current = self.current | {'job_id':'j4'}
        latest = self.module.repetition_advice(replace(self.binding, job_id='j4'), current, rows)
        reread = self.module.repetition_advice(replace(self.binding, session_id='new-session', job_id='j1'),
            self.current | {'job_id':'j1'}, rows)
        self.assertEqual(latest['consecutive_occurrences'], 2)
        self.assertEqual(reread['consecutive_occurrences'], 2)
        self.assertFalse(reread['suggest_hint_request'])

    def test_committed_old_job_reread_keeps_its_original_position_for_future_jobs(self):
        rows = [self.row('j1'), self.row('j2', observation=dict(job_id='j2',concept='latch',eligible=False)),
                self.row('j3'), self.row('j4'), self.row('j1')]
        result = self.module.repetition_advice(replace(self.binding, job_id='j5'),
            self.current | {'job_id':'j5'}, rows)
        self.assertEqual(result['consecutive_occurrences'], 3)
        self.assertTrue(result['suggest_hint_request'])

    def test_other_scope_never_contributes(self):
        for field, value in [('learner_id','other'), ('project_id','other'), ('issue_id','other'), ('instance_id','other')]:
            with self.subTest(field=field):
                result = self.module.repetition_advice(self.binding, self.current,
                    [self.row('j1', **{field:value}), self.row('j2', **{field:value})])
                self.assertEqual(result['consecutive_occurrences'], 1)

    def test_different_pattern_legal_counterexample_and_legacy_interrupt_streak(self):
        for observation in (dict(job_id='j2', concept='ports', eligible=True),
                            dict(job_id='j2', concept='latch', eligible=False), {}):
            with self.subTest(observation=observation):
                result = self.module.repetition_advice(self.binding, self.current,
                    [self.row('j1'), self.row('j2', observation=observation)])
                self.assertEqual(result['consecutive_occurrences'], 1)

    def test_projection_requires_supported_tool_evidence_and_diagnosis(self):
        request = RunRequest('p', 'r', 'issue', 'explain evidence')
        evidence = scoped_evidence()
        self.assertEqual(self.module.project_observation(evidence, request), self.current)
        for changes in (dict(origin='fixture'), dict(confidence_kind='hypothesis'), dict(complete=False),
                        dict(combinational_goal=False), dict(anchor=None), dict(concept='job_observation')):
            with self.subTest(changes=changes):
                projected = self.module.project_observation(replace(evidence, **changes), request)
                self.assertFalse(projected['eligible'])
        self.assertIsNone(self.module.project_observation(evidence, replace(request, intent='explain')))

    def test_malformed_or_foreign_job_metadata_cannot_contribute(self):
        for current in (None, {}, self.current | {'eligible':1}, self.current | {'job_id':'foreign'}):
            with self.subTest(current=current):
                result = self.module.repetition_advice(self.binding, current, [self.row('j1'), self.row('j2')])
                self.assertEqual(result['consecutive_occurrences'], 0)
                self.assertFalse(result['suggest_hint_request'])


class RepetitionHostTests(unittest.TestCase):
    def test_committed_advice_survives_restart_without_auto_upgrade_or_extra_work(self):
        directory = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(directory.cleanup)
        store = DurableStore(Path(directory.name)/'host.sqlite3', 'inst')
        def factory(binding, request, mode, reserve):
            class Source:
                def collect(self, project_id, revision):
                    return scoped_evidence(binding.job_id)
            return TeachingSession(TeachingLoop(request, Source()))
        runtime = Runtime(store, 'inst', factory, evidence_origin='tool')
        resources = [runtime, store]
        self.addCleanup(lambda: [resource.close() for resource in resources])
        def wait(sid, action):
            deadline = time.monotonic()+4
            while time.monotonic() < deadline:
                if store.action(sid, action)['status'] == 'committed':
                    return runtime.snapshot(sid, 'learner')
                time.sleep(.01)
            self.fail('teaching action did not commit')
        body = dict(learner_id='learner', project_id='p', revision='r', snapshot_id='s',
            diagnostic_id='diag', issue_id='issue', question='explain report', mode='rule')
        for index in (1, 2):
            sid = runtime.create(body | {'job_id':f'j{index}'}, f'create{index}')['session_id']
            first = wait(sid, 'create')
            self.assertIn('teaching_advice', first, 'host_repeat_advice_missing')
            self.assertEqual(first['teaching_advice']['consecutive_occurrences'], index)
            runtime.action(sid, 'learner', 'finish', 'finish', int(first['state_version']))
            wait(sid, 'finish')
        runtime.close(); store.close()
        store = DurableStore(Path(directory.name)/'host.sqlite3', 'inst')
        runtime = Runtime(store, 'inst', factory, evidence_origin='tool')
        resources[:] = [runtime, store]
        sid = runtime.create(body | {'job_id':'j3'}, 'create3')['session_id']
        state = wait(sid, 'create')
        self.assertEqual(state['teaching_advice']['consecutive_occurrences'], 3)
        self.assertTrue(state['teaching_advice']['suggest_hint_request'])
        self.assertEqual((state['level'], len(state['cards']), state['model_calls']), (1, 1, 0))
        runtime.create(body | {'job_id':'j3'}, 'create3')
        self.assertEqual(runtime.snapshot(sid, 'learner'), state)
        runtime.action(sid, 'learner', 'hint_next', 'hint', int(state['state_version']))
        second = wait(sid, 'hint')
        self.assertEqual((second['level'], second['teaching_advice']['consecutive_occurrences']), (2, 3))
