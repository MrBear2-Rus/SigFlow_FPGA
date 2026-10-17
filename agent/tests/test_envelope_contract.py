"""T4 contract tests for envelopes, error codes, IDs and wire values.

The scanner is intentionally recursive: a value that is valid at the envelope
top level but represented as a JSON number inside a run, card or event still
fails the test.  All tests use the real HTTP server and the shared Gateway
stub, so the assertions cover bytes that the UI can actually receive.
"""

from __future__ import annotations

import json
import os
import re
import sys
import unittest
from datetime import datetime
from typing import Any, Mapping

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.dirname(TESTS_DIR)
SRC_DIR = os.path.join(AGENT_DIR, "src")
if AGENT_DIR not in sys.path:
    sys.path.insert(0, AGENT_DIR)
if SRC_DIR not in sys.path:
    sys.path.insert(0, SRC_DIR)

from sigflow_edu_agent.errors import (  # noqa: E402
    ERROR_CODES,
    AgentError,
    failure_envelope,
    validate_response_envelope,
)
from sigflow_edu_agent.version import PROTOCOL  # noqa: E402
from tests.test_http_api import (  # noqa: E402
    UI_TOKEN,
    SidecarFixture,
    _raw_request,
    _selection,
)

_UINT64_FIELDS = frozenset(
    {
        "state_version",
        "sequence",
        "size",
        "tick",
        "start_tick",
        "end_tick",
        "high_watermark",
        "oldest_sequence",
        "next_after",
    }
)
_TIMESTAMP_FIELDS = frozenset({"at", "created_at", "timestamp"})
_ID_PATTERNS = {
    "session_id": re.compile(r"^sess-[1-9][0-9]*$"),
    "run_id": re.compile(r"^run-[1-9][0-9]*$"),
    "event_id": re.compile(r"^ev-[1-9][0-9]*$"),
}
_REQUEST_ID = re.compile(r"^req-([1-9][0-9]*)$")
_TRACE_ID = re.compile(r"^trace-([1-9][0-9]*)$")
_DECIMAL_64 = re.compile(r"^(?:0|[1-9][0-9]*)$")
_UTC_SECOND = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$")


def _walk_pairs(value: Any, path: tuple[str, ...] = ()):
    """Yield every mapping entry with a dotted path for failure messages."""
    if isinstance(value, Mapping):
        for key, item in value.items():
            child = path + (str(key),)
            yield child, key, item
            yield from _walk_pairs(item, child)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from _walk_pairs(item, path + ("[%d]" % index,))


def _assert_wire_values(test: unittest.TestCase, payload: Mapping[str, Any]) -> None:
    """Negative scan: fail on non-decimal 64-bit values or illegal IDs/times."""
    for path, key, value in _walk_pairs(payload):
        location = ".".join(path)
        if key in _UINT64_FIELDS:
            test.assertIsInstance(value, str, "%s must be a decimal string" % location)
            test.assertRegex(value, _DECIMAL_64, location)
            test.assertLessEqual(int(value), 2**64 - 1, "%s exceeds uint64" % location)
        elif key in _TIMESTAMP_FIELDS:
            test.assertIsInstance(value, str, "%s must be a timestamp" % location)
            test.assertRegex(value, _UTC_SECOND, "%s must use UTC ISO 8601" % location)
            datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
        elif key in _ID_PATTERNS:
            test.assertIsInstance(value, str, "%s must be an ID string" % location)
            test.assertRegex(value, _ID_PATTERNS[key], "%s has the wrong ID shape" % location)
        elif key == "source_id" and value is not None:
            # source_id is scoped by project; this scan only requires an opaque
            # string and must never impose a global uniqueness format on it.
            test.assertIsInstance(value, str, "%s must be an opaque string" % location)
        if (
            key == "error"
            and isinstance(value, Mapping)
            and "code" in value
            and "message" in value
        ):
            test.assertIn(value["code"], ERROR_CODES, "%s.error.code is not frozen" % location)


