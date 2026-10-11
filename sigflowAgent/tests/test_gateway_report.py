import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
import unittest

from sigflow_edu_agent.clients.gateway import GatewayClient, GatewayConfig, GatewayError


class GatewayReportTests(unittest.TestCase):
    def setUp(self):
        self.report = {'schema_version': 'edu.jobreport.v1', 'job_id': 'job-1',
                       'origin': 'core', 'capability': 'eda.synth', 'project_id': 'p1',
                       'revision': 'r1', 'snapshot_id': 'snap-1', 'state': 'Succeeded',
                       'completeness': 'complete', 'diagnostics': [], 'metrics': {}, 'artifacts': []}
        self.health = {'instance_id': 'inst-1', 'protocol_version': 'edu.api.v1', 'edition': 'edu'}
        self.status, self.raw, self.paths, self.auth = 200, None, [], []
        self.version, self.request_id = 'edu.api.v1', 'req-1'
        self.error_code = 'NOT_FOUND'
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.paths.append(self.path)
                owner.auth.append(self.headers.get('Authorization'))
                health = self.path == '/api/v1/health'
                status = 200 if health else owner.status
                envelope = {'schema_version': 'edu.api.v1' if health else owner.version,
                            'request_id': 'h' if health else owner.request_id, 'trace_id': 't',
                            'data': owner.health if health else owner.report}
                if status != 200:
                    envelope.pop('data')
                    envelope['error'] = {'code': owner.error_code, 'message': 'secret test-token', 'retryable': False}
                raw = json.dumps(envelope).encode() if health or owner.raw is None else owner.raw
                self.send_response(status)
                self.send_header('Content-Length', str(len(raw)))
                self.send_header('Location', '/unexpected')
                self.end_headers()
                try:
                    self.wfile.write(raw)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass
            def log_message(self, *args):
                pass
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.close)
        self.client = GatewayClient(GatewayConfig(self.server.server_port, 'test-token', 'inst-1'))

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def read(self, **changes):
        args = dict(job_id='job-1', project_id='p1', revision='r1', snapshot_id='snap-1')
        args.update(changes)
        return self.client.job_report(**args)

    def test_read_returns_raw_report_after_unauthenticated_handshake(self):
        self.assertTrue(callable(getattr(self.client, 'job_report', None)))
        self.assertEqual(self.read(), self.report)
        self.assertEqual(self.paths, ['/api/v1/health', '/api/v1/jobs/job-1/report'])
        self.assertEqual(self.auth, [None, 'Bearer test-token'])

    def test_identifiers_fail_before_any_network_access(self):
        for key in ('job_id', 'project_id', 'revision', 'snapshot_id'):
            for bad in ('', '.', '..', '../x', 'a/b', 'a\\b', 'a?b', 'a#b', 'a%b', 'a\r\nb', '中文', 'x'*257, None, True):
                with self.subTest(key=key, bad=bad), self.assertRaisesRegex(GatewayError, 'INVALID_ARGUMENT'):
                    self.read(**{key: bad})
        self.assertEqual(self.paths, [])

    def test_bad_handshake_prevents_token_disclosure(self):
        for key, bad, code in [('instance_id', 'other', 'INSTANCE_MISMATCH'),
                               ('protocol_version', 'v0', 'PROTOCOL_MISMATCH'),
                               ('edition', 'pro', 'POLICY_DENIED')]:
            old = self.health[key]
            self.health[key] = bad
            with self.assertRaisesRegex(GatewayError, code):
                self.read()
            self.health[key] = old
        self.assertEqual(self.paths, ['/api/v1/health']*3)
        self.assertEqual(self.auth, [None]*3)

    def test_binding_and_legacy_fail_closed(self):
        for key, bad, code in [('job_id', 'other', 'INVALID_RESPONSE'), ('project_id', 'other', 'INVALID_RESPONSE'),
                               ('revision', 'old', 'STALE_REVISION'), ('snapshot_id', 'old', 'STALE_REVISION'),
                               ('schema_version', 'edu.jobreport.v0', 'PROTOCOL_MISMATCH'),
                               ('origin', 'legacy', 'INVALID_RESPONSE'), ('completeness', 'legacy_unverified', 'INVALID_RESPONSE'),
                               ('state', 'Invented', 'INVALID_RESPONSE'), ('diagnostics', None, 'INVALID_RESPONSE'),
                               ('metrics', [], 'INVALID_RESPONSE'), ('artifacts', {}, 'INVALID_RESPONSE')]:
            old = self.report[key]
            self.report[key] = bad
            with self.subTest(key=key), self.assertRaisesRegex(GatewayError, code):
                self.read()
            self.report[key] = old

    def test_nonterminal_and_incomplete_reports_remain_raw_for_evidence_layer(self):
        for state in ('Queued', 'Running', 'Cancelling', 'Cancelled', 'Succeeded', 'Failed', 'TimedOut', 'Rejected', 'Unknown'):
            self.report['state'] = state
            self.report['completeness'] = 'unavailable'
            self.assertEqual(self.read()['state'], state)

    def test_transport_errors_are_bounded_redacted_and_never_followed(self):
        for status, code in [(401, 'UNAUTHENTICATED'), (404, 'NOT_FOUND'), (409, 'STALE_REVISION'),
                             (503, 'SERVICE_UNAVAILABLE'), (302, 'REDIRECT_DENIED')]:
            self.status, self.error_code = status, code
            with self.assertRaises(GatewayError) as result:
                self.read()
            self.assertEqual(str(result.exception), code)
            self.assertEqual(self.paths[-2:], ['/api/v1/health', '/api/v1/jobs/job-1/report'])
        self.status, self.raw = 200, b'x' * (1048576 + 1)
        with self.assertRaisesRegex(GatewayError, 'RESOURCE_EXHAUSTED'):
            self.read()
        for raw in (b'secret test-token', b'[]', b'NaN'):
            self.raw = raw
            with self.assertRaisesRegex(GatewayError, 'INVALID_RESPONSE'):
                self.read()

    def test_envelope_protocol_and_identifiers_checked(self):
        self.version = 'v0'
        with self.assertRaisesRegex(GatewayError, 'PROTOCOL_MISMATCH'):
            self.read()
        self.version, self.request_id = 'edu.api.v1', ''
        with self.assertRaisesRegex(GatewayError, 'INVALID_RESPONSE'):
            self.read()

    def test_nonstandard_numbers_and_duplicate_keys_are_rejected(self):
        envelope = {'schema_version': 'edu.api.v1', 'request_id': 'r', 'trace_id': 't', 'data': self.report}
        self.report['metrics'] = {'area': float('nan')}
        self.raw = json.dumps(envelope).encode()
        with self.assertRaisesRegex(GatewayError, 'INVALID_RESPONSE'):
            self.read()
        self.report['metrics'] = {}
        self.raw = json.dumps(envelope).replace('"job_id": "job-1"', '"job_id": "other", "job_id": "job-1"').encode()
        with self.assertRaisesRegex(GatewayError, 'INVALID_RESPONSE'):
            self.read()
