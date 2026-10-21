"""Boundary and stress matrix for the sidecar runtime.

The regular suites cover the happy paths and most caps.  This file fills the
release-matrix gaps that need a near-real boundary: exact request size, the
response budget, counts above 2^53, Unicode/long identifiers, two projects in
parallel, a large diagnostic window, and simultaneous model/Gateway outages.
"""

from __future__ import annotations

import io
import json
import os
import sys
import threading
import unittest
from types import SimpleNamespace
from typing import Any
from unittest import mock

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.dirname(TESTS_DIR)
SRC_DIR = os.path.join(AGENT_DIR, "src")
for path in (AGENT_DIR, SRC_DIR):
    if path not in sys.path:
        sys.path.insert(0, path)

from sigflow_edu_agent.errors import success_envelope  # noqa: E402
from sigflow_edu_agent.http_server import (  # noqa: E402
    EduAgentRequestHandler,
    MAX_BODY_BYTES,
    MAX_RESPONSE_BYTES,
)
from sigflow_edu_agent.model_client import (  # noqa: E402
    API_KEY_ENV,
    BASE_URL_ENV,
    MODEL_ENV,
    ModelClient,
    ModelStatus,
)
from sigflow_edu_agent.session import SessionStore  # noqa: E402
from tests.test_http_api import (  # noqa: E402
    SidecarFixture,
    _selection,
)
from tests.test_run_semantics import (  # noqa: E402
    _FakeGateway,
    _dispatcher,
    _report,
    _request,
)


class _FakeState:
    """Minimal state used to exercise the response writer directly."""

    config = SimpleNamespace(secrets=())

    def next_ids(self) -> tuple[str, str]:
        return "req-1", "trace-1"


class _FakeWriter(EduAgentRequestHandler):
    """Capture a response without opening a socket."""

    def __init__(self) -> None:
        self.server = SimpleNamespace(state=_FakeState())
        self.wfile = io.BytesIO()
        self.response_status = 0

    def send_response(self, code: int, message: Any = None) -> None:
        self.response_status = int(code)

    def send_header(self, keyword: str, value: str) -> None:
        return None

    def end_headers(self) -> None:
        return None


class BodyAndResponseBoundaryTests(unittest.TestCase):
    """1 MiB request and 2 MiB response boundaries."""

    def test_request_body_of_exactly_one_mib_is_accepted(self) -> None:
        fixture = SidecarFixture()
        try:
            base = {"project_id": "prj-1", "revision": "rev-1", "padding": ""}
            base_size = len(json.dumps(base, separators=(",", ":")).encode("utf-8"))
            padding = MAX_BODY_BYTES - base_size
            body = json.dumps(
                {
                    "project_id": "prj-1",
                    "revision": "rev-1",
                    "padding": "x" * padding,
                },
                separators=(",", ":"),
            ).encode("utf-8")
            self.assertEqual(len(body), MAX_BODY_BYTES)
            response = fixture.request(
                "POST",
                "/api/v1/sessions",
                raw_body=body,
            )
            self.assertEqual(response.status, 201, response.payload)
        finally:
            fixture.close()

    def test_response_over_two_mib_is_replaced_by_429(self) -> None:
        self.assertEqual(MAX_RESPONSE_BYTES, 2 * 1024 * 1024)
        writer = _FakeWriter()
        payload = success_envelope(
            "req-1",
            "trace-1",
            {"blob": "x" * (MAX_RESPONSE_BYTES + 1)},
        )
        writer._send_json(200, payload)
        self.assertEqual(writer.response_status, 429)
        decoded = json.loads(writer.wfile.getvalue().decode("utf-8"))
        self.assertEqual(decoded["error"]["code"], "RESOURCE_EXHAUSTED")
        self.assertNotIn("blob", decoded)


