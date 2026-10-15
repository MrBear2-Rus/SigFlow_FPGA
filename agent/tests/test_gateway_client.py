"""T9 / AD-04 tests: the sidecar's outbound channel is loopback-only and closed.

These are security-boundary tests, not integration tests.  They assert that a
misconfigured or hostile ``gateway_url`` can never carry the Agent→EDA bearer
token off the machine, that a redirect or a proxy environment variable cannot
re-route a credentialed request, that a failed POST is never replayed, that the
Agent identity can never issue a grant or a receipt, and that the response and
identifier budgets hold.
"""

from __future__ import annotations

import json
import os
import sys
import threading
import unittest
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Optional
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))

from sigflow_edu_agent.gateway_client import (  # noqa: E402
    MAX_REPORT_BYTES,
    _DOMAIN_ERROR_CODES,
    GatewayClient,
    parse_loopback_base_url,
)
from sigflow_edu_agent.errors import ERROR_CODES  # noqa: E402


_ENVELOPE_PREFIX = (
    b'{"schema_version":"edu.api.v1","request_id":"req-1","trace_id":"trace-1",'
    b'"data":{"pad":"'
)
_ENVELOPE_SUFFIX = b'"}}'


def envelope_padding(size: int) -> int:
    """Length of the filler string inside a ``size``-byte success envelope."""
    return size - len(_ENVELOPE_PREFIX) - len(_ENVELOPE_SUFFIX)


def valid_envelope(size: int) -> bytes:
    """Build a valid success envelope of exactly *size* bytes."""
    padding = envelope_padding(size)
    assert padding >= 0, "the requested envelope is too small"
    return _ENVELOPE_PREFIX + b"x" * padding + _ENVELOPE_SUFFIX


def error_envelope(code: str, message: str) -> bytes:
    """Build a failure envelope carrying one error code."""
    return json.dumps(
        {
            "schema_version": "edu.api.v1",
            "request_id": "req-1",
            "trace_id": "trace-1",
            "error": {
                "code": code,
                "message": message,
                "retryable": False,
                "details": None,
            },
        }
    ).encode("utf-8")


class RecordingHandler(BaseHTTPRequestHandler):
    """Records every request and answers with a fixed status/body.

    Each :class:`ServerFixture` derives its **own** subclass so two servers never
    share one counter or one request log (a shared class attribute would
    silently alias them).
    """

    hits = 0
    seen: list = []
    status_code = 200
    body = b'{"schema_version":"edu.api.v1","request_id":"req-1","trace_id":"trace-1","data":{"ok":true}}'
    location: Optional[str] = None

    def _respond(self) -> None:
        handler = type(self)
        handler.hits += 1
        length = int(self.headers.get("Content-Length") or 0)
        payload = self.rfile.read(length) if length > 0 else b""
        handler.seen.append(
            {
                "method": self.command,
                "path": self.path,
                "authorization": self.headers.get("Authorization", ""),
                "host": self.headers.get("Host", ""),
                "idempotency_key": self.headers.get("Idempotency-Key", ""),
                "body_bytes": len(payload),
            }
        )
        self.send_response(self.status_code)
        if self.location:
            self.send_header("Location", self.location)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(self.body)))
        self.end_headers()
        try:
            self.wfile.write(self.body)
        except (BrokenPipeError, ConnectionResetError):
            # The client stopped reading (for example after hitting the 2 MiB
            # response budget).  That is the behaviour under test, not an error.
            return

    do_GET = _respond  # noqa: N815 - BaseHTTPRequestHandler API
    do_POST = _respond  # noqa: N815 - BaseHTTPRequestHandler API

    def log_message(self, *args: Any) -> None:  # noqa: D102 - silence the test log
        return


class ServerFixture:
    """A loopback HTTP server used as a stand-in Gateway."""

    def __init__(self, **attrs: Any) -> None:
        # 每个实例一个独立子类，计数与请求日志互不串扰。
        self.handler = type(
            "RecordingHandlerInstance",
            (RecordingHandler,),
            {"hits": 0, "seen": [], **attrs},
        )
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), self.handler)
        self.server.daemon_threads = True
        self.port = int(self.server.server_address[1])
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def hits(self) -> int:
        return int(self.handler.hits)

    @property
    def seen(self) -> list:
        """Every request this stand-in Gateway received, in order."""
        return list(self.handler.seen)

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
                os.environ,
                {
                    "http_proxy": "http://127.0.0.1:%d" % proxy.port,
                    "HTTP_PROXY": "http://127.0.0.1:%d" % proxy.port,
                    "https_proxy": "http://127.0.0.1:%d" % proxy.port,
                    "HTTPS_PROXY": "http://127.0.0.1:%d" % proxy.port,
                    "all_proxy": "http://127.0.0.1:%d" % proxy.port,
                    "ALL_PROXY": "http://127.0.0.1:%d" % proxy.port,
                },
                clear=False,
            ):
                client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
                outcome = client.fetch_job_report("job-1")
            self.assertTrue(outcome.ok, outcome.message)
            self.assertEqual(gateway.hits, 1, "the request reached the Gateway directly")
            self.assertEqual(proxy.hits, 0, "no proxy variable may be used for the token")
        finally:
            gateway.close()
            proxy.close()


