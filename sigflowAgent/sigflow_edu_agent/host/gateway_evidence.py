"""Explicit Gateway trust mode: fresh, scoped metadata; no paths or log parsing."""

import hashlib
import json

from ..clients.gateway import GatewayClient, GatewayConfig
from ..context.gateway_report import normalize_report_artifacts
from ..context.models import EvidenceAnchor
from ..context.report import ReportEvidenceSource
from ..domain import Evidence, EvidenceError


class GatewayEvidenceCatalog:
    """Certify one report and at most 16 artifacts per collection, without cache.

    Evidence refs in real diagnostics may name verified artifact IDs. Opaque
    aliases require the external AnchorCatalog; free text never assigns a ref.
    The reserved job-report selection produces a task observation, no diagnosis.
    """

    MAX_ARTIFACTS = 16
    _PROVIDERS = {'eda.synth': 'eda-synth-yosys',
                  'eda.sim.build': 'eda-sim-verilator', 'eda.sim.run': 'eda-sim-verilator-run'}

    def __init__(self, gateway: GatewayConfig):
        if not isinstance(gateway, GatewayConfig):
            raise ValueError('invalid_gateway_evidence_config')
        self.gateway = gateway
        self._client = GatewayClient(gateway)

    def collect(self, job_id, *, project_id, revision, snapshot_id, diagnostic_id='job-report'):
        for value in (job_id, project_id, revision, snapshot_id, diagnostic_id):
            self._client._identifier(value)
        report = self._client.job_report(job_id, project_id=project_id, revision=revision,
                                         snapshot_id=snapshot_id)
        if (report.get('origin') != 'core' or report.get('completeness') != 'complete'
                or type(report.get('input_fingerprint')) is not str
                or not report['input_fingerprint'].strip()
                or len(report['input_fingerprint']) > 4096
                or report.get('state') not in ('Succeeded', 'Failed')
                or self._PROVIDERS.get(report.get('capability')) != report.get('plugin_id')
                or report.get('capability') not in self._PROVIDERS
                or type(report.get('diagnostics')) is not list
                or not all(type(d) is dict for d in report['diagnostics'])):
            raise EvidenceError('incomplete_report')
        if diagnostic_id == 'job-report' and (report['state'] != 'Succeeded'
                or type(report.get('exit_code')) is not int or report['exit_code'] != 0):
            raise EvidenceError('incomplete_report')
        normalized = normalize_report_artifacts(report)
        if not 1 <= len(normalized['artifacts']) <= self.MAX_ARTIFACTS:
            raise EvidenceError('unresolved_evidence')
        # Validate all shapes and identifiers before issuing metadata requests.
        for artifact in normalized['artifacts']:
            self._client._identifier(artifact['id'])
        anchors = []
        for wire, artifact in zip(report['artifacts'], normalized['artifacts']):
            metadata = self._client.artifact_metadata(artifact['id'], project_id=project_id,
                revision=revision, job_id=job_id, sha256=artifact['hash'])
            # Core metadata role is the plugin-local artifact ID, whereas the
            # report role is the plugin's role label. Ownership is certified by
            # scoped ID/hash, never by either role label. Schemas do agree.
            if (wire.get('schema') or '') != metadata['schema']:
                raise EvidenceError('unresolved_evidence')
            anchors.append(EvidenceAnchor(artifact['id'], project_id, revision, job_id,
                                          snapshot_id, artifact['id'], artifact['hash']))
        if diagnostic_id != 'job-report':
            return ReportEvidenceSource(normalized, anchors, job_id=job_id,
                diagnostic_id=diagnostic_id, origin='tool').collect(project_id, revision)
        # Hash the entire structured report, not diagnostic prose interpreted as
        # source locations or ownership. A changed report receives a new ID.
        digest = hashlib.sha256(json.dumps(report, sort_keys=True, separators=(',', ':'),
            ensure_ascii=True, allow_nan=False).encode('utf-8')).hexdigest()
        primary = anchors[0]
        anchor = EvidenceAnchor('gateway-' + digest, project_id, revision, job_id,
                                snapshot_id, primary.artifact_id, primary.artifact_hash)
        summary = (f"Core {report['capability']} Job {job_id}: Succeeded, exit_code=0; "
                   f"project={project_id}, revision={revision}, snapshot={snapshot_id}; "
                   f"完整结构化报告与 {len(anchors)} 个产物已核验。"
                   '仅证明该工具任务完成，不证明功能正确，也不证明诊断消失代表修复。')
        return Evidence(anchor.evidence_id, project_id, revision, job_id, 'job_observation',
                        True, summary, False, 'tool', 'tool', anchor)
