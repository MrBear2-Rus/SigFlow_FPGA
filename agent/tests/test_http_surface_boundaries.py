"""Acceptance matrix for the frozen HTTP surface.

Every documented route has a positive case, and every non-health route is
checked against missing/wrong credentials, a non-loopback Host and a
non-loopback Origin.  The same file covers the versioned-path and payload-size
boundaries without duplicating the teaching behaviour covered elsewhere.
"""

from __future__ import annotations

import json
import os
import sys
import unittest

AGENT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(AGENT_DIR, "src")
for path in (AGENT_DIR, SRC_DIR):
    if path not in sys.path:
        sys.path.insert(0, path)

from sigflow_edu_agent.http_server import (  # noqa: E402
    EduAgentState,
    LOOPBACK_BIND_ADDRESS,
    ServerConfig,
    create_server,
)
from sigflow_edu_agent.version import MAX_CARD_BYTES  # noqa: E402
from tests.test_http_api import (  # noqa: E402
    UI_TOKEN,
    SidecarFixture,
    _error_code,
    _raw_request,
    _selection,
)


class HttpRouteMatrixTests(unittest.TestCase):
    """Normal routing and the security boundary applied to every route."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = SidecarFixture()
        cls.session_id = cls.fixture.create_session()
        response = cls.fixture.run(
            cls.session_id,
            {"kind": "explain", "selection": _selection()},
        )
        if response.status != 201 or response.payload is None:
            raise AssertionError("could not create a run for the HTTP matrix")
        cls.run_id = str(response.payload["data"]["run_id"])

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.close()

    def _protected_routes(self):
        """Yield ``(method, path, body)`` for every non-health route."""
        yield "GET", "/api/v1/capabilities", None
        yield "POST", "/api/v1/sessions", {
            "project_id": "prj-matrix",
            "revision": "rev-matrix",
        }
        yield "GET", "/api/v1/sessions/%s" % self.session_id, None
        yield "POST", "/api/v1/sessions/%s/runs" % self.session_id, {
            "kind": "explain",
            "selection": _selection(),
        }
        yield "GET", "/api/v1/sessions/%s/events" % self.session_id, None
        yield "GET", "/api/v1/runs/%s" % self.run_id, None
        yield "POST", "/api/v1/shutdown", None

    def test_all_documented_routes_have_a_positive_case(self) -> None:
        health = self.fixture.request("GET", "/api/v1/health", token=None)
        capabilities = self.fixture.request("GET", "/api/v1/capabilities")
        session = self.fixture.request(
            "POST",
            "/api/v1/sessions",
            {"project_id": "prj-positive", "revision": "rev-positive"},
        )
        session_read = self.fixture.request(
            "GET", "/api/v1/sessions/%s" % self.session_id
        )
        run = self.fixture.run(
            self.session_id,
            {"kind": "explain", "selection": _selection()},
        )
        events = self.fixture.request(
            "GET", "/api/v1/sessions/%s/events" % self.session_id
        )
        run_read = self.fixture.request("GET", "/api/v1/runs/%s" % self.run_id)

        self.assertEqual(
            [
                health.status,
                capabilities.status,
                session.status,
                session_read.status,
                run.status,
                events.status,
                run_read.status,
            ],
            [200, 200, 201, 200, 201, 200, 200],
        )

    def test_shutdown_has_positive_and_negative_cases(self) -> None:
        fixture = SidecarFixture()
        try:
            rejected = fixture.request("POST", "/api/v1/shutdown", token=None)
            self.assertEqual(rejected.status, 401)
            self.assertEqual(_error_code(rejected), "UNAUTHENTICATED")

            accepted = fixture.request("POST", "/api/v1/shutdown")
            self.assertEqual(accepted.status, 202)
            fixture.thread.join(timeout=3)
            self.assertFalse(fixture.thread.is_alive())
        finally:
            fixture.close()

    def test_missing_token_is_rejected_on_every_non_health_route(self) -> None:
        for method, path, body in self._protected_routes():
            with self.subTest(method=method, path=path):
                response = self.fixture.request(
                    method, path, payload=body, token=None
                )
                self.assertEqual(response.status, 401)
                self.assertEqual(_error_code(response), "UNAUTHENTICATED")

    def test_wrong_token_is_rejected_on_every_non_health_route(self) -> None:
        for method, path, body in self._protected_routes():
            with self.subTest(method=method, path=path):
                response = self.fixture.request(
                    method, path, payload=body, token="wrong-ui-token"
                )
                self.assertEqual(response.status, 401)
                self.assertEqual(_error_code(response), "UNAUTHENTICATED")

    def test_health_accepts_absent_or_non_matching_credentials(self) -> None:
        absent = self.fixture.request("GET", "/api/v1/health", token=None)
        wrong = self.fixture.request(
            "GET", "/api/v1/health", token="not-the-ui-token"
        )
        self.assertEqual(absent.status, 200)
        self.assertEqual(wrong.status, 200)

    def test_non_loopback_host_is_rejected_on_every_route(self) -> None:
        routes = list(self._protected_routes())
        routes.insert(0, ("GET", "/api/v1/health", None))
        for method, path, body in routes:
            with self.subTest(method=method, path=path):
                response = _raw_request(
                    self.fixture.port,
                    method,
                    path,
                    "evil.example.com",
                    body=(
                        json.dumps(body).encode("utf-8")
                        if body is not None
                        else b""
                    ),
                    token=UI_TOKEN,
                )
                self.assertEqual(response.status, 403)
                self.assertEqual(
                    str(response.payload["error"]["code"]), "POLICY_DENIED"
                )

    def test_non_loopback_origin_is_rejected_on_every_route(self) -> None:
        routes = list(self._protected_routes())
        routes.insert(0, ("GET", "/api/v1/health", None))
        for method, path, body in routes:
            with self.subTest(method=method, path=path):
                response = self.fixture.request(
                    method,
                    path,
                    payload=body,
                    headers={"Origin": "https://evil.example"},
                )
                self.assertEqual(response.status, 403)
                self.assertEqual(_error_code(response), "POLICY_DENIED")

    def test_host_and_loopback_origin_names_are_case_insensitive(self) -> None:
        host = _raw_request(
            self.fixture.port,
            "GET",
            "/api/v1/health",
            "LOCALHOST:%d" % self.fixture.port,
            token=None,
        )
        origin = self.fixture.request(
            "GET",
            "/api/v1/capabilities",
            headers={"Origin": "http://LOCALHOST:19387"},
        )
        self.assertEqual(host.status, 200)
        self.assertEqual(origin.status, 200)


class HttpPathAndLimitTests(unittest.TestCase):
    """Versioned routing and the two request-size limits."""

    def setUp(self) -> None:
        self.fixture = SidecarFixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def test_unknown_or_unversioned_paths_are_404(self) -> None:
        for path in (
            "/health",
            "/capabilities",
            "/api/v2/health",
            "/api/v1/not-a-route",
        ):
            with self.subTest(path=path):
                response = self.fixture.request("GET", path)
                self.assertEqual(response.status, 404)
                self.assertEqual(_error_code(response), "NOT_FOUND")

    def test_request_body_one_byte_over_one_mib_is_413(self) -> None:
        response = self.fixture.request(
            "POST",
            "/api/v1/sessions",
            raw_body=b"x" * (1024 * 1024 + 1),
        )
        self.assertEqual(response.status, 413)
        self.assertEqual(_error_code(response), "RESOURCE_EXHAUSTED")

    def test_selection_text_accepts_64_kib_and_rejects_one_more_byte(self) -> None:
        session_id = self.fixture.create_session()
        accepted = self.fixture.run(
            session_id,
            {"kind": "explain", "selection": {"text": "x" * MAX_CARD_BYTES}},
        )
        rejected = self.fixture.run(
            session_id,
            {"kind": "explain", "selection": {"text": "x" * (MAX_CARD_BYTES + 1)}},
        )
        self.assertEqual(accepted.status, 201)
        self.assertEqual(rejected.status, 400)
        self.assertEqual(_error_code(rejected), "INVALID_ARGUMENT")

    def test_invalid_unicode_selection_is_400_not_500(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.request(
            "POST",
            "/api/v1/sessions/%s/runs" % session_id,
            raw_body=b'{"kind":"explain","selection":{"text":"\\ud800"}}',
        )
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    def test_server_refuses_a_non_loopback_bind_configuration(self) -> None:
        state = EduAgentState(
            ServerConfig(
                instance_id="inst-bind-test",
                ui_token=UI_TOKEN,
                host="0.0.0.0",
            )
        )
        with self.assertRaises(ValueError):
            create_server(state)

    def test_bound_socket_is_the_ipv4_loopback_address(self) -> None:
        self.assertEqual(
            self.fixture.httpd.server_address[0],
            LOOPBACK_BIND_ADDRESS,
        )


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
