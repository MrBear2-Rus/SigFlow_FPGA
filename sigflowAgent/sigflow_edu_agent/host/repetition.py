"""Advice from committed workflow observations; no profile or mastery inference."""
from ..domain import Evidence
from ..patterns import PATTERNS, assess
from ..pedagogy import valid_evidence


def project_observation(evidence, request):
    if request.intent != 'diagnose' or not isinstance(evidence, Evidence):
        return None
    eligible = (evidence.origin == 'tool' and evidence.confidence_kind == 'tool'
        and evidence.anchor is not None and valid_evidence(evidence, request)
        and evidence.concept in PATTERNS and assess(evidence).status == 'supported')
    return dict(job_id=evidence.job_id, concept=evidence.concept, eligible=eligible)


def _valid(value, job):
    return (type(value) is dict and set(value) == {'job_id', 'concept', 'eligible'}
        and type(value['job_id']) is str and value['job_id'] == job
        and type(value['concept']) is str and type(value['eligible']) is bool)


def repetition_advice(binding, current, committed_rows):
    """Count different Jobs in the same issue; uncertainty ends the streak.

    Current work is only published by the caller's ordinary commit. A report
    reread, hint, recovered session or new session over that Job is one event.
    """
    result = dict(consecutive_occurrences=0, suggest_hint_request=False, message='')
    if (not _valid(current, binding.job_id) or not current['eligible']
            or current['concept'] not in PATTERNS):
        return result
    seen, observations = set(), []
    scope = ('instance_id', 'learner_id', 'project_id', 'issue_id', 'policy')
    # The first saved workflow position owns a Job's place in the sequence.
    # A later session over an old Job must never move it past an interruption.
    for row in committed_rows:
        saved = row['binding']
        if (row['intent'] != 'diagnose'
                or any(saved[key] != getattr(binding, key) for key in scope)):
            continue
        job = saved['job_id']
        if job in seen:
            continue
        seen.add(job)
        observation = current if saved['session_id'] == binding.session_id else row['state'].get('teaching_observation')
        observations.append((job, observation))
    if binding.job_id not in seen:
        observations.append((binding.job_id, current))
    count = 0
    for job, observation in reversed(observations):
        if (not _valid(observation, job) or not observation['eligible']
                or observation['concept'] != current['concept']):
            break
        count += 1
    result['consecutive_occurrences'] = count
    if count >= 3:
        result.update(suggest_hint_request=True,
            message='已连续观察到同类问题，要更直接的提示吗？提示等级仅在明确请求后提升。')
    return result
