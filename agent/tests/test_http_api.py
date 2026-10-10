"""End-to-end HTTP tests for the sidecar's frozen ``/api/v1`` surface.

The tests drive the real server and a stub EDA Gateway over a real loopback
socket, using only the standard library.  They cover the NG-07 acceptance
behaviours: no-model rule cards, real report teaching cards, degradation,
hardening caps and the "no absolute path / no token" invariant.
"""

from __future__ import annotations

import json
import os
import socket
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Mapping, Optional
from urllib.request import Request, urlopen

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from sigflow_edu_agent.__main__ import serve_until_stopped  # noqa: E402
from sigflow_edu_agent.gateway_client import GatewayClient  # noqa: E402
from sigflow_edu_agent.http_server import (  # noqa: E402
    EduAgentState,
    ServerConfig,
    create_server,
)
from sigflow_edu_agent.model_client import ModelClient, ModelStatus  # noqa: E402
from sigflow_edu_agent.version import MAX_CARD_BYTES, MAX_SESSIONS, PROTOCOL  # noqa: E402

STUB_GATEWAY_TOKEN = "tok-edge-test"
UI_TOKEN = "tok-ui-test"
INSTANCE_ID = "inst-test-01"
DATA_DIRECTORY = "C:\\Users\\tester\\AppData\\Local\\SigFlow\\agent-data"
GATEWAY_HOST = "127.0.0.1"

LATCH_SNIPPET = (
    "module blk(input clk, input en, input d, output reg q);\n"
    "  always @(*) begin\n"
    "    if (en)\n"
    "      q = d;\n"
    "  end\n"
    "endmodule\n"
)

FORBIDDEN_FRAGMENTS = (
    "C:\\",
    "C:/",
    "/home/",
    "/tmp/",
    DATA_DIRECTORY,
    STUB_GATEWAY_TOKEN,
    UI_TOKEN,
)

REPORT_WITH_DIAGNOSTIC: dict[str, Any] = {
    "schema_version": "edu.jobreport.v1",
    "job_id": "job-1",
    "origin": "core",
    "capability": "eda.synth",
    "plugin_id": "eda-synth-yosys",
    "plugin_version": "1.0.0",
    "project_id": "prj-1",
    "revision": "rev-12",
    "snapshot_id": "snap-12",
    "state": "Succeeded",
    "exit_code": 0,
    "diagnostics": [
        {
            "id": "diag-1",
            "code": "EDU_LATCH_INFERRED",
            "raw_code": "LATCH",
            "severity": "warning",
            "stage": "synth",
            "summary": "Latch inferred for signal 'q' (missing assignment in some branch).",
            "origin": "core",
            "confidence_kind": "tool",
            "location": {"file": "rtl/top.v", "start_line": 7, "end_line": 7},
            "evidence_refs": ["ev-1"],
        }
    ],
    "metrics": {},
    "artifacts": [],
    "completeness": "complete",
    "input_fingerprint": "sha256:abc",
    "raw_report_schema": "eda.jobreport.v1",
}

REPORT_EMPTY_TRUSTED: dict[str, Any] = {
    "schema_version": "edu.jobreport.v1",
    "job_id": "job-empty",
    "origin": "core",
    "capability": "eda.synth",
    "project_id": "prj-1",
    "revision": "rev-12",
    "snapshot_id": "snap-12",
    "state": "Succeeded",
    "diagnostics": [],
    "metrics": {},
    "artifacts": [],
    "completeness": "complete",
    "input_fingerprint": "sha256:def",
}

REPORT_EMPTY_LEGACY: dict[str, Any] = {
    "schema_version": "edu.jobreport.v1",
    "job_id": "job-legacy",
    "origin": "legacy",
    "capability": "eda.synth",
    "project_id": "prj-1",
    "revision": None,
    "state": "Succeeded",
    "diagnostics": [],
    "metrics": {},
    "artifacts": [],
    "completeness": "legacy_unverified",
    "reason": "legacy job has no input fingerprint or revision",
}

REPORTS: dict[str, dict[str, Any]] = {
    "job-1": REPORT_WITH_DIAGNOSTIC,
    "job-empty": REPORT_EMPTY_TRUSTED,
    "job-legacy": REPORT_EMPTY_LEGACY,
}


