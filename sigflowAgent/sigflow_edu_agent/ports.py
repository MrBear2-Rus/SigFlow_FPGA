"""Trusted host ports. Model output must never implement these interfaces."""

from __future__ import annotations

from typing import Protocol
from .domain import CardContext, CourseBundle, Evidence, Plan, VerificationResult
from .checkers.plan import DetailedPlan


class EvidenceSource(Protocol):
    def collect(self, project_id: str, revision: str) -> Evidence | None: ...


class CardProvider(Protocol):
    def generate(self, context: CardContext) -> dict: ...


class CourseProvider(Protocol):
    def prepare(self, question: str, concept_ids: tuple[str, ...]) -> CourseBundle: ...


class TeachingMemoryPort(Protocol):
    """U2 composition adapter; UA-10's own storage and API stay external.

    Facts contain committed metadata, not question/card/source bodies. An ACK
    must contain accepted=True and the exact event_id after durable receipt.
    History returns exactly learner_id/project_id/concept_ids/summary. These
    signatures describe an internal U2 adapter, not a frozen shared HTTP DTO.
    """
    def record_teaching_fact(self, fact: dict) -> dict: ...

    def get_teaching_context(self, *, learner_id: str, project_id: str,
                             concept_ids: list[str], max_bytes: int) -> dict: ...


class VerificationPort(Protocol):
    """Adapter validates real grant/snapshot/params and owns Job waiting/cancel.

    execute must reuse the supplied idempotency key and return a bounded Job
    observation. Nonterminal/unknown observations require host reconciliation;
    they never authorize a following step or prove success. sim.run inputs
    must not resolve outside this plan's successful build.
    This synchronous development interface is not a shared Gateway DTO.
    """

    def authorized(self, plan: Plan | DetailedPlan, grant_ref: str) -> bool: ...

    def execute(self, plan: Plan | DetailedPlan, step_index: int, grant_ref: str,
                idempotency_key: str) -> VerificationResult: ...

    def observe(self, plan: Plan | DetailedPlan, step_index: int,
                idempotency_key: str) -> VerificationResult:
        """Observe an already reserved step; never submit, retry POST or grant successors."""
        ...


class DenyVerification:
    def authorized(self, plan, grant_ref):
        return False

    def execute(self, plan, step_index, grant_ref, idempotency_key):
        raise PermissionError('verification_unavailable')

    def observe(self, plan, step_index, idempotency_key):
        raise PermissionError('verification_unavailable')
