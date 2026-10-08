"""Version and protocol constants for the SigFlow Edu Agent sidecar.

The protocol identifier is frozen for the whole v1 contract.  Bumping
``PROTOCOL`` requires a new versioned path and a matching schema revision.
"""

from __future__ import annotations

AGENT_VERSION: str = "0.1.0"
"""Package version reported in the stdout ready record and in /health."""

PROTOCOL: str = "edu.api.v1"
"""Frozen protocol identifier of the UI-to-Agent and Agent-to-Gateway contract."""

BOOTSTRAP_TYPE: str = "sigflow-bootstrap"
"""Required ``type`` discriminator of the single stdin bootstrap line."""

READY_TYPE: str = "ready"
"""Required ``type`` discriminator of the single stdout ready line."""

SCHEMA_VERSION: str = PROTOCOL
"""Value placed in the ``schema_version`` field of every response envelope."""

PLAN_CAPABILITIES: tuple[str, ...] = ("eda.synth", "eda.sim.build", "eda.sim.run")
"""The only three execution capabilities a PlanCard may ever reference."""

LEVELS: tuple[str, ...] = ("L1", "L2", "L3", "L4")
"""Teaching levels, from a bare question (L1) to a full reference solution (L4)."""

RUN_KINDS: tuple[str, ...] = ("explain", "hint", "plan", "report_review")
"""Request kinds accepted by ``POST /api/v1/sessions/{sid}/runs``."""

CARD_KINDS: tuple[str, ...] = (
    "rule",
    "teaching",
    "reference_solution",
    "diagnostic_explanation",
)
"""TeachingCard ``kind`` enumeration."""

MAX_CARD_BYTES: int = 64 * 1024
"""Upper bound for a single ``selection.text`` payload (64 KiB)."""

MAX_CARDS_PER_RUN: int = 16
"""Upper bound for the number of cards returned by one run."""

MAX_SESSIONS: int = 32
"""In-memory session cap; the least recently used session is evicted first."""

MAX_EVENTS_PER_SESSION: int = 1000
"""Per-session event retention window, trimmed from the oldest end."""