class _StubGatewayHandler(BaseHTTPRequestHandler):
    """Stub EDA Gateway: serves three reports and refuses everything else."""

    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
        """Silence the stub access log."""

    def do_GET(self) -> None:  # noqa: N802 - http.server API
        """Answer ``/api/v1/jobs/{id}/report`` with the shared envelope."""
        header = self.headers.get("Authorization", "")
        path = self.path.split("?", 1)[0]
        if not path.startswith("/api/v1/jobs/") or not path.endswith("/report"):
            self._send(404, {"schema_version": "edu.api.v1", "error": {"code": "NOT_FOUND"}})
            return
        if header != "Bearer " + STUB_GATEWAY_TOKEN:
            self._send(
                401,
                {
                    "schema_version": "edu.api.v1",
                    "error": {"code": "UNAUTHENTICATED", "message": "missing or invalid bearer token"},
                },
            )
            return
        job_id = path[len("/api/v1/jobs/") : -len("/report")]
        if job_id == "job-down":
            self._send(
                503,
                {
                    "schema_version": "edu.api.v1",
                    "error": {
                        "code": "SERVICE_UNAVAILABLE",
                        "message": "job service is not available",
                    },
                },
            )
            return
        report = REPORTS.get(job_id)
        if report is None:
            self._send(
                404,
                {
                    "schema_version": "edu.api.v1",
                    "error": {"code": "NOT_FOUND", "message": "unknown agent job"},
                },
            )
            return
        self._send(
            200,
            {
                "schema_version": "edu.api.v1",
                "request_id": "req-1",
                "trace_id": "trace-1",
                "data": report,
            },
        )

    def _send(self, status: int, payload: Mapping[str, Any]) -> None:
        """Send one JSON response and close the connection."""
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


class _Response:
    """Minimal HTTP response record used by the raw-socket helpers."""

    def __init__(self, status: int, payload: Optional[Mapping[str, Any]]) -> None:
        self.status = status
        self.payload = payload


def _raw_request(
    port: int,
    method: str,
    path: str,
    host_header: str,
    body: bytes = b"",
    token: Optional[str] = None,
) -> _Response:
    """Perform a raw HTTP request so the Host header can be controlled."""
    lines = [
        "%s %s HTTP/1.1" % (method, path),
        "Host: %s" % host_header,
        "Connection: close",
        "Accept: application/json",
    ]
    if token is not None:
        lines.append("Authorization: Bearer %s" % token)
    if body:
        lines.append("Content-Type: application/json")
        lines.append("Content-Length: %d" % len(body))
    request = ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + body
    with socket.create_connection((GATEWAY_HOST, port), timeout=10) as connection:
        connection.sendall(request)
        chunks: list[bytes] = []
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    raw = b"".join(chunks)
    header, _, payload = raw.partition(b"\r\n\r\n")
    status_line = header.split(b"\r\n", 1)[0].decode("latin-1")
    status = int(status_line.split(" ")[1])
    parsed: Optional[Mapping[str, Any]] = None
    if payload.strip():
        try:
            parsed = json.loads(payload.decode("utf-8"))
        except ValueError:
            parsed = None
    return _Response(status, parsed)


