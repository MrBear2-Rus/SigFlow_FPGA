"""The real acceptance transport must not churn sockets on every poll."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from threading import Thread
import time
import unittest

from sigflow_edu_agent.host.closed_loop import HTTP, RunFailure


class AcceptanceHTTPTests(unittest.TestCase):
    def test_lost_mutation_response_is_never_automatically_repeated(self):
        accepted = []
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                accepted.append(self.path)
                self.close_connection = True
            def log_message(self, *args):
                pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        Thread(target=server.serve_forever, kwargs={'poll_interval':.01}, daemon=True).start()
        client = HTTP(server.server_port, 'synthetic-test-token', time.monotonic()+5)
        self.addCleanup(client.close)
        with self.assertRaises(RunFailure):
            client.request('POST', '/api/v1/synthetic-mutation', {})
        self.assertEqual(accepted, ['/api/v1/synthetic-mutation'])

    def test_fully_consumed_poll_responses_reuse_connection(self):
        self.exercise(close_after_response=False)

    def test_idle_peer_close_reconnects_only_a_safe_read(self):
        self.exercise(close_after_response=True)

    def exercise(self, *, close_after_response):
        peers = set()
        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def do_GET(self):
                peers.add(self.client_address)
                raw = json.dumps(dict(schema_version='edu.api.v1', request_id='req', trace_id='trace', data={'ready':True})).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                self.close_connection = close_after_response
            def log_message(self, *args):
                pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        Thread(target=server.serve_forever, kwargs={'poll_interval':.01}, daemon=True).start()
        client = HTTP(server.server_port, 'synthetic-test-token', time.monotonic()+5)
        self.addCleanup(lambda: getattr(client, 'close', lambda:None)())
        for _ in range(3):
            self.assertTrue(client.request('GET', '/api/v1/health')['ready'])
        self.assertEqual(len(peers), 3 if close_after_response else 1)
