"""HTTP tests for the read-only usage and balance routes."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from unittest import mock

AGENT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(AGENT_DIR, "src")
for path in (AGENT_DIR, SRC_DIR):
    if path not in sys.path:
        sys.path.insert(0, path)

from sigflow_edu_agent.usage import UsageStore  # noqa: E402
from tests.test_http_api import SidecarFixture, UI_TOKEN, _data  # noqa: E402


class UsageRouteTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.store = UsageStore(self._temporary.name)
        self.fixture = SidecarFixture(usage=self.store)
        self.addCleanup(self.fixture.close)

    def test_usage_requires_the_ui_token(self) -> None:
        response = self.fixture.request("GET", "/api/v1/usage", token=None)
        self.assertEqual(response.status, 401)
        self.assertEqual(response.payload["error"]["code"], "UNAUTHENTICATED")

    def test_usage_returns_empty_statistics(self) -> None:
        response = self.fixture.request("GET", "/api/v1/usage")
        self.assertEqual(response.status, 200)
        data = _data(response)
        self.assertEqual(data["schema"], "edu.usage.v1")
        self.assertIsNone(data["since"])
        self.assertEqual(data["days"], 30)
        self.assertEqual(data["skipped"], 0)
        self.assertEqual(data["daily"], [])
        self.assertEqual(
            data["totals"],
            {
                "calls": 0,
                "failed_calls": 0,
                "prompt_tokens": 0,
                "completion_tokens": 0,
                "total_tokens": 0,
                "tokens_unavailable_calls": 0,
            },
        )

    def test_usage_aggregates_recorded_calls(self) -> None:
        self.store.record(
            model="deepseek-chat",
            ok=True,
            usage={"prompt_tokens": 4, "completion_tokens": 6, "total_tokens": 10},
        )
        data = _data(self.fixture.request("GET", "/api/v1/usage"))
        self.assertEqual(data["totals"]["calls"], 1)
        self.assertEqual(data["totals"]["total_tokens"], 10)
        self.assertEqual(len(data["daily"]), 1)
        self.assertEqual(data["daily"][0]["calls"], 1)

    def test_usage_accepts_an_explicit_window(self) -> None:
        data = _data(self.fixture.request("GET", "/api/v1/usage?days=7"))
        self.assertEqual(data["days"], 7)

    def test_usage_rejects_invalid_windows(self) -> None:
        for value in ("0", "91", "abc", "1.5", "-1", "7&days=8"):
            with self.subTest(value=value):
                response = self.fixture.request("GET", "/api/v1/usage?days=" + value)
                self.assertEqual(response.status, 400, response.payload)
                self.assertEqual(response.payload["error"]["code"], "INVALID_ARGUMENT")

    def test_usage_never_includes_prompt_content(self) -> None:
        self.store.record(
            model="deepseek-chat",
            ok=True,
            usage={"prompt_tokens": 1, "completion_tokens": 2, "total_tokens": 3},
        )
        body = repr(self.fixture.request("GET", "/api/v1/usage").payload)
        self.assertNotIn("SECRET", body)
        self.assertNotIn(UI_TOKEN, body)


class BalanceRouteTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = SidecarFixture()
        self.addCleanup(self.fixture.close)

    def test_balance_requires_the_ui_token(self) -> None:
        response = self.fixture.request("GET", "/api/v1/balance", token=None)
        self.assertEqual(response.status, 401)
        self.assertEqual(response.payload["error"]["code"], "UNAUTHENTICATED")

    def test_unconfigured_model_reports_a_fixed_reason(self) -> None:
        data = _data(self.fixture.request("GET", "/api/v1/balance"))
        self.assertFalse(data["available"])
        self.assertEqual(data["reason"], "model_unconfigured")
        self.assertEqual(data["schema"], "edu.model.balance.v1")

    def test_success_payload_is_normalized(self) -> None:
        canned = {
            "schema": "edu.model.balance.v1",
            "available": True,
            "checked_at": "2026-11-17T08:00:00Z",
            "currency": "CNY",
            "total_balance": "12.34",
            "granted_balance": "0.00",
            "topped_up_balance": "12.34",
        }
        with mock.patch.object(
            self.fixture.state.model, "fetch_balance", return_value=canned
        ):
            data = _data(self.fixture.request("GET", "/api/v1/balance"))
        self.assertEqual(data, canned)


if __name__ == "__main__":
    unittest.main()
