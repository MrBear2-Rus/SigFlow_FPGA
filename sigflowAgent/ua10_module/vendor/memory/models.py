"""Trusted runtime DTOs. No source, chat, credentials or model request bodies."""
from __future__ import annotations

import re
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from typing import Optional, Tuple, Protocol


class MemoryErrorBase(ValueError):
    pass


class ScopeError(MemoryErrorBase):
    pass


class ConflictError(MemoryErrorBase):
    pass


class DeletedScopeError(ScopeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds")


def timestamp(value: str) -> str:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise MemoryErrorBase("timestamps must include timezone")
    return parsed.astimezone(timezone.utc).isoformat(timespec="microseconds")


def identifier(value: str) -> str:
    # Identifiers/references only; URLs, absolute paths and arbitrary text are excluded.
    if not isinstance(value, str) or not re.fullmatch(r"[\w][\w.:@+-]{0,159}", value):
        raise MemoryErrorBase("invalid identifier/reference")
    if re.search(r"(?i)(sk-|bearer|api[_-]?key|token[=:]|password[=:])", value):
        raise MemoryErrorBase("credential-like reference rejected")
    return value


EVENT_TYPES = (
    "concept_exposed", "issue_observed", "hint_presented", "reference_viewed",
    "student_understood", "student_modified", "student_explanation",
    "retry_passed", "retry_failed", "course_shown", "course_clicked", "course_used",
    "feedback_reported", "practice_suggested", "u2_run_started", "u2_diagnostic_observed",
    "u2_card_generated", "u2_hint_accepted", "u2_action_accepted", "u2_checkpoint_saved",
    "u2_run_ended", "u2_observation_enriched",
)
RUN_STATES = (
    "Created", "CollectingEvidence", "Analyzing", "PreparingResponse", "WaitingStudent",
    "WaitingApproval", "WaitingJob", "Evaluating", "Completed", "Cancelled", "Failed",
    "RecoveryRequired",
)
TERMINAL_STATES = ("Completed", "Cancelled", "Failed")


@dataclass(frozen=True)
class Scope:
    profile_id: str
    project_id: str
    instance_id: str
    session_id: str


@dataclass(frozen=True)
class HintPreferences:
    # Only a direct student settings action may set this DTO.
    start_level: int = 1
    explanation_length: str = "standard"
    wording: str = "plain"
    prerequisite_reminders: bool = True
    practice_suggestions: bool = True


@dataclass(frozen=True)
class PersonalizationPolicy:
    allow_l2_start: bool = False
    max_start_level: int = 1


@dataclass(frozen=True)
class CourseInteraction:
    passage_id: str
    index_version: str
    interaction: str  # shown / clicked / used


@dataclass(frozen=True)
class JobEvidence:
    job_id: str
    project_id: str
    instance_id: str
    revision: str
    snapshot_id: str
    report_ref: str
    input_fingerprint: str
    kind: str = "mock"  # runtime verifies this against Gateway, never a model field
    validation: str = "process_exit"  # test_expectation / diagnostic_absence / process_exit / synth_exit
    outcome: str = "passed"
    complete: bool = False
    scope_ref: Optional[str] = None  # checked experiment expectation / targeted diagnostic


@dataclass(frozen=True)
class LearningEvent:
    event_id: str
    run_id: str
    event_type: str
    concept_ids: Tuple[str, ...]
    issue_id: Optional[str] = None
    occurrence_id: Optional[str] = None
    hint_level: int = 0
    reference_viewed: bool = False
    student_modified: bool = False
    explanation_ref: Optional[str] = None
    followup_ref: Optional[str] = None
    evidence_refs: Tuple[str, ...] = ()
    job: Optional[JobEvidence] = None
    course: Optional[CourseInteraction] = None
    created_at: Optional[str] = None
    u2: Optional["U2TeachingFacts"] = None

    def payload(self) -> dict:
        result = asdict(self)
        # Keep v1 event fingerprints and exports byte-for-byte compatible.
        if self.u2 is None:
            result.pop("u2")
        elif self.u2.diagnostic_type is None:
            result["u2"].pop("diagnostic_type")
        return result


@dataclass(frozen=True)
class U2TeachingFacts:
    """Allowlisted, body-free facts copied from the current U2 runtime contract."""

    external_issue_id: Optional[str] = None
    diagnostic_id: Optional[str] = None
    job_id: Optional[str] = None
    snapshot_id: Optional[str] = None
    evidence_id: Optional[str] = None
    evidence_sha256: Optional[str] = None
    evidence_origin: Optional[str] = None
    evidence_integrity: Optional[str] = None
    concept_id: Optional[str] = None
    card_id: Optional[str] = None
    card_level: Optional[int] = None
    card_producer: Optional[str] = None
    card_sha256: Optional[str] = None
    card_hint: Optional[str] = None
    card_question: Optional[str] = None
    course_refs: Tuple[Tuple[str, str], ...] = ()
    action_kind: Optional[str] = None
    action_sha256: Optional[str] = None
    previous_version: Optional[int] = None
    current_version: Optional[int] = None
    previous_level: Optional[int] = None
    current_level: Optional[int] = None
    status: Optional[str] = None
    stage: Optional[str] = None
    checkpoint_ref: Optional[str] = None
    diagnostic_type: Optional[str] = None  # trusted host code, never inferred from an ID or prose


@dataclass(frozen=True)
class Action:
    action_id: str
    card_id: str
    expected_state_version: int
    action_type: str  # acknowledge / more_hint / return_l1
    receipt_ref: str  # verified UI receipt reference only, never token/credential


@dataclass(frozen=True)
class RetentionPolicy:
    learning_days: int = 90
    technical_log_days: int = 7
    debug_body_sampling: bool = False


class RunController(Protocol):
    def cancel_and_wait(self, run: dict) -> bool:
        """Stop requests and owned Jobs; return True only when quiescent."""
        ...
