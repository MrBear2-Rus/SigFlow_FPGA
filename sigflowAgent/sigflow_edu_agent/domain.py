"""Internal domain values, deliberately independent of the shared HTTP DTOs."""

from dataclasses import dataclass, field
from enum import StrEnum
import hashlib
import json

from .context.models import ContextItem, EvidenceAnchor, Omission


class Status(StrEnum):
    RUNNING = 'Running'
    WAITING_STUDENT = 'WaitingStudent'
    WAITING_EVIDENCE = 'WaitingEvidence'
    WAITING_APPROVAL = 'WaitingApproval'
    COMPLETED = 'Completed'
    CANCELLED = 'Cancelled'
    FAILED = 'Failed'
    BUDGET_EXCEEDED = 'BudgetExceeded'


@dataclass(frozen=True)
class RunRequest:
    project_id: str
    revision: str
    issue_id: str
    question: str
    intent: str = 'diagnose'

    def __post_init__(self):
        if self.intent not in {'explain', 'diagnose', 'verify'}:
            raise ValueError('unknown_intent')
        for value in (self.project_id, self.revision, self.issue_id, self.question):
            if not isinstance(value, str) or not value.strip() or len(value) > 4096:
                raise ValueError('invalid_request')


@dataclass(frozen=True)
class Evidence:
    evidence_id: str
    project_id: str
    revision: str
    job_id: str
    concept: str
    complete: bool
    summary: str
    combinational_goal: bool = False
    origin: str = 'fixture'
    confidence_kind: str = 'tool'
    anchor: EvidenceAnchor | None = None
    context_items: tuple[ContextItem, ...] = ()
    omitted: tuple[Omission, ...] = ()


@dataclass(frozen=True)
class Plan:
    project_id: str
    revision: str
    goal: str
    steps: tuple[str, ...]

    def __post_init__(self):
        allowed = {'eda.synth', 'eda.sim.build', 'eda.sim.run'}
        if not isinstance(self.steps, tuple) or not 1 <= len(self.steps) <= 3:
            raise ValueError('invalid_plan_size')
        for value in (self.project_id, self.revision, self.goal):
            if not isinstance(value, str) or not value.strip() or len(value) > 4096:
                raise ValueError('invalid_plan')
        for index, step in enumerate(self.steps):
            if step not in allowed:
                raise ValueError('capability_denied')
            if step == 'eda.sim.run' and 'eda.sim.build' not in self.steps[:index]:
                raise ValueError('missing_build_dependency')

    @property
    def fingerprint(self) -> str:
        # Domain fingerprint only. Gateway adapter must bind the full wire plan.
        data = [self.project_id, self.revision, self.goal, self.steps]
        return hashlib.sha256(json.dumps(data, ensure_ascii=False).encode()).hexdigest()


@dataclass(frozen=True)
class CardContext:
    question: str
    level: int
    evidence: Evidence
    repair: bool = False


@dataclass(frozen=True)
class Card:
    level: int
    hint: str
    question: str
    evidence_ids: tuple[str, ...]
    producer: str
    limitations: tuple[str, ...] = ()


@dataclass(frozen=True)
class VerificationResult:
    state: str
    evidence: Evidence | None
    matches_expected: bool | None


@dataclass(frozen=True)
class Trace:
    stage: str
    status: Status
    reason: str
    version: int


@dataclass
class RunState:
    run_id: str
    stage: str = 'E0'
    status: Status = Status.RUNNING
    level: int = 1
    version: int = 0
    steps: int = 0
    model_calls: int = 0
    reason: str = ''
    cards: list[Card] = field(default_factory=list)
    trace: list[Trace] = field(default_factory=list)


class ProviderError(Exception):
    """Only an allowlisted code crosses the model boundary, never raw errors."""

    CODES = {'model_unconfigured', 'model_timeout', 'model_unavailable',
             'model_auth_failed', 'model_rate_limited', 'invalid_output',
             'context_too_large', 'response_too_large'}

    def __init__(self, code):
        self.code = code if code in self.CODES else 'model_unavailable'
        super().__init__(self.code)


class EvidenceError(ValueError):
    """Stable, redacted errors from evidence adapters."""

    CODES = {'invalid_report', 'stale_report', 'incomplete_report', 'diagnostic_missing',
             'hypothesis_not_fact', 'unresolved_evidence', 'expired_evidence',
             'invalid_source_ref', 'mixed_context', 'invalid_context', 'invalid_budget'}

    def __init__(self, code):
        self.code = code if code in self.CODES else 'invalid_report'
        super().__init__(self.code)