class SidecarFixture:
    """A running sidecar plus a running stub Gateway, both on loopback."""

    def __init__(self, health_fault: Optional[str] = None) -> None:
        self.gateway = ThreadingHTTPServer((GATEWAY_HOST, 0), _StubGatewayHandler)
        self.gateway.daemon_threads = True
        self.gateway_thread = threading.Thread(target=self.gateway.serve_forever, daemon=True)
        self.gateway_thread.start()

        self.stop_event = threading.Event()
        model = ModelClient(
            ModelStatus(
                available=False,
                provider=None,
                reason="未配置模型 Key（SIGFLOW_EDU_MODEL_API_KEY），已降级为确定性规则讲解。",
            )
        )
        self.state = EduAgentState(
            ServerConfig(
                instance_id=INSTANCE_ID,
                ui_token=UI_TOKEN,
                gateway=GatewayClient(
                    "http://%s:%d" % (GATEWAY_HOST, self.gateway.server_address[1]),
                    STUB_GATEWAY_TOKEN,
                    secrets=(STUB_GATEWAY_TOKEN, UI_TOKEN),
                ),
                model=model,
                secrets=(STUB_GATEWAY_TOKEN, UI_TOKEN),
                stop_event=self.stop_event,
                health_fault=health_fault,
            )
        )
        self.httpd = create_server(self.state)
        self.port = int(self.httpd.server_address[1])
        self.thread = threading.Thread(
            target=serve_until_stopped,
            args=(self.httpd, self.stop_event),
            daemon=True,
        )
        self.thread.start()

    def close(self) -> None:
        """Stop the sidecar and the stub Gateway."""
        self.stop_event.set()
        self.thread.join(timeout=5)
        self.state.learning.close()
        self.gateway.shutdown()
        self.gateway.server_close()

    # -- request helpers --------------------------------------------------

    def request(
        self,
        method: str,
        path: str,
        payload: Optional[Mapping[str, Any]] = None,
        token: Optional[str] = UI_TOKEN,
        raw_body: Optional[bytes] = None,
        headers: Optional[Mapping[str, str]] = None,
    ) -> _Response:
        """Send a JSON request to the sidecar."""
        if raw_body is None:
            body = json.dumps(payload).encode("utf-8") if payload is not None else b""
        else:
            body = raw_body
        request = Request(
            "http://%s:%d%s" % (GATEWAY_HOST, self.port, path),
            data=body if body else None,
            method=method,
        )
        if token is not None:
            request.add_header("Authorization", "Bearer " + token)
        for name, value in (headers or {}).items():
            request.add_header(name, value)
        if body:
            request.add_header("Content-Type", "application/json")
        try:
            with urlopen(request, timeout=10) as response:
                return _Response(response.status, json.loads(response.read().decode("utf-8")))
        except Exception as exc:  # urllib raises HTTPError for non-2xx
            status = int(getattr(exc, "code", 0) or 0)
            body_bytes = getattr(exc, "read", lambda: b"")()
            parsed = json.loads(body_bytes.decode("utf-8")) if body_bytes else None
            return _Response(status, parsed)

    def create_session(self, revision: str = "rev-1") -> str:
        """Create a session and return its id."""
        response = self.request(
            "POST",
            "/api/v1/sessions",
            {"project_id": "prj-1", "revision": revision, "top": "top", "target": {"profile": "x"}},
        )
        assert response.status == 201, response.payload
        return str(response.payload["data"]["session_id"])

    def run(self, session_id: str, body: Mapping[str, Any]) -> _Response:
        """Post a run to a session."""
        return self.request("POST", "/api/v1/sessions/%s/runs" % session_id, body)

    def all_cards(self) -> list[Mapping[str, Any]]:
        """Collect every card stored in the fixture's sidecar state."""
        cards: list[Mapping[str, Any]] = []
        with self.state.sessions._lock:  # noqa: SLF001 - test introspection
            sessions = list(self.state.sessions._sessions.values())  # noqa: SLF001
        for session in sessions:
            for run in session.runs.values():
                cards.extend(run.cards)
        return cards


def _selection(text: str = LATCH_SNIPPET) -> dict[str, Any]:
    """Build a selection payload with a source reference."""
    return {
        "node_id": "node-1",
        "source_id": "source-000000000000000000000001",
        "start_line": 1,
        "end_line": 6,
        "text": text,
    }


def _data(response: _Response) -> Mapping[str, Any]:
    """Return the success envelope's ``data`` object."""
    assert response.payload is not None
    return response.payload["data"]


def _error_code(response: _Response) -> str:
    """Return the failure envelope's error code."""
    assert response.payload is not None
    return str(response.payload["error"]["code"])


def _walk_strings(value: Any) -> list[str]:
    """Collect every string contained in a nested JSON value."""
    if isinstance(value, str):
        return [value]
    if isinstance(value, Mapping):
        found: list[str] = []
        for key, item in value.items():
            found.append(str(key))
            found.extend(_walk_strings(item))
        return found
    if isinstance(value, (list, tuple)):
        found = []
        for item in value:
            found.extend(_walk_strings(item))
        return found
    return []