class CapacityAndIdentifierTests(unittest.TestCase):
    """Counters above 2^53 and Unicode/long opaque identifiers."""

    def test_high_watermark_and_state_version_stay_decimal_beyond_2_53(self) -> None:
        store = SessionStore()
        session = store.create_session(
            project_id="prj-1",
            revision="rev-1",
            top="top",
            target={},
        )
        session.sequence = 2**53
        event = store.append_event(session, "test/high", {"value": "x"})
        self.assertEqual(event["sequence"], str(2**53 + 1))

        events, high, oldest = store.read_events(session, after=2**53, limit=10)
        self.assertIsNotNone(events)
        self.assertEqual(high, 2**53 + 1)
        self.assertEqual(oldest, 2**53 + 1)
        self.assertEqual([item["sequence"] for item in events], [str(2**53 + 1)])

        session.state_version = 2**53
        run = store.commit_run(
            session=session,
            kind="explain",
            level="L1",
            state="succeeded",
            cards=[{"card_id": "card-1", "state_version": "0"}],
            omitted=[],
            model_used=False,
        )
        self.assertEqual(run.to_wire()["state_version"], str(2**53 + 1))

    def test_unicode_space_and_long_revision_are_round_tripped(self) -> None:
        fixture = SidecarFixture()
        try:
            project_id = "p" * 256
            revision = "版本 1 " + "长" * 240
            top = "顶层 模块"
            created = fixture.request(
                "POST",
                "/api/v1/sessions",
                {
                    "project_id": project_id,
                    "revision": revision,
                    "top": top,
                    "target": {"profile": "中文 配置"},
                },
            )
            self.assertEqual(created.status, 201, created.payload)
            session_id = str(created.payload["data"]["session_id"])
            read = fixture.request("GET", "/api/v1/sessions/%s" % session_id)
            self.assertEqual(read.status, 200)
            self.assertEqual(read.payload["data"]["revision"], revision)
            self.assertEqual(read.payload["data"]["top"], top)
            run = fixture.run(
                session_id,
                {"kind": "explain", "selection": _selection()},
            )
            self.assertEqual(run.status, 201, run.payload)
        finally:
            fixture.close()

    def test_two_projects_can_run_in_parallel(self) -> None:
        fixture = SidecarFixture()
        try:
            sessions: dict[str, str] = {}
            for project_id in ("prj-alpha", "prj-beta"):
                response = fixture.request(
                    "POST",
                    "/api/v1/sessions",
                    {"project_id": project_id, "revision": "rev-1", "top": "top"},
                )
                self.assertEqual(response.status, 201, response.payload)
                sessions[project_id] = str(response.payload["data"]["session_id"])

            barrier = threading.Barrier(2)
            results: dict[str, Any] = {}
            errors: list[BaseException] = []
            lock = threading.Lock()

            def worker(project_id: str) -> None:
                try:
                    barrier.wait(timeout=5)
                    response = fixture.run(
                        sessions[project_id],
                        {"kind": "explain", "selection": _selection()},
                    )
                    with lock:
                        results[project_id] = response
                except BaseException as exc:  # pragma: no cover - reported below
                    with lock:
                        errors.append(exc)

            threads = [
                threading.Thread(target=worker, args=(project_id,))
                for project_id in ("prj-alpha", "prj-beta")
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10)

            self.assertEqual(errors, [])
            self.assertEqual(len(results), 2)
            run_ids = set()
            for project_id, response in results.items():
                self.assertEqual(response.status, 201, (project_id, response.payload))
                self.assertEqual(response.payload["data"]["state"], "degraded")
                run_ids.add(response.payload["data"]["run_id"])
            self.assertEqual(len(run_ids), 2)
        finally:
            fixture.close()


class ReportAndOutageTests(unittest.TestCase):
    """Large diagnostic windows and simultaneous dependency outages."""

    def test_large_diagnostic_window_is_capped_at_sixteen_cards(self) -> None:
        diagnostics = []
        for index in range(20):
            diagnostics.append(
                {
                    "id": "diag-%d" % index,
                    "code": "EDU_LATCH_INFERRED",
                    "raw_code": "LATCH",
                    "severity": "warning",
                    "stage": "synth",
                    "summary": "诊断 %d：条件分支未覆盖。" % index,
                    "origin": "core",
                    "confidence_kind": "tool",
                    "location": {"file": "rtl/top.v", "line": index + 1},
                }
            )
        outcome = _dispatcher(
            _FakeGateway(
                {"job-many": _report(job_id="job-many", diagnostics=diagnostics)}
            )
        ).dispatch(
            _request("report_review", job_id="job-many"),
            "2",
        )
        self.assertEqual(len(outcome.cards), 16)
        self.assertTrue(
            any(item["kind"] == "diagnostic" for item in outcome.omitted),
            outcome.omitted,
        )

    def test_model_and_gateway_outages_are_survivable_together(self) -> None:
        fixture = SidecarFixture()

        class _TimeoutOpener:
            def open(self, *args: Any, **kwargs: Any) -> Any:
                raise TimeoutError("slow model")

        try:
            with mock.patch.dict(
                os.environ,
                {
                    API_KEY_ENV: "sk-test-matrix-0123456789",
                    BASE_URL_ENV: "https://llm.example.edu/v1",
                    MODEL_ENV: "test-model",
                },
                clear=False,
            ):
                model = ModelClient(
                    status=ModelStatus(
                        available=True,
                        provider="llm.example.edu",
                        reason="configured",
                        model="test-model",
                    )
                )
                model._opener = _TimeoutOpener()
                fixture.state.dispatcher._model = model

                explained = fixture.create_session("rev-1")
                reviewed = fixture.create_session("rev-1")
                barrier = threading.Barrier(2)
                results: dict[str, Any] = {}

                def explain_worker() -> None:
                    barrier.wait(timeout=5)
                    results["explain"] = fixture.run(
                        explained,
                        {"kind": "explain", "selection": _selection()},
                    )

                def review_worker() -> None:
                    barrier.wait(timeout=5)
                    results["review"] = fixture.run(
                        reviewed,
                        {
                            "kind": "report_review",
                            "job_id": "job-down",
                            "selection": _selection(),
                        },
                    )

                threads = [
                    threading.Thread(target=explain_worker),
                    threading.Thread(target=review_worker),
                ]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join(timeout=15)

                self.assertEqual(results["explain"].status, 201)
                self.assertEqual(results["explain"].payload["data"]["state"], "degraded")
                self.assertFalse(results["explain"].payload["data"]["model_used"])
                self.assertEqual(results["review"].status, 503)
                self.assertEqual(
                    results["review"].payload["error"]["code"],
                    "GATEWAY_UNAVAILABLE",
                )
        finally:
            fixture.close()


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
