"""Trusted host ports. Model output must never implement these interfaces."""

from typing import Protocol
from .domain import CardContext, Evidence, Plan, VerificationResult


class EvidenceSource(Protocol):
    def collect(self, project_id: str, revision: str) -> Evidence | None: ...


class CardProvider(Protocol):
    def generate(self, context: CardContext) -> dict: ...


class VerificationPort(Protocol):
    """Adapter validates real grant/snapshot/params and owns Job waiting/cancel.

    execute must reuse the supplied idempotency key, return a terminal Job
    observation, and never resolve sim.run inputs outside this plan's build.
    This synchronous development interface is not a shared Gateway DTO.
    """

    def authorized(self, plan: Plan, grant_ref: str) -> bool: ...

    def execute(self, plan: Plan, step_index: int, grant_ref: str,
                idempotency_key: str) -> VerificationResult: ...


class DenyVerification:
    def authorized(self, plan, grant_ref):
        return False

    def execute(self, plan, step_index, grant_ref, idempotency_key):
        raise PermissionError('verification_unavailable')
