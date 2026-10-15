import unittest
from dataclasses import replace
from typing import get_type_hints, get_args

from sigflow_edu_agent.checkers import DetailedPlan, PlanStep, PlanChecker, OutcomeChecker
from sigflow_edu_agent.domain import Evidence, Plan, RunRequest, Status, VerificationResult
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.ports import VerificationPort


def synth(**kwargs):
    return PlanStep('synth', 'eda.synth', 'yosys', kwargs or {'top': 'top'})


def plan(**kwargs):
    data = dict(project_id='p', revision='r2', snapshot_id='snapshot', goal='verify',
                steps=(synth(),), expected_observations=('no latch',))
    return DetailedPlan(**(data | kwargs))


class PlanTests(unittest.TestCase):
    def test_execution_port_annotations_resolve_for_runtime_adapters(self):
        for method in (VerificationPort.authorized, VerificationPort.execute):
            self.assertEqual(set(get_args(get_type_hints(method)['plan'])), {Plan, DetailedPlan})

    def test_malformed_legacy_steps_have_stable_validation_error(self):
        for value in ([], {}, None, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                Plan('p', 'r', 'verify', (value,))

    def test_unknown_parameters_capabilities_and_provider_rejected(self):
        for args in [('x', 'eda.flash', 'yosys', {}), ('x', 'eda.synth', 'shell', {'top': 'top'}),
                     ('x', 'eda.synth', 'yosys', {'top': 'top', 'opts': '--shell'}),
                     ('x', 'eda.synth', 'yosys', {'top': ['top']})]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                plan(steps=(PlanStep(*args),))

    def test_dependencies_and_build_binding(self):
        build = PlanStep('build', 'eda.sim.build', 'iverilog', {'top': 'tb', 'testbench': 'tb'})
        run = PlanStep('run', 'eda.sim.run', 'iverilog', {'build_step_id': 'build', 'timeout_ms': 1000}, ('build',))
        self.assertEqual(len(plan(steps=(build, run)).steps), 2)
        for steps in [(run, build), (build, build), (build, replace(run, dependencies=())),
                      (build, replace(run, params={'build_step_id': 'other'})),
                      (build, replace(run, provider='verilator')),
                      (replace(build, dependencies=('run',)), run)]:
            with self.subTest(steps=steps), self.assertRaises(ValueError):
                plan(steps=steps)
        with self.assertRaises(ValueError):
            plan(steps=(PlanStep('build', 'eda.sim.build', 'iverilog', {'top': 'tb'}),))

    def test_budgets_and_required_bindings(self):
        for changes in [dict(max_jobs=0), dict(max_jobs=4), dict(max_jobs=True), dict(snapshot_id=''),
                        dict(expected_observations=()), dict(stop_conditions=()),
                        dict(steps=tuple(replace(synth(), step_id=str(i)) for i in range(4)))]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                plan(**changes)

    def test_module_names_are_identifiers_not_paths(self):
        for value in ('../tb.v', 'tb/foo.v', '..', '1foo', 'tb.v'):
            for field in ('top', 'testbench'):
                with self.subTest(value=value, field=field), self.assertRaises(ValueError):
                    PlanStep('build', 'eda.sim.build', 'iverilog', {'top': 'top', 'testbench': 'tb'} | {field: value})

    def test_immutable_and_canonical_fingerprint(self):
        params = {'top': 'top'}
        steps = [PlanStep('synth', 'eda.synth', 'yosys', params)]
        observations = ['no latch']
        first = plan(steps=steps, expected_observations=observations)
        params['top'] = 'evil'
        steps.clear()
        observations.clear()
        self.assertEqual(first.fingerprint, plan().fingerprint)
        with self.assertRaises(TypeError):
            first.steps[0].params['top'] = 'evil'
        for field, value in [('project_id', 'q'), ('revision', 'r3'), ('snapshot_id', 'other'),
                             ('goal', 'other'), ('expected_observations', ('other',)), ('max_jobs', 2),
                             ('stop_conditions', ('job_failed',)), ('steps', (synth(top='other'),))]:
            self.assertNotEqual(first.fingerprint, replace(first, **{field: value}).fingerprint)
        self.assertEqual(PlanChecker().check(first), first)
        self.assertFalse(Plan('p', 'r1', 'legacy', ('eda.synth',)).gateway_ready)


class OutcomeTests(unittest.TestCase):
    def test_later_success_does_not_hide_prior_mismatch_or_uncertainty(self):
        initial = Evidence('e0', 'p', 'r2', 'j0', 'latch', True, 'initial', True,
                           anchor=EvidenceAnchor('e0', 'p', 'r2', 'j0', 'snapshot', 'a0', 'h0'))
        class Source:
            def collect(self, *args): return initial
        for observations, status, reason in (
                ((False, True), Status.WAITING_EVIDENCE, 'recollect_and_replan'),
                ((None, True), Status.COMPLETED, 'outcome_indeterminate')):
            class Executor:
                def authorized(self, *args): return True
                def execute(self, plan, index, *args):
                    identity = f'result-{index}'
                    new = replace(initial, evidence_id=identity, job_id=identity,
                                  anchor=EvidenceAnchor(identity, 'p', 'r2', identity, 'snapshot', identity, 'hash'))
                    return VerificationResult('Succeeded', new, observations[index])
            with self.subTest(observations=observations):
                detailed = plan(steps=(synth(), replace(synth(), step_id='second')),
                                stop_conditions=('job_failed',))
                loop = TeachingLoop(RunRequest('p', 'r2', 'i', 'verify', 'verify'), Source(),
                                    plan=detailed, verification=Executor())
                state = loop.run_until_pause()
                loop.act('execute_plan', 'go', state.version, grant_ref='grant')
                state = loop.run_until_pause()
                self.assertEqual(state.status, status)
                self.assertEqual(state.reason, reason)
                self.assertEqual(state.cards[-1].evidence_ids, ('result-0', 'result-1'))

    def test_preexisting_presubmission_job_cannot_be_claimed_as_new_result(self):
        initial = Evidence('e1', 'p', 'r', 'j1', 'latch', True, 'initial', True)
        current = replace(initial, evidence_id='e2', job_id='j2')

        class Source:
            calls = 0
            def collect(self, *args):
                self.calls += 1
                return initial if self.calls == 1 else current

        class Executor:
            def authorized(self, *args): return True
            def execute(self, *args): return VerificationResult('Succeeded', current, True)

        loop = TeachingLoop(RunRequest('p', 'r', 'i', 'verify', 'verify'), Source(),
                            plan=Plan('p', 'r', 'verify', ('eda.synth',)), verification=Executor())
        state = loop.run_until_pause()
        loop.act('execute_plan', 'go', state.version, grant_ref='grant')
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.FAILED)
        self.assertEqual(state.reason, 'reused_job_result')

    def setUp(self):
        self.old = Evidence('old', 'p', 'r1', 'oldjob', 'latch', True, 'old')
        self.new = replace(self.old, evidence_id='new', revision='r2', job_id='newjob')

    def compare(self, **kwargs):
        return OutcomeChecker().compare(kwargs.pop('old', self.old), kwargs.pop('new', self.new),
                                        'r2', project_id='p', **kwargs)

    def test_explicit_outcome_tristate(self):
        for flag, expected in [(True, 'matches'), (False, 'mismatch'), (None, 'indeterminate')]:
            self.assertEqual(self.compare(matches_expected=flag).state, expected)

    def test_invalid_or_reused_evidence_never_matches(self):
        for changes in [dict(project_id='q'), dict(revision='r1'), dict(job_id='oldjob'),
                        dict(evidence_id='old'), dict(complete=False)]:
            self.assertEqual(self.compare(new=replace(self.new, **changes), matches_expected=True).state, 'rejected')
        self.assertEqual(self.compare(old=replace(self.old, project_id='q'), matches_expected=True).state, 'rejected')
        self.assertEqual(self.compare(matches_expected=1).state, 'rejected')
        self.assertEqual(self.compare(matches_expected=True, seen_job_ids=('newjob',)).state, 'rejected')

    def test_snapshot_and_concept_binding_are_required(self):
        self.assertEqual(self.compare(matches_expected=True, snapshot_id='snapshot').state, 'rejected')
        anchor = EvidenceAnchor('new', 'p', 'r2', 'newjob', 'snapshot', 'artifact', 'hash')
        new = replace(self.new, anchor=anchor)
        self.assertEqual(self.compare(new=new, matches_expected=True, snapshot_id='snapshot').state, 'matches')
        self.assertEqual(self.compare(new=new, matches_expected=True, snapshot_id='other').state, 'rejected')
        self.assertEqual(self.compare(new=replace(new, concept='width'), matches_expected=True).state, 'rejected')

    def test_malformed_baseline_rejected_at_loop_boundary(self):
        with self.assertRaises(ValueError):
            TeachingLoop(RunRequest('p', 'r2', 'i', 'verify', 'verify'), None,
                         plan=plan(), baseline_evidence={'revision': 'r1'})

    def test_loop_mismatch_recollects_without_grant_reuse(self):
        old = self.old
        new = replace(self.new, anchor=EvidenceAnchor('new', 'p', 'r2', 'newjob', 'snapshot', 'artifact', 'hash'))
        class Source:
            def collect(self, *args): return new
        class Executor:
            calls = 0
            def authorized(self, *args): return True
            def execute(self, *args):
                self.calls += 1
                anchor = EvidenceAnchor('result', 'p', 'r2', 'result', 'snapshot', 'artifact', 'a' * 64)
                return VerificationResult('Succeeded', replace(new, job_id='result', evidence_id='result', anchor=anchor), False)
        executor = Executor()
        two_steps = plan(steps=(synth(), replace(synth(), step_id='second')))
        loop = TeachingLoop(RunRequest('p', 'r2', 'i', 'verify', 'verify'), Source(), plan=two_steps,
                            verification=executor, baseline_evidence=old)
        state = loop.run_until_pause()
        loop.act('execute_plan', 'execute', state.version, grant_ref='grant')
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        loop.act('refresh_evidence', 'refresh', state.version)
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual(executor.calls, 1)
        with self.assertRaises(ValueError):
            loop.act('execute_plan', 'again', state.version, grant_ref='grant')

    def test_snapshot_is_checked_before_side_effect(self):
        new = self.new
        class Source:
            def collect(self, *args): return new
        class Executor:
            calls = 0
            def authorized(self, *args): return True
            def execute(self, *args):
                self.calls += 1
                return VerificationResult('Failed', None, None)
        executor = Executor()
        loop = TeachingLoop(RunRequest('p', 'r2', 'i', 'verify', 'verify'), Source(),
                            plan=plan(), verification=executor)
        state = loop.run_until_pause()
        loop.act('execute_plan', 'go', state.version, grant_ref='grant')
        self.assertEqual(loop.run_until_pause().status, Status.FAILED)
        self.assertEqual(executor.calls, 0)
