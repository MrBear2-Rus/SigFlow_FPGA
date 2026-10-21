"""Runtime contract, fixture and historical-deviation tests."""

from __future__ import annotations

import io
import json
import os
import sys
import unittest
import urllib.error
from copy import deepcopy
from pathlib import Path
from unittest.mock import patch

TESTS_DIR = Path(__file__).resolve().parent
AGENT_DIR = TESTS_DIR.parent
REPO_ROOT = AGENT_DIR.parent
SRC_DIR = AGENT_DIR / "src"
if str(AGENT_DIR) not in sys.path:
    sys.path.insert(0, str(AGENT_DIR))
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from sigflow_edu_agent.contract_schema import (  # noqa: E402
    ContractSchemaValidator,
    SchemaValidationError,
)
from sigflow_edu_agent.errors import ERROR_CODES  # noqa: E402
from sigflow_edu_agent.gateway_client import GatewayClient, GatewayOutcome  # noqa: E402
from tests.test_http_api import SidecarFixture  # noqa: E402

CONTRACT_DIR = REPO_ROOT / "contracts" / "edu-agent" / "v1"
SCHEMA_DIR = CONTRACT_DIR / "schemas"
FIXTURE_DIR = CONTRACT_DIR / "fixtures"


def _load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def _response_body(code: str, status: int = 400, message: str = "failure") -> io.BytesIO:
    return io.BytesIO(
        json.dumps(
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
    )


class ValidatorSelfTests(unittest.TestCase):
    """The dependency-free validator must reject real contract violations."""

    def test_validator_enforces_required_enum_bounds_and_additional_properties(self) -> None:
        validator = ContractSchemaValidator(SCHEMA_DIR)
        validator.validate(
            {
                "protocol": "edu.api.v1",
                "agent_version": "0.1.0",
                "instance_id": "inst-1",
                "ready": True,
                "model": {"available": False, "provider": None, "reason": "no key"},
            },
            "agent-health.schema.json",
        )
        with self.assertRaises(SchemaValidationError):
            validator.validate(
                {
                    "protocol": "edu.api.v1",
                    "agent_version": "0.1.0",
                    "instance_id": "inst-1",
                    "ready": True,
                },
                "agent-health.schema.json",
            )
        with self.assertRaises(SchemaValidationError):
            validator.validate(
                {
                    "project_id": "project-1",
                    "revision": "rev-1",
                    "source_id": "source-1",
                    "file_hash": "sha256:abc",
                    "start_line": 1,
                    "end_line": 1,
                    "unexpected": True,
                },
                "source-ref.schema.json",
            )


class RuntimeAgentSchemaTests(unittest.TestCase):
    """Every Agent-facing schema is checked against real loopback responses."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.validator = ContractSchemaValidator(SCHEMA_DIR)
        cls.fixture = SidecarFixture()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.close()

    def _success(self, response) -> dict:
        self.assertIn(response.status, (200, 201, 202), response.payload)
        assert response.payload is not None
        self.validator.validate(
            response.payload,
            "envelope.schema.json",
            "#/$defs/success",
        )
        self.assertIn("data", response.payload)
        return response.payload["data"]

    def test_real_agent_responses_match_every_agent_schema(self) -> None:
        covered: set[str] = {"envelope.schema.json"}

        health = self.fixture.request("GET", "/api/v1/health", token=None)
        self.validator.validate(self._success(health), "agent-health.schema.json")
        covered.add("agent-health.schema.json")

        capabilities = self.fixture.request("GET", "/api/v1/capabilities")
        caps_data = self._success(capabilities)
        self.validator.validate(caps_data, "agent-capabilities.schema.json")
        self.assertFalse(caps_data["course"]["available"])
        covered.add("agent-capabilities.schema.json")

        created = self.fixture.request(
            "POST",
            "/api/v1/sessions",
            {"project_id": "prj-contract", "revision": "rev-1", "top": "top"},
        )
        session_data = self._success(created)
        self.validator.validate(session_data, "agent-session.schema.json")
        covered.add("agent-session.schema.json")
        session_id = str(session_data["session_id"])

        detail = self.fixture.request(
            "GET",
            "/api/v1/sessions/%s" % session_id,
        )
        self.validator.validate(self._success(detail), "agent-session.schema.json")

        snippet = (
            "module blk(input clk, input en, input d, output reg q);\n"
            "  always @(*) begin\n"
            "    if (en)\n"
            "      q = d;\n"
            "  end\n"
            "endmodule\n"
        )
        run = self.fixture.run(
            session_id,
            {
                "kind": "explain",
                "selection": {
                    "source_id": "source-contract",
                    "node_id": "node-1",
                    "start_line": 1,
                    "end_line": 6,
                    "text": snippet,
                },
            },
        )
        run_data = self._success(run)
        self.validator.validate(run_data, "agent-run.schema.json")
        covered.add("agent-run.schema.json")
        self.assertTrue(run_data["cards"])
        for card in run_data["cards"]:
            self.validator.validate(card, "teaching-card.schema.json")
            covered.add("teaching-card.schema.json")

        run_id = str(run_data["run_id"])
        stored_run = self.fixture.request("GET", "/api/v1/runs/%s" % run_id)
        self.validator.validate(self._success(stored_run), "agent-run.schema.json")

        plan = self.fixture.run(session_id, {"kind": "plan"})
        plan_data = self._success(plan)
        self.validator.validate(plan_data, "agent-run.schema.json")
        for card in plan_data["cards"]:
            self.validator.validate(card, "plan-card.schema.json")
            covered.add("plan-card.schema.json")

        # The stub report is bound to project prj-1 and revision rev-12.
        report_session = self.fixture.create_session("rev-12")
        report = self.fixture.run(
            report_session,
            {
                "kind": "report_review",
                "job_id": "job-1",
                "selection": {
                    "source_id": "source-contract",
                    "start_line": 1,
                    "end_line": 6,
                    "text": snippet,
                },
            },
        )
        report_data = self._success(report)
        self.validator.validate(report_data, "agent-run.schema.json")
        for card in report_data["cards"]:
            self.validator.validate(card, "teaching-card.schema.json")

        events = self.fixture.request(
            "GET",
            "/api/v1/sessions/%s/events?after=0" % session_id,
        )
        event_data = self._success(events)
        self.assertTrue(event_data["events"])
        for event in event_data["events"]:
            self.validator.validate(event, "agent-event.schema.json")
            covered.add("agent-event.schema.json")

        forbidden = self.fixture.request("GET", "/api/v1/capabilities", token=None)
        self.assertEqual(forbidden.status, 401)
        assert forbidden.payload is not None
        self.validator.validate(
            forbidden.payload,
            "envelope.schema.json",
            "#/$defs/failure",
        )

        self.assertEqual(
            covered,
            {
                "envelope.schema.json",
                "agent-health.schema.json",
                "agent-capabilities.schema.json",
                "agent-session.schema.json",
                "agent-run.schema.json",
                "agent-event.schema.json",
                "teaching-card.schema.json",
                "plan-card.schema.json",
            },
        )


class FixtureSchemaTests(unittest.TestCase):
    """The shared golden/source fixtures validate against every remaining schema."""

    def test_every_schema_has_runtime_or_fixture_evidence(self) -> None:
        validator = ContractSchemaValidator(SCHEMA_DIR)
        schema_names = set(validator.schema_names())
        covered: set[str] = {
            "envelope.schema.json",
            "agent-health.schema.json",
            "agent-capabilities.schema.json",
            "agent-session.schema.json",
            "agent-run.schema.json",
            "agent-event.schema.json",
            "teaching-card.schema.json",
            "plan-card.schema.json",
        }

        golden = _load_json(FIXTURE_DIR / "golden.json")
        self.assertIsInstance(golden, list)
        for fixture in golden:
            schema_name = fixture["schema"]
            self.assertIn(schema_name, schema_names, fixture["name"])
            validator.validate(fixture["document"], schema_name)
            covered.add(schema_name)

        cards = _load_json(FIXTURE_DIR / "agent_cards.json")
        for card in cards["cards"]:
            validator.validate(card, "teaching-card.schema.json")
        for plan in cards["plan_cards"]:
            validator.validate(plan, "plan-card.schema.json")
        with self.assertRaises(SchemaValidationError):
            validator.validate(cards["negative"]["bad_unknown_kind"], "teaching-card.schema.json")
        with self.assertRaises(SchemaValidationError):
            validator.validate(cards["negative"]["bad_missing_card_id"], "teaching-card.schema.json")

        source_fixture = _load_json(FIXTURE_DIR / "source-ref.json")
        validator.validate(source_fixture["valid"], "source-ref.schema.json")
        with self.assertRaises(SchemaValidationError):
            validator.validate(
                source_fixture["negative"]["empty_interval_returns_zero"],
                "source-ref.schema.json",
            )
        covered.add("source-ref.schema.json")

        self.assertEqual(
            covered,
            schema_names,
            "schemas without runtime/fixture evidence: %s"
            % ", ".join(sorted(schema_names - covered)),
        )


class KnownDeviationTests(unittest.TestCase):
    """Each historical contract deviation has an implementation/fixture conclusion."""

    def setUp(self) -> None:
        self.validator = ContractSchemaValidator(SCHEMA_DIR)
        self.client = GatewayClient("http://127.0.0.1:1", "token")

    def test_source_ref_empty_interval_is_rejected_and_never_returns_zero(self) -> None:
        fixture = _load_json(FIXTURE_DIR / "source-ref.json")
        self.validator.validate(fixture["valid"], "source-ref.schema.json")
        with self.assertRaises(SchemaValidationError):
            self.validator.validate(
                fixture["negative"]["empty_interval_returns_zero"],
                "source-ref.schema.json",
            )
        with self.assertRaises(ValueError):
            self.client.source("prj-1", "source-1", 7, 0)

    def test_wave_tick_accepts_uint64_strings_and_integers_but_requires_string_responses(self) -> None:
        outcome = GatewayOutcome(ok=True, report={})
        with patch.object(self.client, "request", return_value=outcome) as request:
            self.client.wave_query("art-1", {"start_tick": 0, "end_tick": 10})
            self.client.wave_query("art-1", {"start_tick": "0", "end_tick": "10"})
        for call in request.call_args_list:
            body = call.args[2]
            self.assertEqual(body["start_tick"], "0")
            self.assertEqual(body["end_tick"], "10")

        for body in (
            {"start_tick": True, "end_tick": 10},
            {"start_tick": -1, "end_tick": 10},
            {"start_tick": 1.5, "end_tick": 10},
            {"start_tick": "0", "end_tick": "18446744073709551616"},
            {"start_tick": 11, "end_tick": 10},
        ):
            with self.subTest(body=body), self.assertRaises(ValueError):
                self.client.wave_query("art-1", body)

        golden = _load_json(FIXTURE_DIR / "golden.json")
        wave = next(item["document"] for item in golden if item["name"] == "wave-query.exact")
        self.validator.validate(wave, "wave-query.schema.json")
        invalid = deepcopy(wave)
        invalid["signals"][0]["transitions"][0]["tick"] = 1
        with self.assertRaises(SchemaValidationError):
            self.validator.validate(invalid, "wave-query.schema.json")

    def test_legacy_error_codes_map_to_the_frozen_agent_enum(self) -> None:
        cases = {
            "PATH_OUTSIDE_PROJECT": "POLICY_DENIED",
            "SOURCE_UNAVAILABLE": "GATEWAY_UNAVAILABLE",
            "PLUGIN_UNAVAILABLE": "CAPABILITY_UNAVAILABLE",
        }
        for legacy, expected in cases.items():
            with self.subTest(legacy=legacy):
                error = urllib.error.HTTPError(
                    "http://127.0.0.1:1/api/v1/test",
                    403,
                    "failure",
                    {},
                    _response_body(legacy, 403, "legacy Gateway error"),
                )
                outcome = self.client._http_error_outcome(error)  # noqa: SLF001
                self.assertEqual(outcome.code, expected)
                self.assertIn(outcome.code, ERROR_CODES)

        forbidden = {
            "schema_version": "edu.api.v1",
            "request_id": "req-1",
            "trace_id": "trace-1",
            "error": {
                "code": "PATH_OUTSIDE_PROJECT",
                "message": "legacy",
                "retryable": False,
                "details": None,
            },
        }
        with self.assertRaises(SchemaValidationError):
            self.validator.validate(
                forbidden,
                "envelope.schema.json",
                "#/$defs/failure",
            )


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
