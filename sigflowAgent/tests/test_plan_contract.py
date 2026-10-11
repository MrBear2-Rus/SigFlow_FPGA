import unittest
from dataclasses import FrozenInstanceError, replace
from sigflow_edu_agent.checkers.plan import DetailedPlan, PlanStep
from sigflow_edu_agent.contracts import build_plan_card


def plan():
    return DetailedPlan('project-a', 'rev-a', 'snap-a', 'Verify counter', (
        PlanStep('compile', 'eda.sim.build', 'iverilog', {'top': 'counter', 'testbench': 'counter_tb'}),
        PlanStep('execute', 'eda.sim.run', 'iverilog', {'build_step_id': 'compile'}, ('compile',)),
    ), ('count increments',))


class PlanContractTests(unittest.TestCase):
    def test_literal_card_and_full_binding(self):
        result = build_plan_card(plan(), 'plan-7', 9007199254740993)
        self.assertEqual(result.card, {
            'plan_id': 'plan-7', 'goal': 'Verify counter', 'revision': 'rev-a',
            'state_version': '9007199254740993', 'max_jobs': 3,
            'stop_conditions': ['job_failed', 'observation_mismatch'],
            'snapshot_required': True, 'requires_approval': True,
            'project_id': 'project-a', 'snapshot_id': 'snap-a',
            'expected_observations': ['count increments'],
            'steps': [
                {'step_id': 's1', 'capability': 'eda.sim.build', 'provider': 'iverilog',
                 'description': 'Verify counter: build simulation for counter (counter_tb)',
                 'params': {'top_module': 'counter', 'testbench': 'counter_tb'},
                 'depends_on': [], 'max_jobs': 1},
                {'step_id': 's2', 'capability': 'eda.sim.run', 'provider': 'iverilog',
                 'description': 'Verify counter: run simulation built by s1',
                 'params': {'build_step_id': 's1'}, 'depends_on': ['s1'], 'max_jobs': 1},
            ],
        })
        binding = result.binding
        self.assertEqual(binding['plan_id'], 'plan-7')
        self.assertEqual(binding['state_version'], '9007199254740993')
        self.assertEqual(binding['internal_fingerprint'], plan().fingerprint)
        self.assertEqual(binding['plan'], {
            'project_id': 'project-a', 'revision': 'rev-a', 'snapshot_id': 'snap-a',
            'goal': 'Verify counter', 'max_jobs': 3,
            'expected_observations': ['count increments'],
            'stop_conditions': ['job_failed', 'observation_mismatch'],
            'steps': [
                {'step_id': 'compile', 'capability': 'eda.sim.build', 'provider': 'iverilog',
                 'params': {'top': 'counter', 'testbench': 'counter_tb'}, 'dependencies': []},
                {'step_id': 'execute', 'capability': 'eda.sim.run', 'provider': 'iverilog',
                 'params': {'build_step_id': 'compile'}, 'dependencies': ['compile']},
            ],
        })

    def test_output_cannot_mutate_approval_material(self):
        result = build_plan_card(plan(), 'plan-1', 0)
        digest = result.digest
        result.card['steps'][0]['params']['top_module'] = 'evil'
        result.binding['plan']['steps'].clear()
        self.assertEqual(result.card['steps'][0]['params']['top_module'], 'counter')
        self.assertEqual(len(result.binding['plan']['steps']), 2)
        self.assertEqual(result.digest, digest)
        with self.assertRaises(FrozenInstanceError):
            result.digest = 'changed'

    def test_digest_stable_across_mapping_order(self):
        original = plan()
        reordered = replace(original, steps=(replace(original.steps[0], params={
            'testbench': 'counter_tb', 'top': 'counter'}), original.steps[1]))
        self.assertEqual(build_plan_card(original, 'plan-1', 0).digest,
                         build_plan_card(reordered, 'plan-1', 0).digest)

    def test_digest_binds_all_approval_semantics(self):
        original = plan()
        digest = build_plan_card(original, 'plan-1', 0).digest
        variants = [replace(original, **{field: value}) for field, value in (
            ('project_id', 'project-b'), ('revision', 'rev-b'), ('snapshot_id', 'snap-b'),
            ('goal', 'Check reset'), ('expected_observations', ('reset clears',)),
            ('stop_conditions', ('job_failed',)), ('max_jobs', 2))]
        variants.append(replace(original, steps=tuple(replace(s, provider='verilator') for s in original.steps)))
        variants.append(replace(original, steps=(replace(original.steps[0], params={'top': 'other', 'testbench': 'counter_tb'}), original.steps[1])))
        variants.append(replace(original, steps=(replace(original.steps[0], step_id='other'),
            replace(original.steps[1], params={'build_step_id': 'other'}, dependencies=('other',)))))
        for variant in variants:
            with self.subTest(variant=variant):
                self.assertNotEqual(build_plan_card(variant, 'plan-1', 0).digest, digest)
        self.assertNotEqual(build_plan_card(original, 'plan-2', 0).digest, digest)
        self.assertNotEqual(build_plan_card(original, 'plan-1', 1).digest, digest)

    def test_rejects_unrepresentable_timeout(self):
        original = plan()
        timed = replace(original, steps=(original.steps[0], replace(original.steps[1],
            params={'build_step_id': 'compile', 'timeout_ms': 100})))
        with self.assertRaisesRegex(ValueError, 'UNSUPPORTED_MAPPING'):
            build_plan_card(timed, 'plan-1', 0)

    def test_rejects_invalid_external_identifiers_and_versions(self):
        for bad in ('plan-0', 'plan-01', 'plan-1\n', '1', '', None, True):
            with self.subTest(plan_id=bad), self.assertRaises(ValueError):
                build_plan_card(plan(), bad, 0)
        for bad in (-1, True, 1.0, '1', None):
            with self.subTest(version=bad), self.assertRaises(ValueError):
                build_plan_card(plan(), 'plan-1', bad)

    def test_revalidates_plan_at_boundary(self):
        with self.assertRaises(ValueError):
            build_plan_card({}, 'plan-1', 0)
        invalid = plan()
        object.__setattr__(invalid, 'max_jobs', 16)
        with self.assertRaises(ValueError):
            build_plan_card(invalid, 'plan-1', 0)

    def test_synthesis_mapping(self):
        synth = replace(plan(), steps=(PlanStep('synth', 'eda.synth', 'yosys', {'top': 'counter'}),))
        step = build_plan_card(synth, 'plan-1', 0).card['steps'][0]
        self.assertEqual(step['params'], {'top_module': 'counter'})
        self.assertEqual(step['description'], 'Verify counter: synthesize counter')
        self.assertEqual(step['depends_on'], [])
