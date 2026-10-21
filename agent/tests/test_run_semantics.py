"""T5 acceptance tests for the four run kinds and policy gates."""

from __future__ import annotations

import os
import sys
import unittest
from typing import Any, Mapping

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.dirname(TESTS_DIR)
SRC_DIR = os.path.join(AGENT_DIR, "src")
if AGENT_DIR not in sys.path:
    sys.path.insert(0, AGENT_DIR)
if SRC_DIR not in sys.path:
    sys.path.insert(0, SRC_DIR)

from sigflow_edu_agent.dispatch import (  # noqa: E402
    RUN_STATES as DISPATCH_RUN_STATES,
    RunDispatcher,
    RunRequest,
    sufficient_report,
)
from sigflow_edu_agent.errors import AgentError  # noqa: E402
from sigflow_edu_agent.gateway_client import GatewayOutcome  # noqa: E402
from sigflow_edu_agent.model_client import ModelClient, ModelStatus  # noqa: E402
from sigflow_edu_agent.rules import Selection  # noqa: E402
from sigflow_edu_agent.util import (  # noqa: E402
    REDACTED_PATH,
    contains_absolute_path,
    redact_local_paths,
    redact_structure_paths,
)
from tests.test_http_api import SidecarFixture  # noqa: E402

LATCH_SNIPPET = (
    "module blk(input clk, input en, input d, output reg q);\n"
    "  always @(*) begin\n"
    "    if (en)\n"
    "      q = d;\n"
    "  end\n"
    "endmodule\n"
)

RUN_STATES = {"succeeded", "degraded", "failed"}


def _selection(text: str = LATCH_SNIPPET) -> Selection:
    """Return a selection carrying only opaque source metadata."""
    return Selection(
        text=text,
        source_id="source-000000000000000000000001",
        node_id="node-1",
        start_line=1,
        end_line=6,
    )