class NoRetryTests(unittest.TestCase):
    """A failed call is reported, never replayed by the client."""

    def test_failed_post_is_sent_exactly_once(self) -> None:
        gateway = ServerFixture(status_code=500, body=error_envelope("SERVICE_UNAVAILABLE", "busy"))
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.submit("proj-1", {"snapshot_id": "snap-1"}, "key-1")
            self.assertFalse(outcome.ok)
            self.assertEqual(gateway.hits, 1, "a POST must not be retried automatically")
            self.assertEqual(gateway.seen[0]["idempotency_key"], "key-1")
        finally:
            gateway.close()

    def test_failed_get_is_sent_exactly_once(self) -> None:
        gateway = ServerFixture(status_code=503, body=error_envelope("SERVICE_UNAVAILABLE", "busy"))
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.fetch_job_report("job-1")
            self.assertFalse(outcome.ok)
            self.assertEqual(gateway.hits, 1)
            self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")
        finally:
            gateway.close()


class ResponseBudgetTests(unittest.TestCase):
    """A Gateway response is buffered up to 2 MiB and refused beyond it."""

    def test_response_at_the_budget_is_accepted(self) -> None:
        gateway = ServerFixture(body=valid_envelope(MAX_REPORT_BYTES))
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.fetch_job_report("job-1")
            self.assertTrue(outcome.ok, outcome.message)
            self.assertEqual(len(outcome.report["pad"]), envelope_padding(MAX_REPORT_BYTES))
        finally:
            gateway.close()

    def test_response_over_the_budget_is_refused(self) -> None:
        gateway = ServerFixture(body=valid_envelope(MAX_REPORT_BYTES + 1))
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.fetch_job_report("job-1")
            self.assertFalse(outcome.ok)
            self.assertEqual(outcome.code, "RESOURCE_EXHAUSTED")
        finally:
            gateway.close()


