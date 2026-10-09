import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
import unittest

from sigflow_edu_agent.clients.gateway import GatewayClient, GatewayConfig, GatewayError


class GatewayTests(unittest.TestCase):
    def setUp(self):
        cases = json.loads((Path(__file__).resolve().parents[2] / 'contracts/edu-agent/v1/fixtures/golden.json').read_text(encoding='utf-8'))
        self.payload = next(c['document'] for c in cases if c['name'] == 'capabilities.edu')
        self.envelope_version, self.status, self.paths, self.headers = 'edu.api.v1', 200, [], []
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.paths.append(self.path)
                owner.headers.append(self.headers.get('Authorization'))
                data = owner.payload
                if self.path.endswith('/health'):
                    data = {k: owner.payload[k] for k in ('instance_id', 'protocol_version', 'edition')}
                envelope = {'schema_version': owner.envelope_version, 'request_id': 'req1', 'trace_id': 'trace1', 'data': data}
                if owner.status != 200:
                    envelope = {'schema_version': 'edu.api.v1', 'request_id': 'r', 'trace_id': 't',
                                'error': {'code': 'UNAUTHENTICATED', 'message': 'secret-server-error', 'retryable': False}}
                raw = json.dumps(envelope).encode()
                self.send_response(owner.status)
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
            def log_message(self, *args):
                pass
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = Thread(target=self.server.serve_forever, kwargs={'poll_interval': 0.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        self.client = GatewayClient(GatewayConfig(self.server.server_port, 'test-token', 'inst-1'))

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def test_real_http_uses_only_frozen_routes_and_scope(self):
        self.assertEqual(self.client.health()['instance_id'], 'inst-1')
        capabilities = self.client.capabilities()
        self.assertEqual(capabilities.available_ids, ('eda.synth', 'eda.sim.build', 'eda.sim.run'))
        self.assertEqual(self.paths, ['/api/v1/health', '/api/v1/capabilities'])
        self.assertEqual(self.headers, [None, 'Bearer test-token'])
        self.assertNotIn('test-token', repr(self.client.config))

    def test_disabled_or_unknown_capabilities_are_not_usable(self):
        self.payload['capabilities'][0]['disabled'] = True
        self.payload['capabilities'].append({'id': 'shell', 'ready': True, 'disabled': False})
        self.assertEqual(self.client.capabilities().available_ids, ('eda.sim.build', 'eda.sim.run'))

    def test_instance_and_schema_mismatch_fail_closed(self):
        self.payload['instance_id'] = 'other'
        with self.assertRaisesRegex(GatewayError, 'INSTANCE_MISMATCH'):
            self.client.health()
        self.payload['instance_id'] = 'inst-1'
        self.envelope_version = 'v2'
        with self.assertRaisesRegex(GatewayError, 'PROTOCOL_MISMATCH'):
            self.client.health()

    def test_string_booleans_and_duplicate_capabilities_are_rejected(self):
        self.payload['capabilities'][0]['ready'] = 'true'
        with self.assertRaisesRegex(GatewayError, 'INVALID_RESPONSE'):
            self.client.capabilities()
        self.payload['capabilities'][0]['ready'] = True
        self.payload['capabilities'].append(dict(self.payload['capabilities'][0]))
        with self.assertRaisesRegex(GatewayError, 'INVALID_RESPONSE'):
            self.client.capabilities()

    def test_errors_are_redacted_and_not_retried_or_redirected(self):
        for status in (401, 302):
            self.status = status
            with self.assertRaises(GatewayError) as error:
                self.client.capabilities()
            self.assertNotIn('secret-server-error', str(error.exception))
        self.assertEqual(len(self.paths), 2)

    def test_invalid_connection_configuration_rejected(self):
        for port in (0, 65536, True, '80'):
            with self.assertRaises(ValueError):
                GatewayConfig(port, 'token', 'inst1')
        with self.assertRaises(ValueError):
            GatewayConfig(80, 'bad\r\ntoken', 'inst1')


if __name__ == '__main__':
    unittest.main()
