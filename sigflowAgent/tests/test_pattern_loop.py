from dataclasses import replace
import unittest

from sigflow_edu_agent.domain import Evidence, RunRequest
from sigflow_edu_agent.loop import TeachingLoop


class PatternLoopTests(unittest.TestCase):
    def test_unsupported_or_excluded_evidence_cannot_reach_model(self):
        # A well-formed model card can still invent a root cause. Unsupported
        # pattern evidence must be stopped before generation, not only JSON QA.
        class Source:
            def collect(self, project_id, revision):
                return self.evidence

        class Provider:
            calls = 0

            def generate(self, context):
                self.calls += 1
                return {'level': 1, 'hint': '已经确定根因，直接修复。',
                        'question': '还有问题吗？', 'evidence_ids': ['e']}

        base = Evidence('e', 'p', 'r', 'j', 'latch', True, 'observation', True)
        for ev in (replace(base, concept='reset'), replace(base, concept='unknown'),
                   replace(base, combinational_goal=False)):
            with self.subTest(concept=ev.concept):
                source, provider = Source(), Provider()
                source.evidence = ev
                state = TeachingLoop(RunRequest('p', 'r', 'i', 'why?'), source, provider).run_until_pause()
                self.assertEqual(provider.calls, 0)
                self.assertEqual(state.model_calls, 0)
                self.assertEqual(state.cards[0].producer, 'rule')
                self.assertNotIn('已经确定根因', state.cards[0].hint)