class RequestBudgetTests(unittest.TestCase):
    """An oversized outbound body is refused before a socket is opened."""

    def test_oversized_request_body_never_reaches_the_gateway(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.context_query("proj-1", {"blob": "x" * (1024 * 1024)})
            self.assertFalse(outcome.ok)
            self.assertEqual(outcome.code, "RESOURCE_EXHAUSTED")
            self.assertEqual(gateway.hits, 0)
        finally:
            gateway.close()


class RouteAndHeaderInjectionTests(unittest.TestCase):
    """Neither the route nor a caller header value may smuggle a second line."""

    def test_route_with_a_trailing_newline_is_refused(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            for path in (
                "/api/v1/jobs/job-1/report\n",
                "/api/v1/jobs/job-1/report\r\nX-Injected: 1",
                "/api/v1/../../admin",
                "/api/v1/jobs/job 1/report",
                "/other/v1/jobs",
            ):
                with self.subTest(path=path):
                    outcome = client.request("GET", path)
                    self.assertFalse(outcome.ok)
                    self.assertEqual(outcome.code, "INVALID_ARGUMENT")
            self.assertEqual(gateway.hits, 0)
        finally:
            gateway.close()

    def test_idempotency_key_may_not_inject_a_header(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            for key in ("key-1\r\nX-Injected: 1", "key 1", "", "x" * 201):
                with self.subTest(key=key):
                    outcome = client.submit("proj-1", {"snapshot_id": "snap-1"}, key)
                    self.assertFalse(outcome.ok)
                    self.assertEqual(outcome.code, "INVALID_ARGUMENT")
            self.assertEqual(gateway.hits, 0)
        finally:
            gateway.close()


class ErrorMappingTests(unittest.TestCase):
    """Frozen domain codes survive; anything else is GATEWAY_UNAVAILABLE."""

    def test_preserved_codes_are_a_subset_of_the_frozen_vocabulary(self) -> None:
        """A typo here would silently downgrade a code to GATEWAY_UNAVAILABLE."""
        self.assertTrue(_DOMAIN_ERROR_CODES <= set(ERROR_CODES))

    def test_bare_404_is_not_found(self) -> None:
        gateway = ServerFixture(status_code=404, body=b"")
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.fetch_job_report("job-absent")
            self.assertFalse(outcome.ok)
            self.assertEqual(outcome.code, "NOT_FOUND")
        finally:
            gateway.close()

    def test_frozen_codes_are_preserved_verbatim(self) -> None:
        """Domain codes carry a decision for the UI, so they survive."""
        cases = [
            (403, "POLICY_DENIED"),
            (409, "STALE_REVISION"),
            (409, "IDEMPOTENCY_CONFLICT"),
            (410, "CURSOR_EXPIRED"),
            (413, "RESOURCE_EXHAUSTED"),
            (422, "CAPABILITY_UNAVAILABLE"),
        ]
        for status, code in cases:
            with self.subTest(code=code):
                gateway = ServerFixture(status_code=status, body=error_envelope(code, "detail"))
                try:
                    client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
                    outcome = client.fetch_job_report("job-1")
                    self.assertFalse(outcome.ok)
                    self.assertEqual(outcome.code, code)
                finally:
                    gateway.close()

    def test_availability_failures_become_gateway_unavailable(self) -> None:
        """Our channel failed: the UI is told the Gateway could not serve it."""
        cases = [
            (503, "SERVICE_UNAVAILABLE"),
            (503, "GATEWAY_UNAVAILABLE"),
            (401, "UNAUTHENTICATED"),
            (500, "MODEL_UNAVAILABLE"),
            (400, "INVALID_ARGUMENT"),
        ]
        for status, code in cases:
            with self.subTest(code=code):
                gateway = ServerFixture(status_code=status, body=error_envelope(code, "detail"))
                try:
                    client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
                    outcome = client.fetch_job_report("job-1")
                    self.assertFalse(outcome.ok)
                    self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")
                finally:
                    gateway.close()

    def test_code_outside_the_frozen_set_becomes_gateway_unavailable(self) -> None:
        # PATH_OUTSIDE_PROJECT belongs to another interface (接口 B 早期草案) and
        # must never surface through the Agent's own vocabulary.
        for reported in ("PATH_OUTSIDE_PROJECT", "SOURCE_UNAVAILABLE", "TOTALLY_MADE_UP", ""):
            with self.subTest(reported=reported):
                gateway = ServerFixture(
                    status_code=500, body=error_envelope(reported, "should not surface")
                )
                try:
                    client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
                    outcome = client.fetch_job_report("job-1")
                    self.assertFalse(outcome.ok)
                    self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")
                finally:
                    gateway.close()

    def test_non_json_error_body_becomes_gateway_unavailable(self) -> None:
        gateway = ServerFixture(status_code=502, body=b"<html>bad gateway</html>")
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            outcome = client.fetch_job_report("job-1")
            self.assertFalse(outcome.ok)
            self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")
        finally:
            gateway.close()


class UiOnlyRouteTests(unittest.TestCase):
    """The Agent identity never issues or revokes credentials."""

    UI_ONLY = [
        ("POST", "/api/v1/projects/proj-1/grants"),
        ("POST", "/api/v1/grants/grant-1/revoke"),
        ("POST", "/api/v1/projects/proj-1/ui-receipts"),
    ]

    def test_ui_only_routes_are_refused_before_dialing(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            for method, path in self.UI_ONLY:
                with self.subTest(path=path):
                    outcome = client.request(method, path, {})
                    self.assertFalse(outcome.ok)
                    self.assertEqual(outcome.code, "POLICY_DENIED")
            self.assertEqual(gateway.hits, 0, "the Gateway must never see these calls")
        finally:
            gateway.close()

    def test_reading_a_grant_and_consuming_a_receipt_are_still_allowed(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            self.assertTrue(client.grant("grant-1").ok)
            self.assertTrue(client.consume_ui_receipt("ui-receipt-1").ok)
            self.assertEqual(
                [entry["path"] for entry in gateway.seen],
                ["/api/v1/grants/grant-1", "/api/v1/ui-receipts/ui-receipt-1/consume"],
            )
        finally:
            gateway.close()


class RouteCoverageTests(unittest.TestCase):
    """Every helper this client ships must actually be dialable.

    The route filter is deliberately closed, so a helper that builds a path the
    filter rejects would fail silently in production (the caller would just see
    ``INVALID_ARGUMENT``).  This test walks the whole shipped surface.
    """

    def test_every_helper_reaches_the_gateway(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            calls = [
                ("fetch_job_report", lambda: client.fetch_job_report("job-1")),
                ("project(context)", lambda: client.project("proj-1")),
                ("project(state)", lambda: client.project("proj-1", "state")),
                ("project(jobs)", lambda: client.project("proj-1", "jobs")),
                ("project(legacy/jobs)", lambda: client.project("proj-1", "legacy/jobs")),
                ("capabilities", lambda: client.capabilities()),
                ("source", lambda: client.source("proj-1", "src-1", 1, 2)),
                ("design_node", lambda: client.design_node("proj-1", "node-1")),
                (
                    "wave_query",
                    lambda: client.wave_query("art-1", {"start_tick": "0", "end_tick": "10"}),
                ),
                ("wave_signals", lambda: client.wave_signals("art-1")),
                ("snapshot", lambda: client.snapshot("proj-1", "rev-1")),
                ("context_query", lambda: client.context_query("proj-1", {"max_bytes": "1024"})),
                ("job(read)", lambda: client.job("job-1")),
                ("job(cancel)", lambda: client.job("job-1", cancel=True)),
                ("submit", lambda: client.submit("proj-1", {"snapshot_id": "snap-1"}, "key-1")),
                ("grant", lambda: client.grant("grant-1")),
                ("consume_ui_receipt", lambda: client.consume_ui_receipt("ui-receipt-1")),
            ]
            for name, call in calls:
                with self.subTest(helper=name):
                    outcome = call()
                    self.assertTrue(outcome.ok, "%s was refused: %s" % (name, outcome.message))
            self.assertEqual(gateway.hits, len(calls))
        finally:
            gateway.close()


class OpaqueIdentifierTests(unittest.TestCase):
    """Identifiers are opaque: no separators, no traversal, bounded length."""

    def test_invalid_job_ids_are_refused_without_dialing(self) -> None:
        gateway = ServerFixture()
        invalid = [
            "",
            "job/1",
            "job\\1",
            "job?1",
            "job#1",
            "job 1",
            "任务-1",
            "../../etc/passwd",
            "x" * 257,
        ]
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            for job_id in invalid:
                with self.subTest(job_id=job_id):
                    outcome = client.fetch_job_report(job_id)
                    self.assertFalse(outcome.ok)
                    self.assertEqual(outcome.code, "INVALID_ARGUMENT")
            self.assertEqual(gateway.hits, 0)
        finally:
            gateway.close()

    def test_valid_job_ids_are_accepted(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, "tok-test")
            for job_id in ("job-1", "JOB_1", "x" * 256):
                with self.subTest(job_id=job_id):
                    self.assertTrue(client.fetch_job_report(job_id).ok)
            self.assertEqual(gateway.hits, 3)
        finally:
            gateway.close()

    def test_identifier_helper_rejects_the_same_shapes(self) -> None:
        for value in ("", "a/b", "a\\b", "a b", "x" * 257):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    GatewayClient.identifier(value)
        self.assertEqual(GatewayClient.identifier("job-1"), "job-1")


class TokenHygieneTests(unittest.TestCase):
    """The bearer token travels only in the Authorization header."""

    TOKEN = "tok-ui-xyz"

    def test_token_is_in_the_header_and_never_in_the_url(self) -> None:
        gateway = ServerFixture()
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, self.TOKEN)
            self.assertTrue(client.fetch_job_report("job-1").ok)
            request = gateway.seen[0]
            self.assertEqual(request["authorization"], "Bearer " + self.TOKEN)
            self.assertNotIn(self.TOKEN, request["path"])
            self.assertNotIn(self.TOKEN, client.base_url)
        finally:
            gateway.close()

    def test_gateway_error_that_echoes_the_token_is_redacted(self) -> None:
        gateway = ServerFixture(
            status_code=403,
            body=error_envelope(
                "POLICY_DENIED",
                "token %s rejected; see C:\\Users\\student\\top.v" % self.TOKEN,
            ),
        )
        try:
            client = GatewayClient("http://127.0.0.1:%d" % gateway.port, self.TOKEN)
            outcome = client.fetch_job_report("job-1")
            self.assertFalse(outcome.ok)
            self.assertNotIn(self.TOKEN, outcome.message)
            self.assertNotIn("C:\\Users", outcome.message)
        finally:
            gateway.close()

    def test_unreachable_gateway_log_does_not_leak_the_token(self) -> None:
        client = GatewayClient("http://127.0.0.1:1", self.TOKEN)
        with mock.patch(
            "urllib.request.OpenerDirector.open",
            side_effect=urllib.error.URLError("refused while sending %s" % self.TOKEN),
        ):
            with self.assertLogs("sigflow_edu_agent.gateway_client", level="WARNING") as logs:
                outcome = client.fetch_job_report("job-1")
        self.assertFalse(outcome.ok)
        self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")
        self.assertNotIn(self.TOKEN, "\n".join(logs.output))


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
