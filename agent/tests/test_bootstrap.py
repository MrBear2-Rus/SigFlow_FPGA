"""Tests for the stdin bootstrap handshake and the test-only ready records."""

from __future__ import annotations

import io
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from sigflow_edu_agent.__main__ import (  # noqa: E402
    SELFTEST_MODES,
    _selftest_ready_record,
    build_parser,
    emit_ready,
)
from sigflow_edu_agent.bootstrap import (  # noqa: E402
    Bootstrap,
    BootstrapError,
    from_mapping,
    parse_line,
    read_from_stream,
)
from sigflow_edu_agent.util import RedactionFilter, json_dumps  # noqa: E402
from sigflow_edu_agent.version import AGENT_VERSION, PROTOCOL  # noqa: E402

VALID_LINE = json_dumps(
    {
        "type": "sigflow-bootstrap",
        "protocol": "edu.api.v1",
        "instance_id": "inst-test-1",
        "nonce": "a" * 64,
        "gateway_url": "http://127.0.0.1:19999",
        "gateway_token": "tok-edge-abc",
        "ui_token": "tok-ui-xyz",
        "data_directory": "C:\\Users\\test\\AppData\\Local\\SigFlow\\agent",
    }
)


class BootstrapParseTests(unittest.TestCase):
    """The bootstrap record gates process startup, so it is validated strictly."""

    def test_valid_line_is_accepted(self) -> None:
        bootstrap = parse_line(VALID_LINE)
        self.assertEqual(bootstrap.instance_id, "inst-test-1")
        self.assertEqual(bootstrap.gateway_url, "http://127.0.0.1:19999")
        self.assertEqual(bootstrap.ui_token, "tok-ui-xyz")

    def test_repr_hides_tokens(self) -> None:
        bootstrap = parse_line(VALID_LINE)
        rendered = repr(bootstrap)
        self.assertNotIn("tok-edge-abc", rendered)
        self.assertNotIn("tok-ui-xyz", rendered)
        self.assertNotIn("AppData", rendered)

    def test_secrets_property_lists_both_tokens(self) -> None:
        bootstrap = parse_line(VALID_LINE)
        self.assertEqual(bootstrap.secrets, ("tok-edge-abc", "tok-ui-xyz"))

    def test_wrong_type_is_rejected(self) -> None:
        payload = json.loads(VALID_LINE)
        payload["type"] = "something-else"
        with self.assertRaises(BootstrapError):
            from_mapping(payload)

    def test_wrong_protocol_is_rejected(self) -> None:
        payload = json.loads(VALID_LINE)
        payload["protocol"] = "edu.api.v2"
        with self.assertRaises(BootstrapError):
            from_mapping(payload)

    def test_missing_field_is_rejected(self) -> None:
        for field_name in ("instance_id", "nonce", "gateway_url", "gateway_token", "ui_token", "data_directory"):
            payload = json.loads(VALID_LINE)
            payload.pop(field_name)
            with self.subTest(field=field_name):
                with self.assertRaises(BootstrapError):
                    from_mapping(payload)

    def test_empty_field_is_rejected(self) -> None:
        payload = json.loads(VALID_LINE)
        payload["ui_token"] = "   "
        with self.assertRaises(BootstrapError):
            from_mapping(payload)

    def test_non_http_gateway_url_is_rejected(self) -> None:
        payload = json.loads(VALID_LINE)
        payload["gateway_url"] = "file:///etc/passwd"
        with self.assertRaises(BootstrapError):
            from_mapping(payload)

    def test_invalid_json_is_rejected(self) -> None:
        with self.assertRaises(BootstrapError):
            parse_line("{not json")

    def test_non_object_json_is_rejected(self) -> None:
        with self.assertRaises(BootstrapError):
            parse_line("[1, 2, 3]")

    def test_empty_line_is_rejected(self) -> None:
        with self.assertRaises(BootstrapError):
            parse_line("   \n")

    def test_read_from_bytes_stream(self) -> None:
        bootstrap = read_from_stream(io.BytesIO((VALID_LINE + "\n").encode("utf-8")))
        self.assertEqual(bootstrap.nonce, "a" * 64)

    def test_read_from_text_stream(self) -> None:
        bootstrap = read_from_stream(io.StringIO(VALID_LINE + "\n"))
        self.assertEqual(bootstrap.instance_id, "inst-test-1")

    def test_closed_stream_is_rejected(self) -> None:
        with self.assertRaises(BootstrapError):
            read_from_stream(io.BytesIO(b""))

    def test_non_utf8_is_rejected(self) -> None:
        with self.assertRaises(BootstrapError):
            read_from_stream(io.BytesIO(b"\xff\xfe\xfd\n"))

    def test_oversized_line_is_rejected(self) -> None:
        with self.assertRaises(BootstrapError):
            read_from_stream(io.BytesIO(b"x" * 70000 + b"\n"))


