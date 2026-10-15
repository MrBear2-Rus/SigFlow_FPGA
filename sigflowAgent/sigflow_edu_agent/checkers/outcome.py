"""Compare trusted observations without interpreting arbitrary diagnostic text."""
from dataclasses import dataclass
from ..domain import Evidence, RunRequest
from ..pedagogy import valid_evidence


@dataclass(frozen=True)
class Outcome:
    state: str
    reason: str


class OutcomeChecker:
    def compare(self, old, new, target_revision, *, project_id, matches_expected,
                seen_job_ids=(), snapshot_id=None):
        try:
            old_request = RunRequest(project_id, old.revision, 'comparison', 'comparison')
            new_request = RunRequest(project_id, target_revision, 'comparison', 'comparison')
            if not valid_evidence(old, old_request) or not valid_evidence(new, new_request):
                return Outcome('rejected', 'invalid_comparison_evidence')
            if new.job_id == old.job_id or new.job_id in seen_job_ids or new.evidence_id == old.evidence_id:
                return Outcome('rejected', 'reused_job_result')
            if old.concept != new.concept:
                return Outcome('rejected', 'unrelated_result_evidence')
            if snapshot_id is not None and (new.anchor is None or new.anchor.snapshot_id != snapshot_id):
                return Outcome('rejected', 'snapshot_mismatch')
            if matches_expected is not None and type(matches_expected) is not bool:
                return Outcome('rejected', 'invalid_expected_observation')
        except (AttributeError, TypeError, ValueError):
            return Outcome('rejected', 'invalid_comparison_evidence')
        return Outcome('indeterminate' if matches_expected is None else
                       'matches' if matches_expected else 'mismatch', 'bounded_observation_only')
