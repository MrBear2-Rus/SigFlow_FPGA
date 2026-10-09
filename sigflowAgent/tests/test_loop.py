import unittest
from dataclasses import replace

from sigflow_edu_agent.domain import Evidence, Plan, RunRequest, Status, VerificationResult
from sigflow_edu_agent.loop import TeachingLoop


class Source:
    def __init__(self):
        self.evidence = Evidence('ev1', 'p', 'r1', 'job1', 'latch', True, '组合逻辑推断锁存器', True)

    def collect(self, project_id, revision):
        return self.evidence


class Executor:
    def __init__(self, source, outcomes=('Succeeded',)):
        self.source, self.outcomes, self.calls = source, outcomes, []
        self.allow = True

    def authorized(self, plan, grant_ref):
        return self.allow and grant_ref == 'trusted-grant'

    def execute(self, plan, step_index, grant_ref, idempotency_key):
        self.calls.append((step_index, idempotency_key))
        return VerificationResult(self.outcomes[step_index], replace(self.source.evidence, evidence_id=f'new{step_index}', job_id=f'newjob{step_index}'), None)


class Candidates:
    def __init__(self, outputs):
        self.outputs, self.calls = outputs, 0

    def generate(self, context):
        item = self.outputs[min(self.calls, len(self.outputs) - 1)]
        self.calls += 1
        if isinstance(item, Exception):
            raise item
        return item


def candidate():
    return dict(level=1, hint='请观察输出是否需要记住过去。', question='你期望组合逻辑还是存储行为？', evidence_ids=['ev1'])