class ReadyRecordTests(unittest.TestCase):
    """The ready line is the only thing this process is allowed to put on stdout."""

    def test_emit_ready_writes_one_line(self) -> None:
        buffer = io.StringIO()
        emit_ready(buffer, "nonce-1", 18423, AGENT_VERSION)
        output = buffer.getvalue()
        self.assertTrue(output.endswith("\n"))
        self.assertEqual(output.count("\n"), 1)
        record = json.loads(output)
        self.assertEqual(record["type"], "ready")
        self.assertEqual(record["protocol"], PROTOCOL)
        self.assertEqual(record["nonce"], "nonce-1")
        self.assertEqual(record["port"], 18423)
        self.assertEqual(record["agent_version"], AGENT_VERSION)

    def test_ready_record_has_no_tokens(self) -> None:
        buffer = io.StringIO()
        emit_ready(buffer, "nonce-1", 18423, AGENT_VERSION)
        self.assertNotIn("tok-", buffer.getvalue())

    def test_selftest_modes_are_documented(self) -> None:
        self.assertEqual(
            set(SELFTEST_MODES),
            {
                "none",
                "exit-before-ready",
                "exit-after-ready",
                "wrong-nonce",
                "wrong-protocol",
                "hang",
                "no-ready",
                # AD-02：合法 ready + 坏 health，用于验证宿主的就绪门控。
                "bad-health",
                "bad-health-protocol",
            },
        )

    def test_wrong_nonce_record_differs(self) -> None:
        record = _selftest_ready_record("wrong-nonce", "real-nonce", 1234, AGENT_VERSION)
        self.assertNotEqual(record["nonce"], "real-nonce")

    def test_wrong_protocol_record_differs(self) -> None:
        record = _selftest_ready_record("wrong-protocol", "n", 1234, AGENT_VERSION)
        self.assertNotEqual(record["protocol"], PROTOCOL)


class ParserTests(unittest.TestCase):
    """CLI flags are part of the host contract and must keep their names."""

    def test_defaults(self) -> None:
        args = build_parser().parse_args([])
        self.assertFalse(args.bootstrap_stdin)
        self.assertEqual(args.port, 0)
        self.assertIsNone(args.agent_version)
        self.assertEqual(args.selftest_fail, "none")
        self.assertEqual(args.selftest_ready_delay_ms, 0)

    def test_flags_are_parsed(self) -> None:
        args = build_parser().parse_args(
            [
                "--bootstrap-stdin",
                "--port",
                "18423",
                "--agent-version",
                "9.9.9",
                "--selftest-fail",
                "wrong-nonce",
                "--selftest-ready-delay-ms",
                "250",
            ]
        )
        self.assertTrue(args.bootstrap_stdin)
        self.assertEqual(args.port, 18423)
        self.assertEqual(args.agent_version, "9.9.9")
        self.assertEqual(args.selftest_fail, "wrong-nonce")
        self.assertEqual(args.selftest_ready_delay_ms, 250)

    def test_unknown_selftest_mode_is_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            build_parser().parse_args(["--selftest-fail", "explode"])


class RedactionTests(unittest.TestCase):
    """Once the tokens are known, no log record may still contain them."""

    def test_redaction_filter_rewrites_message(self) -> None:
        import logging

        record = logging.LogRecord(
            "test", logging.INFO, "x", 1, "token=%s", ("tok-ui-xyz",), None
        )
        RedactionFilter(("tok-ui-xyz",)).filter(record)
        self.assertNotIn("tok-ui-xyz", record.getMessage())

    def test_redaction_ignores_empty_secret(self) -> None:
        import logging

        record = logging.LogRecord("test", logging.INFO, "x", 1, "hello", (), None)
        RedactionFilter(("",)).filter(record)
        self.assertEqual(record.getMessage(), "hello")

    def test_bootstrap_object_is_immutable(self) -> None:
        bootstrap = Bootstrap(
            instance_id="i",
            nonce="n",
            gateway_url="http://127.0.0.1:1",
            gateway_token="g",
            ui_token="u",
            data_directory="d",
        )
        with self.assertRaises(Exception):
            bootstrap.nonce = "other"  # type: ignore[misc]


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
