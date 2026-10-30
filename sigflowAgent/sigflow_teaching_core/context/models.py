"""Internal context values; these are not new Gateway wire schemas."""

from dataclasses import dataclass


@dataclass(frozen=True)
class SourceRef:
    project_id: str
    revision: str
    source_id: str
    file_hash: str
    start_line: int
    end_line: int
    start_column: int | None = None
    end_column: int | None = None


@dataclass(frozen=True)
class EvidenceAnchor:
    evidence_id: str
    project_id: str
    revision: str
    job_id: str
    snapshot_id: str
    artifact_id: str
    artifact_hash: str
    source_ref: SourceRef | None = None
    expired: bool = False


@dataclass(frozen=True)
class ContextItem:
    item_id: str
    kind: str
    project_id: str
    revision: str
    text: str


@dataclass(frozen=True)
class Omission:
    item_id: str
    reason: str


@dataclass(frozen=True)
class ContextBundle:
    items: tuple[ContextItem, ...]
    omitted: tuple[Omission, ...]
    bytes_used: int
    estimated_tokens: int