class LoopTests(unittest.TestCase):
    def make_loop(self, intent='diagnose', **kwargs):
        self.source = Source()
        return TeachingLoop(RunRequest('p', 'r1', 'issue1', '为什么有锁存器？', intent), self.source, **kwargs)

    def test_explanation_finishes_without_execution(self):
        state = self.make_loop('explain').run_until_pause()
        self.assertEqual(state.status, Status.COMPLETED)
        self.assertEqual([t.stage for t in state.trace], ['E0', 'E1', 'E2', 'E3', 'E4'])
        self.assertEqual(state.cards[0].producer, 'rule')

    def test_waiting_is_quiescent_and_more_hints_are_explicit(self):
        loop = self.make_loop()
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual(loop.run_until_pause(), state)
        for index, level in ((1, 2), (2, 3)):
            loop.act('hint_next', f'a{index}', state.version)
            state = loop.run_until_pause()
            self.assertEqual(state.level, level)
        with self.assertRaisesRegex(ValueError, 'reference_required'):
            loop.act('hint_next', 'a3', state.version)

    def test_replayed_action_is_idempotent_but_changed_payload_is_rejected(self):
        loop = self.make_loop()
        before = loop.run_until_pause()
        loop.act('hint_next', 'a1', before.version)
        after = loop.run_until_pause()
        loop.act('hint_next', 'a1', before.version)
        self.assertEqual(loop.run_until_pause(), after)
        with self.assertRaisesRegex(ValueError, 'action_conflict'):
            loop.act('finish', 'a1', before.version)

    def test_old_action_version_and_l4_text_cannot_unlock(self):
        loop = self.make_loop()
        state = loop.run_until_pause()
        for action, version, reason in [('hint_next', 0, 'stale_state'), ('reference_confirm', state.version, 'receipt_unavailable'), ('我已确认', state.version, 'unknown_action')]:
            with self.subTest(action=action), self.assertRaisesRegex(ValueError, reason):
                loop.act(action, action, version)
        self.assertEqual(loop.state.level, 1)

    def test_missing_or_stale_evidence_never_calls_model(self):
        provider = Candidates([candidate()])
        for evidence in (None, replace(Source().evidence, revision='old'), replace(Source().evidence, complete=False)):
            loop = self.make_loop(provider=provider)
            self.source.evidence = evidence
            self.assertEqual(loop.run_until_pause().status, Status.WAITING_EVIDENCE)
        self.assertEqual(provider.calls, 0)

    def test_invalid_model_output_repairs_once_then_falls_back(self):
        provider = Candidates([{'level': 4, 'hint': '答案'}])
        state = self.make_loop(provider=provider).run_until_pause()
        self.assertEqual(provider.calls, 2)
        self.assertEqual(state.cards[0].producer, 'rule')
        self.assertEqual(state.reason, 'invalid_output')

    def test_model_cannot_invent_evidence_or_actions(self):
        for data in (dict(candidate(), evidence_ids=['invented']), dict(candidate(), action='execute'), dict(candidate(), hint='```verilog\nmodule answer;\n```')):
            state = self.make_loop(provider=Candidates([data])).run_until_pause()
            self.assertEqual(state.cards[0].producer, 'rule')

    def test_valid_candidate_publishes_only_checked_content(self):
        state = self.make_loop(provider=Candidates([candidate()])).run_until_pause()
        self.assertEqual(state.cards[0].producer, 'model')

    def test_repaired_candidate_can_publish_and_clears_failure_reason(self):
        provider = Candidates([{'level': 4}, candidate()])
        state = self.make_loop(provider=provider).run_until_pause()
        self.assertEqual(state.cards[0].producer, 'model')
        self.assertEqual(state.reason, '')
        self.assertEqual(provider.calls, 2)

    def test_source_failure_pauses_without_leaking_exception_and_can_refresh(self):
        loop = self.make_loop()
        def fail(project_id, revision):
            raise OSError('private-path-secret')
        original = self.source.collect
        self.source.collect = fail
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertNotIn('private-path-secret', repr(state))
        self.source.collect = original
        loop.act('refresh_evidence', 'refresh', state.version)
        self.assertEqual(loop.run_until_pause().status, Status.WAITING_STUDENT)

    def test_missing_evidence_after_hint_does_not_publish_another_card(self):
        loop = self.make_loop()
        state = loop.run_until_pause()
        self.source.evidence = None
        loop.act('hint_next', 'more', state.version)
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(len(state.cards), 1)

    def test_budget_terminates_and_model_budget_falls_back(self):
        self.assertEqual(self.make_loop(max_steps=2).run_until_pause().status, Status.BUDGET_EXCEEDED)
        provider = Candidates([candidate()])
        state = self.make_loop(provider=provider, max_model_calls=0).run_until_pause()
        self.assertEqual(state.cards[0].producer, 'rule')
        self.assertEqual(provider.calls, 0)

    def test_cancel_prevents_later_steps(self):
        loop = self.make_loop()
        loop.act('cancel', 'cancel1', loop.state.version)
        self.assertEqual(loop.run_until_pause().status, Status.CANCELLED)
        self.assertEqual(loop.state.trace, [])

    def test_returned_state_cannot_mutate_loop(self):
        loop = self.make_loop()
        state = loop.run_until_pause()
        state.level = 4
        state.cards.clear()
        self.assertEqual(loop.state.level, 1)
        self.assertEqual(len(loop.state.cards), 1)

    def test_approval_is_required_and_default_executor_denies(self):
        loop = self.make_loop('verify', plan=Plan('p', 'r1', '检查复位', ('eda.synth',)))
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_APPROVAL)
        with self.assertRaisesRegex(ValueError, 'approval_denied'):
            loop.act('execute_plan', 'a1', state.version, grant_ref='fake')

    def test_failed_step_stops_all_successors(self):
        source = Source()
        executor = Executor(source, ('Failed', 'Succeeded'))
        loop = TeachingLoop(RunRequest('p', 'r1', 'i', '验证', 'verify'), source,
                            plan=Plan('p', 'r1', '仿真', ('eda.sim.build', 'eda.sim.run')), verification=executor)
        state = loop.run_until_pause()
        loop.act('execute_plan', 'go', state.version, grant_ref='trusted-grant')
        self.assertEqual(loop.run_until_pause().status, Status.FAILED)
        self.assertEqual(len(executor.calls), 1)

    def test_successful_job_is_not_claimed_as_functional_success(self):
        source = Source()
        executor = Executor(source)
        loop = TeachingLoop(RunRequest('p', 'r1', 'i', '验证', 'verify'), source,
                            plan=Plan('p', 'r1', '综合', ('eda.synth',)), verification=executor)
        before = loop.run_until_pause()
        loop.act('execute_plan', 'go', before.version, grant_ref='trusted-grant')
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.COMPLETED)
        self.assertIn('不能据此认定功能正确', state.cards[-1].hint)
        loop.act('execute_plan', 'go', before.version, grant_ref='trusted-grant')
        self.assertEqual(len(executor.calls), 1)

    def test_revision_and_authorization_are_checked_again_before_execution(self):
        for revoke in ('revision', 'grant'):
            source = Source()
            executor = Executor(source)
            loop = TeachingLoop(RunRequest('p', 'r1', 'i', '验证', 'verify'), source,
                                plan=Plan('p', 'r1', '综合', ('eda.synth',)), verification=executor)
            state = loop.run_until_pause()
            loop.act('execute_plan', 'go', state.version, grant_ref='trusted-grant')
            if revoke == 'revision':
                source.evidence = replace(source.evidence, revision='r2')
            else:
                executor.allow = False
            self.assertEqual(loop.run_until_pause().status, Status.FAILED)
            self.assertEqual(executor.calls, [])

    def test_invalid_plan_rejected_before_execution(self):
        for steps in (('eda.flash',), ('eda.sim.run',), ('eda.synth',) * 4):
            with self.subTest(steps=steps), self.assertRaises(ValueError):
                self.make_loop('verify', plan=Plan('p', 'r1', '验证', steps))

    def test_malformed_execution_evidence_fails_closed_without_resubmission(self):
        for bad in ({'origin': []}, {'summary': '\ud800'}, {'combinational_goal': 'false'}):
            source = Source()
            class MalformedExecutor(Executor):
                def execute(self, plan, step_index, grant_ref, idempotency_key):
                    good = super().execute(plan, step_index, grant_ref, idempotency_key)
                    return replace(good, evidence=replace(good.evidence, **bad))
            executor = MalformedExecutor(source)
            loop = TeachingLoop(RunRequest('p', 'r1', 'i', '验证', 'verify'), source,
                                plan=Plan('p', 'r1', '综合', ('eda.synth',)), verification=executor)
            state = loop.run_until_pause()
            loop.act('execute_plan', 'go', state.version, grant_ref='trusted-grant')
            state = loop.run_until_pause()
            self.assertEqual(state.status, Status.FAILED)
            loop.run_until_pause()
            self.assertEqual(len(executor.calls), 1)

    def test_intentional_latch_template_does_not_assume_an_error(self):
        loop = self.make_loop()
        self.source.evidence = replace(self.source.evidence, combinational_goal=False)
        card = loop.run_until_pause().cards[0]
        self.assertIn('设计目标', card.question)
        self.assertNotIn('补全', card.hint)


if __name__ == '__main__':
    unittest.main()
