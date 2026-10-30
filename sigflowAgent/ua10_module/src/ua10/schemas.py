"""Stable public DTOs. Scope is issued by the authenticated host, not by an LLM."""
from dataclasses import dataclass,field,asdict
from typing import Any,Literal
from memory import LearningEvent,JobEvidence,Scope

SourceKind=Literal['real','mock','imported_unverified']

@dataclass(frozen=True)
class Observation:
    diagnostic_type: str
    exercise_id: str
    logical_object: str | None = None
    observation_key: str = 'diagnostic-0'

@dataclass(frozen=True)
class WriteResult:
    event_id: str
    recorded: bool
    issue_id: str | None
    occurrence_id: str | None
    source_committed: bool = True
    projection_state: str = 'synchronized'

@dataclass(frozen=True)
class LearningFact:
    event_id: str
    run_id: str
    revision: str
    issue_id: str | None
    event_type: str
    source_kind: SourceKind
    occurred_at: str
    available_at: str
    concept_ids: tuple[str,...]
    summary: str
    validation_scope: str | None
    status: str
    evidence_refs: tuple[str,...]
    synthetic: bool = False
    layer: Literal['fact'] = 'fact'
    occurrence_id: str | None = None
    session_id: str | None = None
    snapshot_id: str | None = None
    exercise_id: str | None = None

    @classmethod
    def from_item(cls,item):
        return cls(item['memory_id'],item['run_id'],item['revision'],item.get('issue_id'),item.get('event_type','unknown'),
            item['source_kind'],item['occurred_at'],item['available_at'],tuple(item['concept_ids']),item['summary'],
            item.get('validation_scope'),item['relevant_status'],tuple(item['evidence_refs']),item.get('synthetic',False),
            occurrence_id=item.get('occurrence_id'),session_id=item.get('session_id'),snapshot_id=item.get('snapshot_id'),exercise_id=item.get('exercise_id'))

@dataclass(frozen=True)
class LearningPattern:
    pattern_id: str
    summary: str
    evidence_refs: tuple[str,...]
    observations: tuple[dict[str,Any],...]
    difficulties: tuple[dict[str,Any],...]
    strategies: tuple[dict[str,Any],...]
    uncertainty: tuple[str,...]
    needs_teacher_review: bool = True
    layer: Literal['pattern'] = 'pattern'

@dataclass(frozen=True)
class QueryResult:
    facts: tuple[LearningFact,...]
    abstained: bool
    reason: str | None
    query_id: str
    measured_tokens: int
    token_budget: int
    token_measurement: str
    truncated: bool
    has_more: bool = False
    next_offset: int | None = None

@dataclass(frozen=True)
class RelatedAttempt:
    run_id: str
    revision: str
    evidence_refs: tuple[str,...]
    relationship: str
    same_issue_with_runs: tuple[str,...]
    fusion_allowed: bool = False

@dataclass(frozen=True)
class RelatedAttemptsResult:
    attempts: tuple[RelatedAttempt,...]
    query_id: str
    truncated: bool

@dataclass(frozen=True)
class PatternResult:
    patterns: tuple[LearningPattern,...]
    query_id: str
    truncated: bool

@dataclass(frozen=True)
class TeachingTask:
    question: str
    concept: str
    current_evidence_id: str
    revision: str
    as_of: str

@dataclass(frozen=True)
class TeachingMemoryBundle:
    facts: tuple[LearningFact,...]
    patterns: tuple[dict[str,Any],...]
    recommendations: tuple[dict[str,Any],...]
    unknown: tuple[str,...]
    dto: dict[str,Any]
    query_id: str
    measured_tokens: int
    token_measurement: str
    needs_teacher_review: bool

    def to_dict(self):
        """The budget-checked context DTO, without duplicate audit metadata."""
        import copy
        return copy.deepcopy(self.dto)

@dataclass(frozen=True)
class RevocationResult:
    event_id: str
    revoked: bool
    original_fact_preserved: bool = True
