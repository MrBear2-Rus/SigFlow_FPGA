"""Normalize an explicitly selected report; never parse raw EDA logs here."""

from copy import deepcopy

from ..domain import Evidence, EvidenceError
from .builder import build_context
from .models import EvidenceAnchor, SourceRef


def _text(value):
    if type(value) is not str or not value.strip() or len(value) > 8192:
        return False
    try:
        value.encode('utf-8')
    except UnicodeError:
        return False
    return True


def valid_source_ref(ref, project_id, revision):
    if not isinstance(ref, SourceRef):
        return False
    if (ref.project_id, ref.revision) != (project_id, revision):
        return False
    if not all(_text(value) for value in (ref.source_id, ref.file_hash)):
        return False
    if not all(type(n) is int and n >= 1 for n in (ref.start_line, ref.end_line)) or ref.end_line < ref.start_line:
        return False
    for column in (ref.start_column, ref.end_column):
        if column is not None and (type(column) is not int or column < 1):
            return False
    return not (ref.start_line == ref.end_line and ref.start_column is not None
                and ref.end_column is not None and ref.end_column < ref.start_column)


def valid_anchor(anchor, project_id, revision, job_id):
    if not isinstance(anchor, EvidenceAnchor):
        return False
    fields = (anchor.evidence_id, anchor.project_id, anchor.revision, anchor.job_id,
              anchor.snapshot_id, anchor.artifact_id, anchor.artifact_hash)
    return (all(_text(value) for value in fields)
            and type(anchor.expired) is bool and not anchor.expired
            and (anchor.project_id, anchor.revision, anchor.job_id) == (project_id, revision, job_id)
            and (anchor.source_ref is None or valid_source_ref(anchor.source_ref, project_id, revision)))


class ReportEvidenceSource:
    def __init__(self, report, anchors, *, job_id, diagnostic_id, combinational_goal=False,
                 origin='fixture', context_items=(), max_bytes=8192, max_tokens=4096):
        if origin not in ('fixture', 'tool') or type(combinational_goal) is not bool:
            raise EvidenceError('invalid_report')
        if not _text(job_id) or not _text(diagnostic_id):
            raise EvidenceError('invalid_report')
        if not isinstance(anchors, (tuple, list)) or len(anchors) > 256:
            raise EvidenceError('unresolved_evidence')
        self._anchors = {}
        for anchor in anchors:
            if not isinstance(anchor, EvidenceAnchor) or not _text(anchor.evidence_id) or anchor.evidence_id in self._anchors:
                raise EvidenceError('unresolved_evidence')
            self._anchors[anchor.evidence_id] = deepcopy(anchor)
        self._report = deepcopy(report)
        self.job_id, self.diagnostic_id = job_id, diagnostic_id
        self.combinational_goal, self.origin = combinational_goal, origin
        self._context_items = deepcopy(context_items)
        self._max_bytes, self._max_tokens = max_bytes, max_tokens

    def collect(self, project_id, revision):
        report = self._report
        if not isinstance(report, dict) or report.get('schema_version') != 'edu.jobreport.v1':
            raise EvidenceError('invalid_report')
        if (report.get('project_id'), report.get('revision'), report.get('job_id')) != (project_id, revision, self.job_id):
            raise EvidenceError('stale_report')
        if report.get('origin') not in ('core', 'legacy') or report.get('state') not in ('Succeeded', 'Failed'):
            raise EvidenceError('invalid_report')
        if not _text(report.get('capability')):
            raise EvidenceError('invalid_report')
        if (report.get('completeness') != 'complete' or not _text(report.get('snapshot_id'))
                or not _text(report.get('input_fingerprint'))):
            raise EvidenceError('incomplete_report')
        diagnostics = report.get('diagnostics')
        if not isinstance(diagnostics, list) or not all(isinstance(d, dict) for d in diagnostics):
            raise EvidenceError('invalid_report')
        matches = [d for d in diagnostics if d.get('id') == self.diagnostic_id]
        if len(matches) != 1:
            raise EvidenceError('diagnostic_missing')
        diagnostic = matches[0]
        if (not all(_text(diagnostic.get(k)) for k in ('id', 'code', 'summary'))
                or type(diagnostic.get('stage')) is not str
                or diagnostic.get('origin') not in ('core', 'legacy')
                or diagnostic.get('severity') not in ('info', 'warning', 'error')):
            raise EvidenceError('invalid_report')
        confidence = diagnostic.get('confidence_kind')
        if confidence == 'hypothesis':
            raise EvidenceError('hypothesis_not_fact')
        if confidence not in ('tool', 'rule'):
            raise EvidenceError('invalid_report')
        refs = diagnostic.get('evidence_refs')
        if not isinstance(refs, list) or not refs or not all(_text(ref) for ref in refs) or len(set(refs)) != len(refs):
            raise EvidenceError('unresolved_evidence')
        # Artifact id/hash normalization is an internal adapter convention, not
        # a new requirement silently added to the shared Gateway schema.
        artifacts = report.get('artifacts')
        if not isinstance(artifacts, list) or not all(isinstance(a, dict) for a in artifacts):
            raise EvidenceError('unresolved_evidence')
        for ref in refs:
            anchor = self._anchors.get(ref)
            if anchor is not None and anchor.expired is True:
                raise EvidenceError('expired_evidence')
            if not valid_anchor(anchor, project_id, revision, self.job_id):
                raise EvidenceError('unresolved_evidence')
            if anchor.snapshot_id != report['snapshot_id']:
                raise EvidenceError('stale_report')
            if not any(a.get('id') == anchor.artifact_id and a.get('hash') == anchor.artifact_hash for a in artifacts):
                raise EvidenceError('unresolved_evidence')
        context = build_context(self._context_items, project_id, revision,
                                max_bytes=self._max_bytes, max_tokens=self._max_tokens)
        primary = self._anchors[refs[0]]
        concept = 'latch' if diagnostic['code'] == 'EDU_LATCH_INFERRED' else 'unknown'
        return Evidence(primary.evidence_id, project_id, revision, self.job_id, concept, True,
                        diagnostic['summary'], self.combinational_goal, self.origin,
                        confidence, primary, context.items, context.omitted)
