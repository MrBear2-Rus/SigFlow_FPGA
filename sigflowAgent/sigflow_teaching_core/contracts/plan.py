"""PlanCard approval material, separate from Gateway job/grant requests."""
from dataclasses import dataclass
import hashlib
import json
import re

from ..checkers.plan import DetailedPlan, PlanChecker


def _canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'),
                      allow_nan=False)


@dataclass(frozen=True)
class ApprovalPackage:
    """Immutable JSON snapshot; callers receive independent mutable copies.

    digest is a local handoff integrity digest, NOT Gateway plan_hash.
    """
    _material_json: str

    @property
    def card(self):
        return json.loads(self._material_json)['card']

    @property
    def binding(self):
        return json.loads(self._material_json)['binding']

    @property
    def digest(self):
        return hashlib.sha256(self._material_json.encode('utf-8')).hexdigest()


def build_plan_card(plan: DetailedPlan, plan_id: str, state_version: int) -> ApprovalPackage:
    """Map a restricted internal plan without submitting jobs or granting access."""
    if type(plan_id) is not str or not re.fullmatch(r'plan-[1-9][0-9]*', plan_id):
        raise ValueError('invalid_plan_id')
    if type(state_version) is not int or state_version < 0:
        raise ValueError('invalid_state_version')
    plan = PlanChecker().check(plan)
    ids = {step.step_id: f's{index}' for index, step in enumerate(plan.steps, 1)}
    steps = []
    for step in plan.steps:
        if 'timeout_ms' in step.params:
            raise ValueError('UNSUPPORTED_MAPPING: timeout_ms has no duration_ticks conversion')
        params = {('top_module' if key == 'top' else key): value
                  for key, value in step.params.items()}
        if 'build_step_id' in params:
            params['build_step_id'] = ids[params['build_step_id']]
        if step.capability == 'eda.synth':
            description = f"synthesize {params['top_module']}"
        elif step.capability == 'eda.sim.build':
            description = f"build simulation for {params['top_module']} ({params['testbench']})"
        else:
            description = f"run simulation built by {params['build_step_id']}"
        steps.append(dict(step_id=ids[step.step_id], capability=step.capability,
                          provider=step.provider, description=f'{plan.goal}: {description}',
                          params=params, depends_on=[ids[dep] for dep in step.dependencies], max_jobs=1))
    card = dict(plan_id=plan_id, goal=plan.goal, steps=steps, max_jobs=plan.max_jobs,
                stop_conditions=list(plan.stop_conditions), snapshot_required=True,
                revision=plan.revision, requires_approval=True, state_version=str(state_version),
                project_id=plan.project_id, snapshot_id=plan.snapshot_id,
                expected_observations=list(plan.expected_observations))
    original = dict(project_id=plan.project_id, revision=plan.revision,
                    snapshot_id=plan.snapshot_id, goal=plan.goal, max_jobs=plan.max_jobs,
                    expected_observations=list(plan.expected_observations),
                    stop_conditions=list(plan.stop_conditions),
                    steps=[dict(step_id=s.step_id, capability=s.capability, provider=s.provider,
                                params=dict(s.params), dependencies=list(s.dependencies))
                           for s in plan.steps])
    binding = dict(plan_id=plan_id, state_version=str(state_version),
                   internal_fingerprint=plan.fingerprint, plan=original)
    return ApprovalPackage(_canonical(dict(format='sigflow-local-approval-v1',
                                            card=card, binding=binding)))
