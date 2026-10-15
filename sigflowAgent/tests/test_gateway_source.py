from copy import deepcopy
from dataclasses import replace
import json
from pathlib import Path
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

from sigflow_edu_agent.clients.gateway import GatewayClient, GatewayConfig
from sigflow_edu_agent.context.gateway_report import GatewayReportSource, normalize_report_artifacts
from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.domain import EvidenceError, RunRequest, Status
from sigflow_edu_agent.loop import TeachingLoop


def fixture():
    path = Path(__file__).resolve().parents[2] / 'contracts/edu-agent/v1/fixtures/golden.json'
    cases = json.loads(path.read_text(encoding='utf-8'))
    report = next(c['document'] for c in cases if c['name'] == 'job-report-view.core')
    diagnostic = next(c['document'] for c in cases if c['name'] == 'diagnostic.latch')
    report['diagnostics'] = [diagnostic]
    report['artifacts'] = [{'artifact_id': 'artifact-1', 'sha256': 'a' * 64, 'role': 'report'}]
    anchor = EvidenceAnchor('ev-1', 'prj-1', 'rev-12', 'job-21', 'snap-12', 'artifact-1', 'a' * 64)
    return report, anchor


class ReportClient:
    """Trusted port double: no network, preserves HTTP report wire fields."""
    def __init__(self, report):
        self.report, self.calls = report, []

    def job_report(self, job_id, **bindings):
        self.calls.append((job_id, bindings))
        return deepcopy(self.report)


def source(client, anchors, **changes):
    options = dict(job_id='job-21', project_id='prj-1', revision='rev-12', snapshot_id='snap-12',
                   diagnostic_id='diag-1', anchors=anchors, combinational_goal=True, origin='fixture')
    return GatewayReportSource(client, **(options | changes))


class GatewaySourceTests(unittest.TestCase):
    def test_cpp_optional_schema_and_unknown_role_do_not_change_hash_binding(self):
        report, anchor = fixture()
        report['artifacts'][0].update(schema=None, role=None)
        mapped = normalize_report_artifacts(report)
        self.assertEqual(mapped['artifacts'], [{'id': 'artifact-1', 'hash': 'a' * 64}])
        self.assertEqual(source(ReportClient(report), [anchor]).collect('prj-1', 'rev-12').anchor, anchor)
        report['artifacts'][0]['schema'] = []
        with self.assertRaises(EvidenceError):
            normalize_report_artifacts(report)

    def test_wire_artifacts_are_mapped_without_mutating_or_fabricating_anchors(self):
        report, anchor = fixture()
        normalized = normalize_report_artifacts(report)
        self.assertEqual(normalized['artifacts'], [{'id': 'artifact-1', 'hash': 'a' * 64}])
        self.assertIn('artifact_id', report['artifacts'][0])
        client = ReportClient(report)
        evidence = source(client, [anchor]).collect('prj-1', 'rev-12')
        self.assertEqual(evidence.anchor, anchor)
        self.assertIsNone(evidence.anchor.source_ref)
        self.assertEqual(client.calls, [('job-21', {'project_id': 'prj-1', 'revision': 'rev-12', 'snapshot_id': 'snap-12'})])

    def test_bound_source_refuses_other_scope_before_network(self):
        report, anchor = fixture()
        client = ReportClient(report)
        adapter = source(client, [anchor])
        for project, revision in (('other', 'rev-12'), ('prj-1', 'old')):
            with self.assertRaisesRegex(EvidenceError, 'stale_report'):
                adapter.collect(project, revision)
        self.assertEqual(client.calls, [])

    def test_untrusted_artifact_shapes_cannot_supply_evidence(self):
        report, _ = fixture()
        valid = report['artifacts'][0]
        variants = [[dict(valid, sha256='bad')], [valid, valid],
                    [{'id': 'artifact-1', 'hash': 'a' * 64}],
                    [dict(valid, path='C:/private/key')], [None], None]
        for artifacts in variants:
            with self.subTest(artifacts=artifacts), self.assertRaises(EvidenceError):
                normalize_report_artifacts(dict(report, artifacts=artifacts))

    def test_missing_anchor_or_hash_mismatch_is_waiting_evidence(self):
        report, anchor = fixture()
        for anchors in ([], [replace(anchor, artifact_hash='b' * 64)], [replace(anchor, expired=True)]):
            adapter = source(ReportClient(report), anchors)
            loop = TeachingLoop(RunRequest('prj-1', 'rev-12', 'issue', '解释'), adapter)
            state = loop.run_until_pause()
            self.assertEqual(state.status, Status.WAITING_EVIDENCE)
            self.assertEqual(state.cards, [])

    def test_report_scope_checked_even_if_client_double_does_not_check(self):
        report, anchor = fixture()
        for field in ('job_id', 'project_id', 'revision', 'snapshot_id'):
            with self.subTest(field=field), self.assertRaises(EvidenceError):
                source(ReportClient(dict(report, **{field: 'wrong'})), [anchor]).collect('prj-1', 'rev-12')

    def test_empty_or_incomplete_report_is_not_assumed_clean(self):
        report, anchor = fixture()
        for changes in ({'diagnostics': []}, {'completeness': 'partial'}, {'completeness': 'legacy_unverified'},
                        {'state': 'Running'}, {'input_fingerprint': None}):
            loop = TeachingLoop(RunRequest('prj-1', 'rev-12', 'issue', '解释'),
                                source(ReportClient(dict(report, **changes)), [anchor]))
            self.assertEqual(loop.run_until_pause().status, Status.WAITING_EVIDENCE)

    def test_source_copies_host_anchors_and_rechecks_on_each_collection(self):
        report, anchor = fixture()
        anchors, client = [anchor], ReportClient(report)
        adapter = source(client, anchors)
        anchors.clear()
        self.assertEqual(adapter.collect('prj-1', 'rev-12').anchor, anchor)
        client.report['revision'] = 'changed'
        with self.assertRaises(EvidenceError):
            adapter.collect('prj-1', 'rev-12')
        self.assertEqual(len(client.calls), 2)


