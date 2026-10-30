"""Read-only report bridge using host-supplied evidence anchors.

The report route never supplies a trusted anchor catalog by itself. This
adapter does not infer source lines or evidence ownership from free text.
"""

from copy import deepcopy
import re

from ..domain import EvidenceError
from .report import ReportEvidenceSource


def _text(value):
    return type(value) is str and bool(value.strip()) and len(value) <= 4096


def normalize_report_artifacts(report):
    """Map wire artifact_id/sha256 to the internal id/hash convention.

    This is intentionally narrower than the generic report JSON schema.
    Ambiguous, path-bearing, or unhashed artifacts cannot certify evidence.
    """
    if type(report) is not dict or type(report.get('artifacts')) is not list:
        raise EvidenceError('unresolved_evidence')
    if len(report['artifacts']) > 256:
        raise EvidenceError('unresolved_evidence')
    artifacts, seen = [], set()
    for item in report['artifacts']:
        if (type(item) is not dict or not {'artifact_id', 'sha256', 'role'} <= set(item)
                or not set(item) <= {'artifact_id', 'sha256', 'role', 'schema'}
                or not _text(item['artifact_id'])
                or any(item.get(key) is not None and not _text(item[key]) for key in ('role', 'schema'))
                or type(item['sha256']) is not str
                or re.fullmatch(r'(?:sha256:)?[a-fA-F0-9]{64}', item['sha256']) is None
                or item['artifact_id'] in seen):
            raise EvidenceError('unresolved_evidence')
        seen.add(item['artifact_id'])
        # Preserve the hash string exactly, so the trusted anchor must agree.
        artifacts.append({'id': item['artifact_id'], 'hash': item['sha256']})
    normalized = deepcopy(report)
    normalized['artifacts'] = artifacts
    return normalized


class GatewayReportSource:
    def __init__(self, client, *, job_id, project_id, revision, snapshot_id,
                 diagnostic_id, anchors, combinational_goal=False, origin='tool',
                 context_items=(), max_bytes=8192, max_tokens=4096):
        if not all(_text(v) for v in (job_id, project_id, revision, snapshot_id, diagnostic_id)):
            raise EvidenceError('invalid_report')
        # Reuse the existing constructor's checks and defensive copies. An empty
        # report is not consumed here; collection always obtains a fresh report.
        ReportEvidenceSource({}, anchors, job_id=job_id, diagnostic_id=diagnostic_id,
                             combinational_goal=combinational_goal, origin=origin)
        self._client = client
        self._job_id, self._project_id, self._revision, self._snapshot_id = job_id, project_id, revision, snapshot_id
        self._options = deepcopy(dict(anchors=anchors, diagnostic_id=diagnostic_id,
                                      combinational_goal=combinational_goal, origin=origin,
                                      context_items=context_items, max_bytes=max_bytes, max_tokens=max_tokens))

    def collect(self, project_id, revision):
        if (project_id, revision) != (self._project_id, self._revision):
            raise EvidenceError('stale_report')
        report = self._client.job_report(self._job_id, project_id=project_id,
                                         revision=revision, snapshot_id=self._snapshot_id)
        if type(report) is not dict:
            raise EvidenceError('invalid_report')
        expected = (self._job_id, project_id, revision, self._snapshot_id)
        if tuple(report.get(k) for k in ('job_id', 'project_id', 'revision', 'snapshot_id')) != expected:
            raise EvidenceError('stale_report')
        normalized = normalize_report_artifacts(report)
        return ReportEvidenceSource(normalized, job_id=self._job_id, **self._options).collect(project_id, revision)