def _report(
    *,
    job_id: str = "job-1",
    origin: str = "core",
    completeness: str = "complete",
    diagnostics: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Build a normalized report with a complete input binding."""
    return {
        "schema_version": "edu.jobreport.v1",
        "job_id": job_id,
        "origin": origin,
        "capability": "eda.synth",
        "plugin_id": "edu-synth",
        "plugin_version": "1.0.0",
        "project_id": "prj-1",
        "revision": "rev-1",
        "snapshot_id": "snap-1",
        "state": "Succeeded",
        "exit_code": 0,
        "diagnostics": diagnostics if diagnostics is not None else [],
        "metrics": {},
        "artifacts": [],
        "completeness": completeness,
        "input_fingerprint": "sha256:test",
        "raw_report_schema": "eda.jobreport.v1",
    }


class _FakeGateway:
    """Single-read Gateway stub that records the requested job ID."""

    def __init__(
        self,
        reports: Mapping[str, Mapping[str, Any]] | None = None,
        failure: GatewayOutcome | None = None,
    ) -> None:
        self.reports = dict(reports or {})
        self.failure = failure
        self.calls: list[str] = []
        self.base_url = "http://127.0.0.1:1"

    def fetch_job_report(self, job_id: str) -> GatewayOutcome:
        self.calls.append(job_id)
        if self.failure is not None:
            return self.failure
        report = self.reports.get(job_id)
        if report is None:
            return GatewayOutcome(False, code="NOT_FOUND", message="unknown job")
        return GatewayOutcome(True, report=report)


def _dispatcher(
    gateway: _FakeGateway | None = None,
    *,
    model_available: bool = False,
) -> RunDispatcher:
    """Create a dispatcher with a deterministic model status."""
    status = ModelStatus(
        available=model_available,
        provider="test" if model_available else None,
        reason="configured" if model_available else "no key",
        model="test-model" if model_available else None,
    )
    return RunDispatcher(
        gateway=gateway or _FakeGateway(),  # type: ignore[arg-type]
        model=ModelClient(status=status),
    )


def _request(kind: str, *, level: str = "L1", job_id: str | None = None) -> RunRequest:
    """Build a request bound to the fake project/session."""
    return RunRequest(
        kind=kind,
        level=level,
        selection=_selection(),
        job_id=job_id,
        top="top",
        revision="rev-1",
        project_id="prj-1",
    )


class FourRunKindsTests(unittest.TestCase):
    """Acceptance: every run kind has a positive semantic case."""

    def test_explain_uses_rule_engine_without_a_model_key(self) -> None:
        outcome = _dispatcher().dispatch(_request("explain"), "2")
        self.assertIn(outcome.state, RUN_STATES)
        self.assertEqual(outcome.state, "degraded")
        self.assertFalse(outcome.model_used)
        self.assertEqual(outcome.cards[0]["kind"], "rule")
        self.assertEqual(outcome.cards[0]["source"], "rule")
        self.assertEqual(outcome.cards[0]["evidence"][0]["kind"], "rule_match")
        self.assertTrue(
            any(item["kind"] == "model_explanation" for item in outcome.omitted)
        )

    def test_hint_is_a_rule_question_not_a_solution(self) -> None:
        outcome = _dispatcher().dispatch(_request("hint"), "2")
        card = outcome.cards[0]
        self.assertIn(outcome.state, RUN_STATES)
        self.assertEqual(outcome.state, "degraded")
        self.assertFalse(outcome.model_used)
        self.assertEqual(card["kind"], "teaching")
        self.assertEqual(card["source"], "rule")
        self.assertIn("请回答一个问题", card["body"])
        self.assertNotIn("q = d", card["body"])
        self.assertTrue(any(item["kind"] == "model_hint" for item in outcome.omitted))

    def test_plan_returns_one_path_free_plan_card(self) -> None:
        gateway = _FakeGateway()
        outcome = _dispatcher(gateway).dispatch(_request("plan"), "2")
        self.assertEqual(outcome.state, "succeeded")
        self.assertFalse(outcome.model_used)
        self.assertEqual(len(outcome.cards), 1)
        card = outcome.cards[0]
        self.assertIn("plan_id", card)
        self.assertNotIn("kind", card)
        self.assertEqual(
            [step["capability"] for step in card["steps"]],
            ["eda.synth", "eda.sim.build", "eda.sim.run"],
        )
        self.assertTrue(card["snapshot_required"])
        self.assertTrue(card["requires_approval"])
        for step in card["steps"]:
            self.assertNotIn("path", step["params"])
            self.assertNotIn("executable", step["params"])
            self.assertNotIn("command", step["params"])
            self.assertNotIn("script", step["params"])
            self.assertNotIn("work_dir", step["params"])
        self.assertEqual(gateway.calls, [], "plan generation must not touch the Gateway")

    def test_report_review_maps_diagnostics_from_the_requested_job(self) -> None:
        diagnostic = {
            "id": "diag-1",
            "code": "EDU_LATCH_INFERRED",
            "raw_code": "LATCH",
            "severity": "warning",
            "stage": "synth",
            "summary": "A latch was inferred.",
            "origin": "core",
            "confidence_kind": "tool",
            "location": {"file": "rtl/top.v", "line": 7},
        }
        gateway = _FakeGateway(
            {"job-1": _report(diagnostics=[diagnostic])}
        )
        outcome = _dispatcher(gateway).dispatch(
            _request("report_review", job_id="job-1"),
            "2",
        )
        self.assertIn(outcome.state, RUN_STATES)
        self.assertFalse(outcome.model_used)
        self.assertEqual(gateway.calls, ["job-1"])
        self.assertEqual(outcome.cards[0]["kind"], "diagnostic_explanation")
        self.assertEqual(
            outcome.cards[0]["evidence"][0]["ref"]["code"],
            "EDU_LATCH_INFERRED",
        )


class L4PolicyTests(unittest.TestCase):
    """Acceptance: L4 is always refused by the base run route."""

    def test_dispatcher_refuses_l4_even_with_more_true(self) -> None:
        request = RunRequest(
            kind="explain",
            level="L4",
            selection=_selection(),
            more=True,
        )
        with self.assertRaises(AgentError) as caught:
            _dispatcher().dispatch(request, "2")
        self.assertEqual(caught.exception.code, "POLICY_DENIED")
        self.assertEqual(caught.exception.http_status, 403)

    def test_http_refuses_l4_before_kind_specific_validation(self) -> None:
        fixture = SidecarFixture()
        try:
            session_id = fixture.create_session()
            for kind in ("explain", "hint", "plan", "report_review"):
                with self.subTest(kind=kind):
                    response = fixture.run(
                        session_id,
                        {
                            "kind": kind,
                            "level": "L4",
                            "more": True,
                            "selection": {"text": LATCH_SNIPPET},
                        },
                    )
                    self.assertEqual(response.status, 403)
                    assert response.payload is not None
                    self.assertEqual(response.payload["error"]["code"], "POLICY_DENIED")
                    self.assertNotIn("reference_solution", str(response.payload))
        finally:
            fixture.close()


class ReportTrustTests(unittest.TestCase):
    """Acceptance: only complete/exact non-legacy empty reports are explained."""

    def test_exact_gateway_empty_report_is_trustworthy(self) -> None:
        report = _report(origin="gateway", completeness="exact")
        trustworthy, reasons = sufficient_report(report)
        self.assertTrue(trustworthy, reasons)
        outcome = _dispatcher(
            _FakeGateway(
                {
                    "job-exact": _report(
                        job_id="job-exact",
                        origin="gateway",
                        completeness="exact",
                    )
                }
            )
        ).dispatch(
            _request("report_review", job_id="job-exact"),
            "2",
        )
        self.assertEqual(outcome.cards[0]["kind"], "teaching")
        self.assertIn("没有报告任何诊断", outcome.cards[0]["body"])
        self.assertIn("不能据此判断电路正确", outcome.cards[0]["body"])

    def test_legacy_or_partial_empty_report_refuses_to_conclude(self) -> None:
        variants = (
            _report(job_id="job-legacy", origin="legacy", completeness="complete"),
            _report(job_id="job-partial", origin="core", completeness="partial"),
        )
        for report in variants:
            with self.subTest(job_id=report["job_id"]):
                outcome = _dispatcher(
                    _FakeGateway({str(report["job_id"]): report})
                ).dispatch(
                    _request("report_review", job_id=str(report["job_id"])),
                    "2",
                )
                card = outcome.cards[0]
                self.assertEqual(card["kind"], "rule")
                self.assertIsNone(card["issue_id"])
                self.assertIn("证据不足", card["title"])
                self.assertNotIn("所以电路没有问题", card["body"])

    def test_report_review_requires_a_job_id(self) -> None:
        with self.assertRaises(AgentError) as caught:
            _dispatcher().dispatch(_request("report_review"), "2")
        self.assertEqual(caught.exception.code, "INVALID_ARGUMENT")

    def test_gateway_failure_returns_failed_without_fake_cards(self) -> None:
        gateway = _FakeGateway(
            failure=GatewayOutcome(
                False,
                code="GATEWAY_UNAVAILABLE",
                message="gateway unavailable",
            )
        )
        outcome = _dispatcher(gateway).dispatch(
            _request("report_review", job_id="job-1"),
            "2",
        )
        self.assertEqual(outcome.state, "failed")
        self.assertEqual(outcome.cards, [])
        self.assertEqual(outcome.error_code, "GATEWAY_UNAVAILABLE")
        self.assertFalse(outcome.model_used)

    def test_malformed_diagnostic_still_rejects_the_report(self) -> None:
        malformed = _report(
            job_id="job-malformed",
            diagnostics=[
                {
                    "id": "diag-broken",
                    "code": "EDU_LATCH_INFERRED",
                    "severity": "warning",
                    "stage": "synth",
                    "origin": "core",
                    "confidence_kind": "tool",
                }
            ],
        )
        with self.assertRaises(AgentError) as caught:
            _dispatcher(_FakeGateway({"job-malformed": malformed})).dispatch(
                _request("report_review", job_id="job-malformed"),
                "2",
            )
        self.assertEqual(caught.exception.code, "GATEWAY_UNAVAILABLE")
        self.assertEqual(caught.exception.http_status, 503)


class ReportPathRedactionTests(unittest.TestCase):
    """A2: paths in free text are redacted, not treated as a broken report."""

    PATHS = (
        r"C:\Users\张三\我的 项目\rtl\top.v",
        "C:/Users/Alice/My Project/rtl/top.v",
        r"c:\Temp\学生 工程\top.sv",
        r"\\fileserver\share\team\长 路径\top.vhd",
        "//fileserver/share/team/top.v",
        "/home/学生/项目/rtl/top.v",
        "/Users/Alice/My Project/rtl/top.v",
        "/HOME/CASE/Top.V",
        "~/proj/rtl/top.v",
        "C:\\Users\\long\\" + "\\".join(["segment%02d" % i for i in range(40)]) + "\\top.v",
    )

    def test_path_variants_are_replaced_with_the_stable_placeholder(self) -> None:
        for path in self.PATHS:
            with self.subTest(path=path[:48]):
                text = '诊断指出 "%s" 处存在问题。' % path
                cleaned, count = redact_local_paths(text)
                self.assertEqual(count, 1)
                self.assertIn(REDACTED_PATH, cleaned)
                self.assertNotIn(path, cleaned)
                self.assertTrue(contains_absolute_path(text))
                self.assertFalse(contains_absolute_path(cleaned))

    def test_url_paths_are_not_mistaken_for_local_paths(self) -> None:
        text = "GET http://127.0.0.1:19387/api/v1/jobs/job-1/report returned 200"
        self.assertFalse(contains_absolute_path(text))
        self.assertEqual(redact_local_paths(text), (text, 0))

    def test_artifact_and_plugin_descriptions_are_redacted_recursively(self) -> None:
        report = {
            "artifacts": [
                {"description": "generated at C:\\Users\\student\\proj\\out"},
                {"description": "copied from /mnt/data/课程 项目/out"},
            ],
            "plugin": {"description": "installed in \\\\server\\share\\tools"},
        }
        cleaned, count = redact_structure_paths(report)
        self.assertEqual(count, 3)
        self.assertNotIn("C:\\Users", str(cleaned))
        self.assertNotIn("/mnt/data", str(cleaned))
        self.assertNotIn("\\\\server", str(cleaned))
        self.assertEqual(str(cleaned).count(REDACTED_PATH), 3)


class RunValidationTests(unittest.TestCase):
    """The dispatcher rejects unknown kinds and levels before any run work."""

    def test_invalid_kind_and_level_are_rejected(self) -> None:
        for request in (
            RunRequest(kind="solve_it", level="L1", selection=_selection()),
            RunRequest(kind="explain", level="L9", selection=_selection()),
        ):
            with self.subTest(request=request), self.assertRaises(AgentError) as caught:
                _dispatcher().dispatch(request, "2")
            self.assertEqual(caught.exception.code, "INVALID_ARGUMENT")

    def test_default_http_level_is_l1(self) -> None:
        fixture = SidecarFixture()
        try:
            session_id = fixture.create_session()
            response = fixture.run(
                session_id,
                {"kind": "explain", "selection": {"text": LATCH_SNIPPET}},
            )
            self.assertEqual(response.status, 201)
            assert response.payload is not None
            self.assertEqual(response.payload["data"]["cards"][0]["level"], "L1")
        finally:
            fixture.close()

    def test_dispatcher_exposes_only_the_frozen_run_states(self) -> None:
        self.assertEqual(set(DISPATCH_RUN_STATES), RUN_STATES)


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
