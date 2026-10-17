"""Course overlay/recovery contracts; generated courses, no network or real EDA."""
from dataclasses import asdict, replace
import json
import unittest

from sigflow_edu_agent.context.models import EvidenceAnchor, Omission
from sigflow_edu_agent.domain import CourseBundle, Status
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.patterns import assess
from sigflow_edu_agent.runtime.checkpoint import _digest, restore_readonly_session
from sigflow_edu_agent.runtime.session import TeachingSession
from test_checkpoint import Source, Provider
from test_courses import Courses, Probe, material
from sigflow_edu_agent.domain import RunRequest
from sigflow_edu_agent.runtime.gateway_session import SessionBinding


class CourseRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.binding = SessionBinding('inst', 'session', 'learner', 'p', 'r', 'snap',
                                      'fixture-job-1', 'diag', 'issue')
        self.request = RunRequest('p', 'r', 'issue', 'private question')
        self.source = Source()
        self.courses = Courses(CourseBundle('ok', (material('old', 'private-course-body'),)))
        class Model(Probe):
            def generate(inner, context):
                result = super().generate(context)
                return result | {'hint': 'old-model-course-explanation'}
        self.provider = Model()
        self.session = TeachingSession(TeachingLoop(self.request, self.source, self.provider,
                                                   course_provider=self.courses))

    def pause(self, session=None):
        session = session or self.session
        for _ in range(32):
            if session.state.status != Status.RUNNING:
                return session.state
            session.advance(session.state.version)
        self.fail('did not pause')

    def restore(self, courses):
        return restore_readonly_session(self.session.checkpoint(self.binding), binding=self.binding,
            request=self.request, source=self.source, provider=self.provider, course_provider=courses)

    def test_course_payload_reaches_model_without_replacing_engineering_observation(self):
        state = self.pause()
        self.assertEqual(state.cards[0].course_refs[0].passage_id, 'test:latch:old')
        self.assertTrue(self.provider.contexts[0].evidence.context_items)
        self.assertEqual(self.session._loop._evidence, self.source.current)

    def test_checkpoint_uses_engineering_hash_and_rule_recovery_without_course_content(self):
        before = self.pause()
        payload = self.session.checkpoint(self.binding)
        data = json.loads(payload)
        self.assertEqual(data['evidence_sha256'], _digest(asdict(self.source.current)))
        self.assertEqual(data['state']['cards'][0]['producer'], 'rule')
        self.assertEqual(data['state']['model_calls'], before.model_calls)
        for value in ('private-course-body', 'old-model-course-explanation', 'test:latch:old', 'private question'):
            self.assertNotIn(value, payload)

    def test_recovery_retrieves_current_course_without_model_call_or_level_change(self):
        before = self.pause()
        fresh = Courses(CourseBundle('ok', (material('new', version='sha256:' + 'b'*64),)))
        restored = self.restore(fresh)
        self.assertEqual(fresh.calls, [])
        state = restored.reconcile(restored.state.version)
        self.assertEqual((state.status, state.course_status, state.level), (Status.WAITING_STUDENT, 'ok', 1))
        self.assertEqual(state.model_calls, before.model_calls)
        self.assertEqual(len(self.provider.contexts), 1)
        self.assertEqual(restored._loop._course_refs[0].passage_id, 'test:latch:new')
        self.assertTrue(all(card.producer == 'rule' and not card.course_refs for card in state.cards))
        self.assertNotIn('old-model-course-explanation', repr(state))

    def test_missing_current_courses_degrade_without_blocking_engineering_recovery(self):
        self.pause()
        class Broken:
            def prepare(self, *args):
                raise OSError('private-path')
        restored = self.restore(Broken())
        state = restored.reconcile(restored.state.version)
        self.assertEqual((state.status, state.course_status), (Status.WAITING_STUDENT, 'unavailable'))
        self.assertFalse(restored._loop._course_refs)
        self.assertNotIn('private-path', repr(state))

    def test_changed_engineering_observation_does_not_retrieve_or_publish_courses(self):
        self.pause()
        fresh = Courses()
        restored = self.restore(fresh)
        self.source.current = replace(self.source.current, summary='changed')
        state = restored.reconcile(restored.state.version)
        self.assertEqual((state.status, state.cards), (Status.RECOVERY_REQUIRED, []))
        self.assertEqual(fresh.calls, [])

    def test_hint_after_recovery_does_not_accumulate_course_overlay(self):
        self.pause()
        fresh = Courses(CourseBundle('ok', (material('new'),)))
        restored = self.restore(fresh)
        state = restored.reconcile(restored.state.version)
        restored.host_action('hint_next', 'next', state.version)
        state = self.pause(restored)
        self.assertEqual(state.level, 2)
        self.assertEqual(self.provider.contexts[-1].evidence.context_items[0].item_id, 'course:test:latch:new')
        self.assertEqual(len(self.provider.contexts[-1].evidence.context_items), 1)
        self.assertEqual(restored._loop._evidence, self.source.current)

    def test_neutral_tool_observation_ignores_only_optional_course_omissions(self):
        anchor = EvidenceAnchor('ev', 'p', 'r', 'fixture-job-1', 'snap', 'artifact', 'a'*64)
        evidence = replace(self.source.current, evidence_id='ev', concept='job_observation',
            origin='tool', confidence_kind='tool', anchor=anchor, omitted=(Omission('course:a', 'token_budget'),))
        self.assertEqual(assess(evidence).status, 'supported')
        self.assertEqual(assess(replace(evidence, omitted=(Omission('rtl', 'token_budget'),))).status, 'insufficient')


if __name__ == '__main__':
    unittest.main()
