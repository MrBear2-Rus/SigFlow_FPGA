"""Explicit fixtures for development only: no real EDA jobs or UI grants."""

from dataclasses import asdict, replace
from .domain import Evidence, Plan, RunRequest, VerificationResult
from .loop import TeachingLoop


class FixtureSource:
    def collect(self, project_id, revision):
        return Evidence('fixture-latch-1', project_id, revision, 'fixture-job-1',
                        'latch', True, '教学示例：组合逻辑块推断出了锁存器。', True, 'fixture')


class FixtureVerification:
    def __init__(self, source, succeeds=True):
        self.source, self.succeeds = source, succeeds

    def authorized(self, plan, grant_ref):
        return grant_ref == 'fixture-only-grant'

    def execute(self, plan, step_index, grant_ref, idempotency_key):
        evidence = replace(self.source.collect(plan.project_id, plan.revision),
                           evidence_id=f'fixture-result-{step_index}',
                           job_id=f'fixture-result-job-{step_index}')
        return VerificationResult('Succeeded' if self.succeeds else 'Failed', evidence, None)


def run_demo() -> dict:
    source = FixtureSource()
    result = {'evidence_origin': 'fixture'}
    for intent in ('explain', 'diagnose'):
        loop = TeachingLoop(RunRequest('demo', 'r1', 'latch', '为什么出现锁存器？', intent), source)
        state = loop.run_until_pause()
        if intent == 'diagnose':
            for action_id in ('more-1', 'more-2'):
                loop.act('hint_next', action_id, state.version)
                state = loop.run_until_pause()
        result[intent] = asdict(state)
    plan = Plan('demo', 'r1', '检查仿真结果', ('eda.sim.build', 'eda.sim.run'))
    for name, succeeds, approved in (('unapproved', True, False),
                                     ('failed_validation', False, True),
                                     ('successful_validation', True, True)):
        loop = TeachingLoop(RunRequest('demo', 'r1', 'latch', '验证', 'verify'), source,
                            plan=plan, verification=FixtureVerification(source, succeeds))
        state = loop.run_until_pause()
        if approved:
            loop.act('execute_plan', 'execute-1', state.version, grant_ref='fixture-only-grant')
            state = loop.run_until_pause()
        result[name] = asdict(state)
    return result
