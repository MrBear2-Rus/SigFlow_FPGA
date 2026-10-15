"""Joint RAG/engineering-gate regressions for the merge."""
from dataclasses import replace
import unittest
from sigflow_edu_agent.context.models import Omission
from sigflow_edu_agent.domain import Evidence, RunRequest
from sigflow_edu_agent.patterns import assess
from sigflow_edu_agent.loop import TeachingLoop
from tests.test_courses import Courses, Probe


class CoursePatternMergeTests(unittest.TestCase):
    def test_optional_course_gap_does_not_hide_supported_engineering_pattern(self):
        original=Evidence('ev1','p','r','j','latch',True,'Fixture storage observation',True)
        partial=replace(original,omitted=(Omission('course:fixture','token_budget'),))
        self.assertEqual(assess(partial).status,'supported')
        for concept in ['unknown','width_signedness']:
            self.assertEqual(assess(replace(partial,concept=concept)).status,'unsupported')
        for reason in ['byte_budget','token_budget']:
            ev=replace(partial,omitted=partial.omitted+(Omission('rtl',reason),))
            self.assertEqual(assess(ev).status,'insufficient')

    def test_gate_and_course_context_coexist_in_model_call(self):
        class Source:
            def collect(self,*args):return self.evidence
        base=Evidence('ev1','p','r','j','latch',True,'Fixture storage observation',True,
                      omitted=(Omission('course:earlier','token_budget'),))
        for concept,expected in [('latch',1),('unknown',0)]:
            with self.subTest(concept=concept):
                source=Source();source.evidence=replace(base,concept=concept)
                provider=Probe()
                state=TeachingLoop(RunRequest('p','r','i','why?'),source,provider,
                                   course_provider=Courses()).run_until_pause()
                self.assertEqual(state.model_calls,expected)
                self.assertEqual(len(provider.contexts),expected)
                if expected:
                    self.assertTrue(provider.contexts[0].course_refs)
                    self.assertTrue(state.cards[0].course_refs)
