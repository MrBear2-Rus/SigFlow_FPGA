"""Bounded, host-owned anchor catalogs; reports cannot establish trust."""

from dataclasses import fields
from pathlib import Path
import re

from ..context.models import EvidenceAnchor, SourceRef
from ..context.report import valid_anchor
from .http import MAX_BODY, strict_json


class AnchorCatalog:
    """Read one complete trusted file on every snapshot, without a cache."""

    def __init__(self, path):
        self.path = Path(path)

    def snapshot(self) -> tuple[EvidenceAnchor, ...]:
        try:
            with self.path.open('rb') as stream:
                raw = stream.read(MAX_BODY + 1)
            if len(raw) > MAX_BODY:
                raise ValueError('invalid_anchor_catalog')
            return _validate(strict_json(raw))
        except (OSError, ValueError, TypeError):
            raise ValueError('invalid_anchor_catalog') from None


def load_anchors(path):
    """Legacy one-shot loader; no configured catalog still means no anchors."""
    return () if path is None else AnchorCatalog(path).snapshot()


def _validate(document):
    if set(document) != {'anchors'} or type(document['anchors']) is not list or len(document['anchors']) > 256:
        raise ValueError('invalid_anchor_catalog')
    anchors, seen = [], set()
    for item in document['anchors']:
        if type(item) is not dict or set(item) - {f.name for f in fields(EvidenceAnchor)}:
            raise ValueError('invalid_anchor_catalog')
        item = dict(item)
        if item.get('source_ref') is not None:
            ref = item['source_ref']
            if type(ref) is not dict or set(ref) - {f.name for f in fields(SourceRef)}:
                raise ValueError('invalid_anchor_catalog')
            item['source_ref'] = SourceRef(**ref)
        anchor = EvidenceAnchor(**item)
        if (not valid_anchor(anchor, anchor.project_id, anchor.revision, anchor.job_id)
                or re.fullmatch(r'(?:sha256:)?[a-fA-F0-9]{64}', anchor.artifact_hash) is None
                or anchor.evidence_id in seen):
            raise ValueError('invalid_anchor_catalog')
        # IDs and source refs carry opaque handles, never filesystem paths.
        ids = [anchor.evidence_id, anchor.project_id, anchor.revision, anchor.job_id,
               anchor.snapshot_id, anchor.artifact_id]
        if anchor.source_ref:
            ids.append(anchor.source_ref.source_id)
        if any(re.fullmatch(r'[A-Za-z0-9_-]+(?:[.:][A-Za-z0-9_-]+)*', value) is None for value in ids):
            raise ValueError('invalid_anchor_catalog')
        seen.add(anchor.evidence_id)
        anchors.append(anchor)
    return tuple(anchors)