def _scan_envelope(
    test: unittest.TestCase,
    status: int,
    payload: Mapping[str, Any] | None,
) -> tuple[int, int]:
    """Check one complete envelope and return its request/trace ordinals."""
    test.assertIsNotNone(payload)
    assert payload is not None
    test.assertEqual(payload.get("schema_version"), PROTOCOL)
    request_match = _REQUEST_ID.fullmatch(str(payload.get("request_id")))
    trace_match = _TRACE_ID.fullmatch(str(payload.get("trace_id")))
    test.assertIsNotNone(request_match)
    test.assertIsNotNone(trace_match)

    if "error" in payload:
        test.assertEqual(
            set(payload),
            {"schema_version", "request_id", "trace_id", "error"},
        )
        error = payload["error"]
        test.assertEqual(set(error), {"code", "message", "retryable", "details"})
        test.assertIn(error["code"], ERROR_CODES)
        test.assertIsInstance(error["message"], str)
        test.assertIsInstance(error["retryable"], bool)
        test.assertEqual(error["retryable"], status == 503)
        test.assertIsNone(error["details"])
    else:
        test.assertEqual(
            set(payload),
            {"schema_version", "request_id", "trace_id", "data"},
        )

    _assert_wire_values(test, payload)
    return int(request_match.group(1)), int(trace_match.group(1))


class EnvelopeHelperTests(unittest.TestCase):
    """Frozen helper-level rules that every HTTP response relies on."""

    def test_error_code_enum_is_the_frozen_twelve(self) -> None:
        self.assertEqual(len(ERROR_CODES), 12)
        self.assertEqual(len(set(ERROR_CODES)), 12)
        for forbidden in (
            "PATH_OUTSIDE_PROJECT",
            "SOURCE_UNAVAILABLE",
            "PLUGIN_UNAVAILABLE",
        ):
            self.assertNotIn(forbidden, ERROR_CODES)

    def test_retryable_is_derived_from_http_status(self) -> None:
        service = failure_envelope(
            "req-1",
            "trace-1",
            "SERVICE_UNAVAILABLE",
            "temporarily unavailable",
            retryable=False,
            http_status=503,
        )
        invalid = failure_envelope(
            "req-2",
            "trace-2",
            "INVALID_ARGUMENT",
            "bad request",
            retryable=True,
            http_status=400,
        )
        self.assertTrue(service["error"]["retryable"])
        self.assertFalse(invalid["error"]["retryable"])
        unknown_status = failure_envelope(
            "req-3",
            "trace-3",
            "SERVICE_UNAVAILABLE",
            "status is not known",
            retryable=True,
        )
        self.assertFalse(unknown_status["error"]["retryable"])
        validate_response_envelope(service, 503)
        validate_response_envelope(invalid, 400)

    def test_details_are_always_null_and_cannot_be_smuggled(self) -> None:
        with self.assertRaises(ValueError):
            failure_envelope(
                "req-1",
                "trace-1",
                "INVALID_ARGUMENT",
                "bad request",
                details={"path": "C:\\secret"},
                http_status=400,
            )

    def test_frozen_enum_rejects_undocumented_codes(self) -> None:
        with self.assertRaises(ValueError):
            AgentError("PATH_OUTSIDE_PROJECT", "not part of edu.api.v1")
        with self.assertRaises(ValueError):
            failure_envelope(
                "req-1",
                "trace-1",
                "SOURCE_UNAVAILABLE",
                "not part of edu.api.v1",
                http_status=503,
            )

    def test_envelope_validator_rejects_wrong_schema_and_retryability(self) -> None:
        bad_schema = failure_envelope(
            "req-1",
            "trace-1",
            "INVALID_ARGUMENT",
            "bad request",
            http_status=400,
        )
        bad_schema["schema_version"] = "edu.api.v0"
        with self.assertRaises(ValueError):
            validate_response_envelope(bad_schema, 400)

        wrong_retryable = failure_envelope(
            "req-2",
            "trace-2",
            "SERVICE_UNAVAILABLE",
            "temporarily unavailable",
            http_status=503,
        )
        wrong_retryable["error"]["retryable"] = False
        with self.assertRaises(ValueError):
            validate_response_envelope(wrong_retryable, 503)


