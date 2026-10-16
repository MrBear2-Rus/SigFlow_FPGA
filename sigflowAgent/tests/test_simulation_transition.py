"""HTTP boundary fixtures catch observation that improperly executes a successor."""
import time
import unittest

from sigflow_edu_agent.contracts.plan import build_plan_card
from sigflow_edu_agent.host.runtime import parse_plan
from sigflow_edu_agent.host.simulation_acceptance import exercise_simulation, RunFailure


SCOPE = dict(project_id='p1', revision='r1', snapshot_id='snap1')


class GatewayFixture:
    def __init__(self):
        self.phase = 'waiting'

    def request(self, method, path, body=None, **kwargs):
        if path == '/api/v1/capabilities':
            return {'capabilities': [dict(id=c, plugin_id=p, ready=True, disabled=False)
                for c,p in [('eda.sim.build','eda-sim-verilator'),
                             ('eda.sim.run','eda-sim-verilator-run')]]}
        if path.endswith('/grants'):
            return {'grant_id':'grant1'}
        if path.endswith('/jobs') and method == 'POST':
            return {}  # Explicit invalid dependency rejection boundary.
        if path.endswith('/jobs'):
            return {'jobs': [{'job_id':'baseline'}] +
                ([{'job_id':'build1'}] if self.phase != 'waiting' else []) +
                ([{'job_id':'run1'}] if self.phase == 'completed' else [])}
        if path == '/api/v1/grants/grant1':
            return {'used_jobs': {'waiting':0, 'building':1, 'completed':2}[self.phase]}
        if path == '/api/v1/jobs/build1':
            return {'state':'Succeeded'}
        if path.endswith('/report'):
            is_run = 'run1' in path
            return dict(**SCOPE, job_id='run1' if is_run else 'build1', state='Succeeded',
                origin='core', schema_version='edu.jobreport.v1', completeness='complete',
                capability='eda.sim.run' if is_run else 'eda.sim.build',
                plugin_id='eda-sim-verilator-run' if is_run else 'eda-sim-verilator',
                exit_code=0, input_fingerprint='verified-input', artifacts=[dict(
                    artifact_id='wave1' if is_run else 'exe1', sha256='a'*64,
                    schema='eda.wave.vcd.v1' if is_run else 'eda.sim.verilator-executable.v1')])
        if path == '/api/v1/artifacts/wave1':
            return dict(project_id='p1', revision='r1', job_id='run1', artifact_id='wave1',
                        sha256='a'*64, schema='eda.wave.vcd.v1', size='100')
        raise AssertionError((method,path))


class TeachingFixture:
    def __init__(self, gateway, *, auto_successor, completed_steps_marker=0):
        self.gateway, self.auto_successor = gateway, auto_successor
        self.completed_steps_marker = completed_steps_marker
        self.execute_count = 0

    def complete(self):
        self.gateway.phase = 'completed'
        self.current = dict(status='Completed', state_version='3',
            reason='outcome_indeterminate', verification=dict(completed_steps=2), model_calls=0,
            evidence_origin='tool', cards=[dict(producer='rule', evidence_ids=['new-build','new-run'])])

    def request(self, method, path, body=None, **kwargs):
        if path.endswith('/sessions'):
            steps = build_plan_card(parse_plan(body['plan']),'plan-1',0).card['steps']
            self.current = dict(status='WaitingApproval',state_version='0',
                                cards=[dict(evidence_ids=['baseline'])],plan_card=dict(steps=steps))
            return {'session_id':'session1'}
        if path.endswith('/state'):
            return self.current
        if path.endswith('/actions'):
            if body['kind'] == 'execute_plan':
                if body['expected_version'] == '0' and self.current['state_version'] == '0':
                    self.execute_count += 1
                    self.gateway.phase = 'building'
                    self.current = dict(status='RecoveryRequired',state_version='1',
                        verification=dict(can_observe=True,job_id='build1',completed_steps=0))
                    if self.completed_steps_marker == 'missing':
                        self.current['verification'].pop('completed_steps')
                    else:
                        self.current['verification']['completed_steps'] = self.completed_steps_marker
                elif body['expected_version'] == '2':
                    self.execute_count += 1
                    self.complete()
            elif body['kind'] == 'observe_verification':
                if self.auto_successor:
                    self.complete()  # Deliberately prohibited side effect of a read.
                else:
                    self.current = dict(status='WaitingApproval',state_version='2',
                        reason='successor_approval_required',verification=dict(completed_steps=1))
            return {}
        raise AssertionError((method,path))


class SimulationTransitionTests(unittest.TestCase):
    def test_observing_build_must_not_accept_automatic_run_even_if_completed(self):
        gateway = GatewayFixture()
        teaching = TeachingFixture(gateway, auto_successor=True)
        with self.assertRaisesRegex(RunFailure, '^simulation_successor_started_automatically$'):
            exercise_simulation(gateway,gateway,teaching,'/api/v1/projects/p1',SCOPE,
                                'baseline',time.monotonic()+5)

    def test_build_observation_then_explicit_successor_confirmation_is_accepted(self):
        gateway = GatewayFixture()
        teaching = TeachingFixture(gateway, auto_successor=False)
        result = exercise_simulation(gateway,gateway,teaching,'/api/v1/projects/p1',SCOPE,
                                     'baseline',time.monotonic()+5)
        self.assertEqual(result['status'], 'passed')
        self.assertEqual((teaching.execute_count,result['successor_confirmations']), (2,1))
        self.assertEqual(result['observation_actions'], 1)

    def test_missing_or_malformed_step_marker_cannot_bypass_successor_check(self):
        for marker in ('missing', None, True, False, -1, 2, '0'):
            with self.subTest(marker=marker):
                gateway = GatewayFixture()
                teaching = TeachingFixture(gateway, auto_successor=True, completed_steps_marker=marker)
                with self.assertRaisesRegex(RunFailure, '^simulation_progress_invalid$'):
                    exercise_simulation(gateway,gateway,teaching,'/api/v1/projects/p1',SCOPE,
                                        'baseline',time.monotonic()+5)