class GatewaySourceHTTPTests(unittest.TestCase):
    def setUp(self):
        self.report, self.anchor = fixture()
        self.requests = []
        self.status = 200
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.requests.append((self.path, self.headers.get('Authorization')))
                status = 200 if self.path == '/api/v1/health' else owner.status
                if self.path == '/api/v1/health':
                    data = {'protocol_version': 'edu.api.v1', 'instance_id': 'inst-1', 'edition': 'edu'}
                elif self.path == '/api/v1/jobs/job-21/report':
                    data = owner.report
                else:
                    status, data = 404, {}
                envelope = {'schema_version': 'edu.api.v1', 'request_id': 'req-1', 'trace_id': 'trace-1'}
                if status == 200:
                    envelope['data'] = data
                else:
                    envelope['error'] = {'code': 'NOT_FOUND', 'message': 'private-server-text', 'retryable': False}
                raw = json.dumps(envelope).encode('utf-8')
                self.send_response(status)
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def log_message(self, *args): pass

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = Thread(target=self.server.serve_forever, kwargs={'poll_interval': 0.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        client = GatewayClient(GatewayConfig(self.server.server_port, 'fixture-token', 'inst-1'))
        self.adapter = source(client, [self.anchor])

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def test_http_report_reaches_loop_and_hint_rereads_changed_revision(self):
        loop = TeachingLoop(RunRequest('prj-1', 'rev-12', 'issue', '解释锁存器'), self.adapter)
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual(state.cards[0].evidence_ids, ('ev-1',))
        self.assertEqual(self.requests, [('/api/v1/health', None),
                                        ('/api/v1/jobs/job-21/report', 'Bearer fixture-token')])
        self.report['revision'] = 'old'
        loop.act('hint_next', 'next', state.version)
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(len(state.cards), 1)
        self.assertEqual(len(self.requests), 4)

    def test_gateway_failure_does_not_publish_error_body_as_teaching(self):
        self.status = 404
        loop = TeachingLoop(RunRequest('prj-1', 'rev-12', 'issue', '解释'), self.adapter)
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(state.cards, [])
        self.assertNotIn('private-server-text', state.reason)