class HttpEnvelopeScanTests(unittest.TestCase):
    """Scan every response shape the UI can receive through the real server."""

    def test_all_http_shapes_pass_the_envelope_and_wire_scan(self) -> None:
        fixture = SidecarFixture()
        responses: list[tuple[int, Mapping[str, Any] | None]] = []

        def record(response: Any) -> Any:
            responses.append((response.status, response.payload))
            return response

        try:
            record(fixture.request("GET", "/api/v1/health", token=None))
            record(fixture.request("GET", "/api/v1/capabilities"))

            created = record(
                fixture.request(
                    "POST",
                    "/api/v1/sessions",
                    {"project_id": "prj-t4", "revision": "rev-t4"},
                )
            )
            self.assertEqual(created.status, 201)
            assert created.payload is not None
            session_id = str(created.payload["data"]["session_id"])
            record(fixture.request("GET", "/api/v1/sessions/%s" % session_id))

            run = record(
                fixture.run(
                    session_id,
                    {"kind": "explain", "selection": _selection()},
                )
            )
            self.assertEqual(run.status, 201)
            assert run.payload is not None
            run_id = str(run.payload["data"]["run_id"])
            record(fixture.request("GET", "/api/v1/runs/%s" % run_id))
            record(
                fixture.request(
                    "GET",
                    "/api/v1/sessions/%s/events?after=0" % session_id,
                )
            )

            record(
                fixture.run(
                    session_id,
                    {"kind": "report_review", "job_id": "job-down", "selection": _selection()},
                )
            )
            record(fixture.request("GET", "/api/v1/runs/run-does-not-exist"))
            record(
                fixture.request(
                    "GET",
                    "/api/v1/sessions/%s/events?after=999999" % session_id,
                )
            )
            record(fixture.request("GET", "/api/v1/capabilities", token=None))
            record(
                _raw_request(
                    fixture.port,
                    "GET",
                    "/api/v1/capabilities",
                    "evil.example.test",
                    token=UI_TOKEN,
                )
            )
            record(
                fixture.request(
                    "POST",
                    "/api/v1/sessions",
                    raw_body=b"x" * (1024 * 1024 + 1),
                )
            )

            # Force the last-line privacy guard to replace a success response
            # with a 503 failure envelope.
            record(
                fixture.request(
                    "POST",
                    "/api/v1/sessions",
                    {
                        "project_id": "prj-t4-private",
                        "revision": "rev-t4",
                        "target": {"local_path": "C:\\private\\project"},
                    },
                )
            )

            shutdown = record(fixture.request("POST", "/api/v1/shutdown"))
            self.assertEqual(shutdown.status, 202)

            ordinals: list[tuple[int, int]] = []
            for status, payload in responses:
                ordinals.append(_scan_envelope(self, status, payload))
            self.assertTrue(all(request > 0 and trace > 0 for request, trace in ordinals))
            self.assertEqual(
                [request for request, _ in ordinals],
                sorted(request for request, _ in ordinals),
            )
            self.assertEqual(
                [trace for _, trace in ordinals],
                sorted(trace for _, trace in ordinals),
            )
            self.assertEqual(len({request for request, _ in ordinals}), len(ordinals))
            self.assertEqual(len({trace for _, trace in ordinals}), len(ordinals))
        finally:
            fixture.close()

    def test_source_id_is_not_used_as_a_global_key(self) -> None:
        fixture = SidecarFixture()
        try:
            first = fixture.request(
                "POST",
                "/api/v1/sessions",
                {"project_id": "prj-a", "revision": "rev-a"},
            )
            second = fixture.request(
                "POST",
                "/api/v1/sessions",
                {"project_id": "prj-b", "revision": "rev-b"},
            )
            self.assertEqual(first.status, 201)
            self.assertEqual(second.status, 201)
            assert first.payload is not None and second.payload is not None
            first_id = str(first.payload["data"]["session_id"])
            second_id = str(second.payload["data"]["session_id"])

            same_source = _selection()
            for session_id in (first_id, second_id):
                response = fixture.run(
                    session_id,
                    {"kind": "explain", "selection": dict(same_source)},
                )
                self.assertEqual(response.status, 201, response.payload)
                assert response.payload is not None
                for card in response.payload["data"]["cards"]:
                    serialized = json.dumps(card, ensure_ascii=False)
                    self.assertIn(same_source["source_id"], serialized)
        finally:
            fixture.close()


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
