"""Immutable, deliberately restricted development verification plans."""
from collections.abc import Mapping
from dataclasses import dataclass
import hashlib
import json
import re
from types import MappingProxyType


def text(value):
    if type(value) is not str or not value.strip() or len(value) > 4096:
        raise ValueError('invalid_plan_text')
    try:
        value.encode('utf-8')
    except UnicodeError as error:
        raise ValueError('invalid_plan_text') from error
    return value


def strings(values):
    if not isinstance(values, (tuple, list)):
        raise ValueError('invalid_plan_sequence')
    return tuple(text(value) for value in values)


@dataclass(frozen=True)
class PlanStep:
    step_id: str
    capability: str
    provider: str
    params: Mapping
    dependencies: tuple[str, ...] = ()

    def __post_init__(self):
        for value in (self.step_id, self.capability, self.provider):
            text(value)
        schemas = {
            'eda.synth': ({'yosys'}, {'top'}, {'top'}),
            'eda.sim.build': ({'iverilog', 'verilator'}, {'top', 'testbench'}, {'top', 'testbench'}),
            'eda.sim.run': ({'iverilog', 'verilator'}, {'build_step_id', 'timeout_ms', 'duration_ticks'}, {'build_step_id'}),
        }
        if self.capability not in schemas:
            raise ValueError('capability_denied')
        providers, allowed, required = schemas[self.capability]
        if self.provider not in providers:
            raise ValueError('provider_denied')
        if not isinstance(self.params, Mapping):
            raise ValueError('invalid_parameters')
        params = dict(self.params)
        if not required <= params.keys() or not params.keys() <= allowed:
            raise ValueError('parameters_denied')
        for key, value in params.items():
            if key == 'duration_ticks':
                if (type(value) is not str or re.fullmatch(r'0|[1-9][0-9]{0,19}', value) is None
                        or int(value) > 2**64-1):
                    raise ValueError('invalid_duration_ticks')
            elif key == 'timeout_ms':
                if type(value) is not int or not 1 <= value <= 60000:
                    raise ValueError('invalid_timeout')
            else:
                text(value)
                if key == 'build_step_id' and not re.fullmatch(r'[A-Za-z0-9_.-]+', value):
                    raise ValueError('invalid_identifier')
                if key in {'top', 'testbench'} and not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_$]*', value):
                    raise ValueError('invalid_module_identifier')
        deps = strings(self.dependencies)
        if len(deps) != len(set(deps)):
            raise ValueError('duplicate_dependency')
        object.__setattr__(self, 'dependencies', deps)
        object.__setattr__(self, 'params', MappingProxyType(dict(sorted(params.items()))))


@dataclass(frozen=True)
class DetailedPlan:
    project_id: str
    revision: str
    snapshot_id: str
    goal: str
    steps: tuple[PlanStep, ...]
    expected_observations: tuple[str, ...]
    stop_conditions: tuple[str, ...] = ('job_failed', 'observation_mismatch')
    max_jobs: int = 3

    def __post_init__(self):
        for value in (self.project_id, self.revision, self.snapshot_id, self.goal):
            text(value)
        if type(self.max_jobs) is not int or not 1 <= self.max_jobs <= 3:
            raise ValueError('invalid_job_budget')
        if not isinstance(self.steps, (tuple, list)) or not 1 <= len(self.steps) <= self.max_jobs:
            raise ValueError('invalid_plan_size')
        steps = tuple(self.steps)
        seen = {}
        for step in steps:
            if type(step) is not PlanStep:
                raise ValueError('invalid_step')
            if step.step_id in seen or any(dep not in seen for dep in step.dependencies):
                raise ValueError('invalid_dependency')
            if step.capability == 'eda.sim.run':
                build_id = step.params['build_step_id']
                build = seen.get(build_id)
                if (build_id not in step.dependencies or build is None
                        or build.capability != 'eda.sim.build' or build.provider != step.provider):
                    raise ValueError('build_mismatch')
            seen[step.step_id] = step
        observations, stops = strings(self.expected_observations), strings(self.stop_conditions)
        if not observations or not stops or 'job_failed' not in stops or not set(stops) <= {'job_failed', 'observation_mismatch'}:
            raise ValueError('invalid_observations_or_stops')
        object.__setattr__(self, 'steps', steps)
        object.__setattr__(self, 'expected_observations', observations)
        object.__setattr__(self, 'stop_conditions', tuple(sorted(set(stops))))

    @property
    def gateway_ready(self):
        # Local validation cannot certify an adapter's wire binding or grant.
        return False

    @property
    def fingerprint(self):
        data = dict(project_id=self.project_id, revision=self.revision, snapshot_id=self.snapshot_id,
                    goal=self.goal, max_jobs=self.max_jobs, expected_observations=self.expected_observations,
                    stop_conditions=self.stop_conditions,
                    steps=[dict(step_id=s.step_id, capability=s.capability, provider=s.provider,
                                params=dict(s.params), dependencies=s.dependencies) for s in self.steps])
        return hashlib.sha256(json.dumps(data, ensure_ascii=False, sort_keys=True,
                                       separators=(',', ':')).encode('utf-8')).hexdigest()


class PlanChecker:
    def check(self, plan):
        if type(plan) is not DetailedPlan:
            raise ValueError('detailed_plan_required')
        # Reconstruct at the execution boundary, including nested fields.
        return DetailedPlan(plan.project_id, plan.revision, plan.snapshot_id, plan.goal,
                            tuple(PlanStep(s.step_id, s.capability, s.provider, s.params, s.dependencies)
                                  for s in plan.steps), plan.expected_observations,
                            plan.stop_conditions, plan.max_jobs)
