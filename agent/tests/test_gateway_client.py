"""AD-04 tests: the sidecar's outbound channel is loopback-only and redirect-free.

These are security-boundary tests, not integration tests: they assert that a
misconfigured or hostile ``gateway_url`` can never carry the Agent→EDA bearer
token off the machine, and that a redirect or a proxy environment variable
cannot re-route a credentialed request.
"""

from __future__ import annotations

import os
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Optional
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))

from sigflow_edu_agent.gateway_client import (  # noqa: E402
    GatewayClient,
    parse_loopback_base_url,
)


class CountingHandler(BaseHTTPRequestHandler):
    """Counts every request and answers with a fixed status/body.

    Each :class:`ServerFixture` derives its **own** subclass so two servers never
    share one counter (a shared class attribute would silently alias them).
    """

    hits = 0
    status_code = 200
    body = b'{"schema_version":"edu.api.v1","request_id":"req-1","trace_id":"trace-1","data":{"ok":true}}'
    location: Optional[str] = None

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        type(self).hits += 1
        self.send_response(self.status_code)
        if self.location:
            self.send_header("Location", self.location)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(self.body)))
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, *args: Any) -> None:  # noqa: D102 - silence the test log
        return


class ServerFixture:
    """A loopback HTTP server used as a stand-in Gateway."""

    def __init__(self, **attrs: Any) -> None:
        # 每个实例一个独立子类，计数互不串扰。
        self.handler = type("CountingHandlerInstance", (CountingHandler,), {"hits": 0, **attrs})
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), self.handler)
        self.server.daemon_threads = True
        self.port = int(self.server.server_address[1])
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def hits(self) -> int:
        return int(self.handler.hits)

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


class BaseUrlValidationTests(unittest.TestCase):
    """The bootstrap address is validated before any request is issued."""

    def test_accepts_loopback_endpoints(self) -> None:
        self.assertEqual(parse_loopback_base_url("http://127.0.0.1:1234"), ("127.0.0.1", 1234))
        self.assertEqual(parse_loopback_base_url("http://localhost:80"), ("localhost", 80))
        self.assertEqual(parse_loopback_base_url("http://[::1]:9"), ("::1", 9))

    def test_rejects_everything_that_could_leave_the_machine(self) -> None:
        rejected = [
            "https://127.0.0.1:1234",          # scheme downgrade/upgrade
            "http://10.0.0.5:1234",            # non-loopback literal
            "http://example.com:1234",         # hostname that may resolve anywhere
            "http://user:pass@127.0.0.1:1",    # userinfo
            "http://127.0.0.1",                # no explicit port
            "http://127.0.0.1:0",              # port 0 is not dialable
            "http://127.0.0.1:1234/api/v1",    # path
            "http://127.0.0.1:1234?x=1",       # query
            "http://127.0.0.1:1234#frag",      # fragment
            "ftp://127.0.0.1:1234",            # wrong scheme
            "",                                # empty
        ]
        for candidate in rejected:
            with self.subTest(candidate=candidate):
                self.assertIsNone(parse_loopback_base_url(candidate))

    def test_blocked_client_never_issues_a_request(self) -> None:
        target = ServerFixture()
        try:
            client = GatewayClient("http://example.com:%d" % target.port, "tok-test")
            self.assertTrue(client.blocked)
            outcome = client.fetch_job_report("job-1")
            self.assertFalse(outcome.ok)
            self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")
            self.assertEqual(target.hits, 0)
        finally:
            target.close()


class RedirectTests(unittest.TestCase):
    """A redirect must be refused, not followed with the bearer token attached."""

    def test_redirect_is_refused_and_target_never_contacted(self) -> None:
        target = ServerFixture()
        gateway = ServerFixture(
            status_code=302, location="http://127.0.0.1:%d/steal" % target.port
        )
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.fetch_job_report("job-1")
            self.assertFalse(outcome.ok)
            self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")
            self.assertEqual(gateway.hits, 1, "the original request was issued once")
            self.assertEqual(target.hits, 0, "the redirect target must never be contacted")
        finally:
            gateway.close()
            target.close()

    def test_proxy_environment_variable_is_ignored(self) -> None:
        gateway = ServerFixture()
        proxy = ServerFixture()
        try:
            with mock.patch.dict(
                os.environ, {"http_proxy": "http://127.0.0.1:%d" % proxy.port}, clear=False
            ):
                client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
                outcome = client.fetch_job_report("job-1")
            self.assertTrue(outcome.ok, outcome.message)
            self.assertEqual(gateway.hits, 1, "the request reached the Gateway directly")
            self.assertEqual(proxy.hits, 0, "an http_proxy must not be used for the token")
        finally:
            gateway.close()
            proxy.close()


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