class SidecarApiTests(unittest.TestCase):
    """One sidecar instance is shared by all cases; card exposure is scanned globally."""

    fixture: SidecarFixture

    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = SidecarFixture()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.close()

    # -- health / capabilities -------------------------------------------

    def test_health_requires_no_token_and_reports_protocol(self) -> None:
        response = self.fixture.request("GET", "/api/v1/health", token=None)
        self.assertEqual(response.status, 200)
        self.assertEqual(response.payload["schema_version"], "edu.api.v1")
        data = _data(response)
        self.assertEqual(data["protocol"], "edu.api.v1")
        self.assertEqual(data["instance_id"], INSTANCE_ID)
        self.assertTrue(data["ready"])
        self.assertEqual(data["agent_version"], "0.1.0")
        self.assertFalse(data["model"]["available"])
        self.assertIsNone(data["model"]["provider"])

    def test_health_accepts_an_authorization_header(self) -> None:
        response = self.fixture.request("GET", "/api/v1/health")
        self.assertEqual(response.status, 200)

    def test_health_never_returns_the_data_directory_or_tokens(self) -> None:
        response = self.fixture.request("GET", "/api/v1/health", token=None)
        text = json.dumps(response.payload, ensure_ascii=False)
        self.assertNotIn(DATA_DIRECTORY, text)
        self.assertNotIn(UI_TOKEN, text)
        self.assertNotIn(STUB_GATEWAY_TOKEN, text)

    def test_capabilities_reports_indexing_mismatch_and_no_execution(self) -> None:
        response = self.fixture.request("GET", "/api/v1/capabilities")
        self.assertEqual(response.status, 200)
        data = _data(response)
        self.assertEqual([mode["id"] for mode in data["modes"]], ["L1", "L2", "L3", "L4"])
        self.assertFalse(data["execution"]["can_run_eda"])
        self.assertIn("grant", data["execution"]["note"])
        self.assertFalse(data["model"]["available"])

    def test_capabilities_requires_auth(self) -> None:
        response = self.fixture.request("GET", "/api/v1/capabilities", token=None)
        self.assertEqual(response.status, 401)
        self.assertEqual(_error_code(response), "UNAUTHENTICATED")

    def test_wrong_token_is_rejected(self) -> None:
        response = self.fixture.request("GET", "/api/v1/capabilities", token="tok-ui-wrong")
        self.assertEqual(response.status, 401)
        self.assertEqual(_error_code(response), "UNAUTHENTICATED")

    # -- hardening --------------------------------------------------------

    def test_non_loopback_host_is_denied(self) -> None:
        response = _raw_request(
            self.fixture.port, "GET", "/api/v1/health", "evil.example.com", token=UI_TOKEN
        )
        self.assertEqual(response.status, 403)
        self.assertEqual(str(response.payload["error"]["code"]), "POLICY_DENIED")

    def test_unknown_path_is_404(self) -> None:
        response = self.fixture.request("GET", "/api/v1/nope")
        self.assertEqual(response.status, 404)
        self.assertEqual(_error_code(response), "NOT_FOUND")

    def test_unknown_api_v1_path_is_404(self) -> None:
        response = self.fixture.request("GET", "/api/v1/sessions/x/y/z")
        self.assertEqual(response.status, 404)

    def test_unsupported_method_is_404(self) -> None:
        response = self.fixture.request("PUT", "/api/v1/health", token=None)
        self.assertEqual(response.status, 404)
        self.assertEqual(_error_code(response), "NOT_FOUND")

    def test_oversized_body_is_413(self) -> None:
        response = self.fixture.request(
            "POST",
            "/api/v1/sessions",
            raw_body=b"x" * (1024 * 1024 + 1),
        )
        self.assertEqual(response.status, 413)
        self.assertEqual(_error_code(response), "RESOURCE_EXHAUSTED")

    def test_oversized_selection_text_is_400(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id,
            {
                "kind": "explain",
                "selection": {"text": "x" * (MAX_CARD_BYTES + 1)},
            },
        )
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    def test_non_json_body_is_400(self) -> None:
        response = self.fixture.request("POST", "/api/v1/sessions", raw_body=b"{not json")
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    # -- sessions ---------------------------------------------------------

    def test_create_session_validates_project_and_revision(self) -> None:
        empty_project = self.fixture.request(
            "POST", "/api/v1/sessions", {"project_id": "", "revision": "rev-1"}
        )
        self.assertEqual(empty_project.status, 400)
        self.assertEqual(_error_code(empty_project), "INVALID_ARGUMENT")
        missing_revision = self.fixture.request(
            "POST", "/api/v1/sessions", {"project_id": "prj-1", "revision": ""}
        )
        self.assertEqual(missing_revision.status, 400)

    def test_create_and_get_session(self) -> None:
        session_id = self.fixture.create_session("rev-7")
        response = self.fixture.request("GET", "/api/v1/sessions/%s" % session_id)
        self.assertEqual(response.status, 200)
        data = _data(response)
        self.assertEqual(data["session_id"], session_id)
        self.assertEqual(data["revision"], "rev-7")
        self.assertEqual(data["project_id"], "prj-1")

    def test_unknown_session_is_404(self) -> None:
        response = self.fixture.request("GET", "/api/v1/sessions/sess-does-not-exist")
        self.assertEqual(response.status, 404)
        self.assertEqual(_error_code(response), "NOT_FOUND")

    def test_session_cap_evicts_the_oldest(self) -> None:
        created = [self.fixture.create_session("rev-cap-%d" % index) for index in range(MAX_SESSIONS + 3)]
        self.assertEqual(
            self.fixture.request("GET", "/api/v1/sessions/%s" % created[0]).status, 404
        )
        self.assertEqual(
            self.fixture.request("GET", "/api/v1/sessions/%s" % created[-1]).status, 200
        )

    # -- runs: acceptance (a) and (d) -------------------------------------

    def test_explain_without_model_key_degrades_to_a_rule_card(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id,
            {"kind": "explain", "level": "L1", "selection": _selection()},
        )
        self.assertEqual(response.status, 201)
        data = _data(response)
        self.assertEqual(data["state"], "degraded")
        self.assertFalse(data["model_used"])
        self.assertEqual(data["state_version"], "2")
        card = data["cards"][0]
        self.assertEqual(card["source"], "rule")
        self.assertEqual(card["level"], "L1")
        self.assertEqual(card["kind"], "rule")
        self.assertEqual(card["evidence"][0]["kind"], "rule_match")
        self.assertIn("锁存器", card["body"])
        self.assertFalse(card["expired"])
        self.assertRegex(card["state_version"], r"^[0-9]+$")
        self.assertTrue(
            any(item["kind"] == "model_explanation" for item in data["omitted"])
        )

    def test_explain_kind_rejects_a_full_corrected_module(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id, {"kind": "explain", "selection": _selection()}
        )
        card = _data(response)["cards"][0]
        body = card["body"]
        self.assertNotIn("module blk", body)
        self.assertNotIn("endmodule", body)
        self.assertNotIn("else\n", body)
        self.assertNotIn("q <= d", body)

    def test_explain_on_clean_code_still_refuses_to_conclude(self) -> None:
        session_id = self.fixture.create_session()
        clean = (
            "module blk(input a, output reg y);\n"
            "  always @(*) begin\n"
            "    y = a;\n"
            "  end\n"
            "endmodule"
        )
        response = self.fixture.run(
            session_id, {"kind": "explain", "selection": {"text": clean}}
        )
        card = _data(response)["cards"][0]
        self.assertEqual(card["kind"], "teaching")
        self.assertIsNone(card["issue_id"])
        self.assertIn("不等于这段代码正确", card["body"])

    def test_hint_never_contains_the_fix(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(session_id, {"kind": "hint", "selection": _selection()})
        self.assertEqual(response.status, 201)
        card = _data(response)["cards"][0]
        self.assertNotIn("else", card["body"].replace("无 else", ""))

    def test_unsupported_kind_is_400(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(session_id, {"kind": "solve_it", "selection": _selection()})
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    def test_unsupported_level_is_400(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id, {"kind": "explain", "level": "L9", "selection": _selection()}
        )
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    def test_run_is_readable_by_its_id(self) -> None:
        session_id = self.fixture.create_session()
        created = _data(self.fixture.run(session_id, {"kind": "explain", "selection": _selection()}))
        response = self.fixture.request("GET", "/api/v1/runs/%s" % created["run_id"])
        self.assertEqual(response.status, 200)
        data = _data(response)
        self.assertEqual(data["run_id"], created["run_id"])
        self.assertEqual(data["session_id"], session_id)
        self.assertEqual(data["state"], "degraded")

    def test_unknown_run_is_404(self) -> None:
        response = self.fixture.request("GET", "/api/v1/runs/run-does-not-exist")
        self.assertEqual(response.status, 404)

    def test_card_cap_records_omissions(self) -> None:
        session_id = self.fixture.create_session()
        many = (
            "module blk(input a, input b, input c, input d, input e, output reg y);\n"
            "  always @(*) begin\n"
            "    if (a)\n"
            "      y = b;\n"
            "    else if (c) begin\n"
            "      y = d;\n"
            "      z1 = e;\n"
            "    end\n"
            "  end\n"
            "endmodule"
        )
        response = self.fixture.run(
            session_id, {"kind": "explain", "selection": {"text": many}}
        )
        self.assertLessEqual(len(_data(response)["cards"]), 16)

    # -- acceptance (a): the L4 two-step policy ---------------------------

    def test_l4_without_more_is_policy_denied_and_leaks_no_answer(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id,
            {"kind": "explain", "level": "L4", "selection": _selection()},
        )
        self.assertEqual(response.status, 403)
        self.assertEqual(_error_code(response), "POLICY_DENIED")
        text = json.dumps(response.payload, ensure_ascii=False)
        self.assertIn("两步", str(response.payload["error"]["message"]))
        self.assertNotIn("always @(*)", text)
        self.assertNotIn("module blk", text)
        self.assertNotIn("reference_solution", text)

    def test_l4_with_more_is_not_authorization(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id,
            {"kind": "explain", "level": "L4", "more": True, "selection": _selection()},
        )
        self.assertEqual(response.status, 403)
        self.assertNotIn("reference_solution", json.dumps(response.payload))

    def test_more_must_be_boolean(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id, {"kind": "explain", "more": "true", "selection": _selection()}
        )
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    # -- acceptance (b): real report teaching cards -----------------------

    def test_report_review_maps_a_diagnostic_verbatim(self) -> None:
        session_id = self.fixture.create_session("rev-12")
        response = self.fixture.run(
            session_id,
            {"kind": "report_review", "job_id": "job-1", "selection": _selection()},
        )
        self.assertEqual(response.status, 201)
        data = _data(response)
        self.assertEqual(data["state"], "degraded")
        card = data["cards"][0]
        self.assertEqual(card["kind"], "diagnostic_explanation")
        self.assertIn("EDU_LATCH_INFERRED", card["body"])
        diagnostic_ref = card["evidence"][0]
        self.assertEqual(diagnostic_ref["kind"], "diagnostic")
        self.assertEqual(diagnostic_ref["ref"]["code"], "EDU_LATCH_INFERRED")
        self.assertEqual(diagnostic_ref["ref"]["raw_code"], "LATCH")
        self.assertEqual(diagnostic_ref["ref"]["severity"], "warning")
        self.assertEqual(diagnostic_ref["ref"]["location"]["start_line"], 7)
        report_ref = card["evidence"][1]
        self.assertEqual(report_ref["kind"], "job_report")
        self.assertEqual(report_ref["ref"]["revision"], "rev-12")
        self.assertEqual(report_ref["ref"]["snapshot_id"], "snap-12")
        self.assertEqual(report_ref["ref"]["input_fingerprint"], "sha256:abc")
        self.assertEqual(report_ref["ref"]["completeness"], "complete")
        self.assertEqual(report_ref["ref"]["origin"], "core")

    def test_report_review_does_not_invent_a_root_cause(self) -> None:
        session_id = self.fixture.create_session("rev-12")
        response = self.fixture.run(
            session_id,
            {"kind": "report_review", "job_id": "job-1", "selection": _selection()},
        )
        card = _data(response)["cards"][0]
        self.assertIn("不会替你推断", card["body"])
        self.assertNotIn("因为你忘记", card["body"])

    def test_report_review_requires_job_id(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id, {"kind": "report_review", "selection": _selection()}
        )
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    def test_empty_diagnostics_on_a_trusted_report_is_explained(self) -> None:
        session_id = self.fixture.create_session("rev-12")
        response = self.fixture.run(
            session_id,
            {"kind": "report_review", "job_id": "job-empty", "selection": _selection()},
        )
        self.assertEqual(response.status, 201)
        card = _data(response)["cards"][0]
        self.assertIn("没有报告任何诊断", card["body"])
        self.assertIn("不能据此判断电路正确", card["body"])

    def test_empty_diagnostics_on_a_legacy_report_refuses_to_conclude(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id,
            {"kind": "report_review", "job_id": "job-legacy", "selection": _selection()},
        )
        self.assertEqual(response.status, 409)
        self.assertEqual(_error_code(response), "STALE_REVISION")

    # -- acceptance (c): degradation --------------------------------------

    def test_report_review_without_a_gateway_fails_without_fake_cards(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id,
            {"kind": "report_review", "job_id": "job-down", "selection": _selection()},
        )
        self.assertEqual(response.status, 503)
        self.assertEqual(_error_code(response), "GATEWAY_UNAVAILABLE")
        self.assertNotIn("cards", response.payload)

    def test_report_review_for_an_unknown_job_is_404(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(
            session_id,
            {"kind": "report_review", "job_id": "job-not-here", "selection": _selection()},
        )
        self.assertEqual(response.status, 404)
        self.assertEqual(_error_code(response), "NOT_FOUND")

    def test_gateway_client_maps_a_missing_job_to_not_found(self) -> None:
        client = GatewayClient(
            "http://%s:%d" % (GATEWAY_HOST, self.fixture.gateway.server_address[1]),
            STUB_GATEWAY_TOKEN,
        )
        outcome = client.fetch_job_report("absent")
        self.assertFalse(outcome.ok)
        self.assertEqual(outcome.code, "NOT_FOUND")

    def test_gateway_client_refuses_a_non_opaque_job_id(self) -> None:
        client = GatewayClient("http://127.0.0.1:1", STUB_GATEWAY_TOKEN)
        outcome = client.fetch_job_report("../../etc/passwd")
        self.assertFalse(outcome.ok)
        self.assertEqual(outcome.code, "INVALID_ARGUMENT")

    def test_gateway_client_reports_an_unreachable_gateway(self) -> None:
        client = GatewayClient("http://127.0.0.1:1", STUB_GATEWAY_TOKEN, timeout=0.5)
        outcome = client.fetch_job_report("job-1")
        self.assertFalse(outcome.ok)
        self.assertEqual(outcome.code, "GATEWAY_UNAVAILABLE")

    # -- events -----------------------------------------------------------

    def test_events_are_returned_with_cursors(self) -> None:
        session_id = self.fixture.create_session()
        self.fixture.run(session_id, {"kind": "explain", "selection": _selection()})
        response = self.fixture.request("GET", "/api/v1/sessions/%s/events" % session_id)
        self.assertEqual(response.status, 200)
        data = _data(response)
        self.assertTrue(data["events"])
        first = data["events"][0]
        self.assertEqual(first["type"], "session/created")
        self.assertEqual(first["sequence"], "1")
        self.assertRegex(first["event_id"], r"^ev-\d+$")
        self.assertIn("high_watermark", data)
        self.assertEqual(data["oldest_sequence"], "1")

    def test_expired_cursor_is_410(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.request(
            "GET", "/api/v1/sessions/%s/events?after=999999" % session_id
        )
        self.assertEqual(response.status, 410)
        self.assertEqual(_error_code(response), "CURSOR_EXPIRED")

    def test_invalid_cursor_is_400(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.request(
            "GET", "/api/v1/sessions/%s/events?after=abc" % session_id
        )
        self.assertEqual(response.status, 400)
        self.assertEqual(_error_code(response), "INVALID_ARGUMENT")

    # -- acceptance (e): no absolute paths, no tokens ---------------------

    def test_no_card_ever_leaks_a_path_or_a_token(self) -> None:
        cards = self.fixture.all_cards()
        self.assertTrue(cards, "the suite should have produced cards to scan")
        for card in cards:
            for text in _walk_strings(card):
                for fragment in FORBIDDEN_FRAGMENTS:
                    self.assertNotIn(
                        fragment,
                        text,
                        "card %s leaked %r" % (card.get("card_id"), fragment),
                    )

    def test_every_card_matches_the_frozen_shape(self) -> None:
        required = {
            "card_id",
            "kind",
            "level",
            "title",
            "body",
            "issue_id",
            "source",
            "evidence",
            "course_refs",
            "limitations",
            "expired",
            "state_version",
        }
        for card in self.fixture.all_cards():
            self.assertEqual(required, set(card.keys()))
            self.assertIn(card["kind"], ("rule", "teaching", "reference_solution", "diagnostic_explanation"))
            self.assertIn(card["level"], ("L1", "L2", "L3", "L4"))
            self.assertIn(card["source"], ("rule", "model"))
            self.assertFalse(card["expired"])
            self.assertRegex(card["state_version"], r"^[0-9]+$")

    def test_plan_card_shape_and_capabilities(self) -> None:
        session_id = self.fixture.create_session()
        response = self.fixture.run(session_id, {"kind": "plan", "selection": _selection()})
        self.assertEqual(response.status, 201)
        card = _data(response)["cards"][0]
        self.assertEqual(card["plan_id"], "plan-1")
        self.assertIn(card["revision"], ("rev-1",))
        self.assertTrue(card["snapshot_required"])
        self.assertTrue(card["requires_approval"])
        self.assertEqual(
            [step["capability"] for step in card["steps"]],
            ["eda.synth", "eda.sim.build", "eda.sim.run"],
        )
        for text in _walk_strings(card):
            for fragment in FORBIDDEN_FRAGMENTS:
                self.assertNotIn(fragment, text)
        for step in card["steps"]:
            self.assertNotIn("path", step["params"])


class ShutdownTests(unittest.TestCase):
    """``POST /api/v1/shutdown`` acknowledges then stops within a few seconds."""

    def test_shutdown_stops_the_loop(self) -> None:
        fixture = SidecarFixture()
        try:
            response = fixture.request("POST", "/api/v1/shutdown")
            self.assertEqual(response.status, 202)
            self.assertEqual(_data(response)["state"], "shutting_down")
            fixture.thread.join(timeout=3)
            self.assertFalse(fixture.thread.is_alive())
        finally:
            fixture.close()

    def test_shutdown_requires_auth(self) -> None:
        fixture = SidecarFixture()
        try:
            response = fixture.request("POST", "/api/v1/shutdown", token=None)
            self.assertEqual(response.status, 401)
        finally:
            fixture.close()


class HealthFaultTests(unittest.TestCase):
    """TEST-ONLY health fault modes used to verify the host's readiness gating (AD-02).

    A sidecar may write a perfectly valid ``ready`` line and still be unusable; the host
    must not announce it as ready before a real health check passes. These modes make
    that condition reproducible.
    """

    def test_bad_health_reports_503(self) -> None:
        fixture = SidecarFixture(health_fault="bad-health")
        try:
            response = fixture.request("GET", "/api/v1/health", token=None)
            self.assertEqual(response.status, 503)
            self.assertEqual(response.payload["error"]["code"], "SERVICE_UNAVAILABLE")
        finally:
            fixture.close()

    def test_bad_health_protocol_reports_a_wrong_protocol(self) -> None:
        fixture = SidecarFixture(health_fault="bad-health-protocol")
        try:
            response = fixture.request("GET", "/api/v1/health", token=None)
            self.assertEqual(response.status, 200)
            self.assertNotEqual(_data(response)["protocol"], PROTOCOL)
        finally:
            fixture.close()

    def test_default_health_stays_healthy(self) -> None:
        fixture = SidecarFixture()
        try:
            response = fixture.request("GET", "/api/v1/health", token=None)
            self.assertEqual(response.status, 200)
            self.assertEqual(_data(response)["protocol"], PROTOCOL)
        finally:
            fixture.close()


class OriginBoundaryTests(unittest.TestCase):
    """AD-04: a non-loopback ``Origin`` must not be able to drive the service."""

    def test_non_loopback_origin_is_denied(self) -> None:
        fixture = SidecarFixture()
        try:
            for origin in ("http://evil.example", "https://evil.example:443", "null"):
                with self.subTest(origin=origin):
                    response = fixture.request(
                        "GET", "/api/v1/capabilities", headers={"Origin": origin}
                    )
                    self.assertEqual(response.status, 403)
                    self.assertEqual(response.payload["error"]["code"], "POLICY_DENIED")
        finally:
            fixture.close()

    def test_loopback_origin_is_allowed(self) -> None:
        fixture = SidecarFixture()
        try:
            response = fixture.request(
                "GET", "/api/v1/capabilities", headers={"Origin": "http://127.0.0.1:19387"}
            )
            self.assertEqual(response.status, 200)
        finally:
            fixture.close()

    def test_absent_origin_is_allowed_for_native_clients(self) -> None:
        fixture = SidecarFixture()
        try:
            response = fixture.request("GET", "/api/v1/capabilities")
            self.assertEqual(response.status, 200)
        finally:
            fixture.close()


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
