"""Simulation duration is an explicit uint64 tick count, never milliseconds."""
import unittest
from sigflow_edu_agent.checkers.plan import DetailedPlan, PlanStep
from sigflow_edu_agent.contracts.plan import build_plan_card


class DurationTicksTests(unittest.TestCase):
    def test_duration_survives_checked_plan_and_approval_mapping_exactly(self):
        build = PlanStep('build', 'eda.sim.build', 'verilator', {'top':'top', 'testbench':'tb'})
        for value in ('0', '40', '18446744073709551615'):
            with self.subTest(value=value):
                run = PlanStep('run', 'eda.sim.run', 'verilator',
                    {'build_step_id':'build', 'duration_ticks':value}, ('build',))
                plan = DetailedPlan('p', 'r', 's', 'Observe a bounded timeline', (build, run), ('sample tick 15',))
                package = build_plan_card(plan, 'plan-1', 0)
                self.assertEqual(package.card['steps'][1]['params'], {'build_step_id':'s1', 'duration_ticks':value})
                self.assertEqual(package.binding['plan']['steps'][1]['params']['duration_ticks'], value)

    def test_duration_rejects_implicit_units_overflow_and_noncanonical_numbers(self):
        for value in (0, True, 40.0, '', '-1', '+1', '01', '1e3', '40ns', '18446744073709551616'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                PlanStep('run', 'eda.sim.run', 'verilator',
                    {'build_step_id':'build', 'duration_ticks':value}, ('build',))
