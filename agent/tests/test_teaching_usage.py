"""Usage accounting for the optional U2 teaching model provider."""

from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
AGENT_DIR = REPO_ROOT / "agent"
for path in (str(AGENT_DIR / "src"), str(REPO_ROOT / "sigflowAgent")):
    if path not in sys.path:
        sys.path.insert(0, path)

from sigflow_teaching_core.deepseek import DeepSeekConfig, DeepSeekProvider  # noqa: E402
from sigflow_teaching_core.domain import CardContext, Evidence, ProviderError  # noqa: E402


def context() -> CardContext:
    return CardContext(
        "为什么有锁存器？",
        1,
        Evidence("ev1", "p", "r1", "job1", "latch", True, "示例报告", True),
    )


def card_body(usage=True) -> bytes:
    card = {
        "level": 1,
        "hint": "报告观察到存储行为。",
        "question": "输出需要记忆吗？",
        "evidence_ids": ["ev1"],
    }
    payload = {
        "choices": [
            {
                "finish_reason": "stop",
                "message": {"role": "assistant", "content": json.dumps(card)},
            }
        ]
    }
    if usage:
        payload["usage"] = {
            "prompt_tokens": 10,
            "completion_tokens": 5,
            "total_tokens": 15,
        }
    return json.dumps(payload).encode("utf-8")


class _Reply:
    def __init__(self, body: bytes, status: int = 200) -> None:
        self._body = body
        self.status = status

    def read(self, size: int) -> bytes:
        return self._body[:size]


class _Connection:
    def __init__(self, reply: _Reply) -> None:
        self._reply = reply
        self.closed = False

    def request(self, *args, **kwargs) -> None:
        return None

    def getresponse(self) -> _Reply:
        return self._reply

    def close(self) -> None:
        self.closed = True


class TeachingProviderUsageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.records: list[dict] = []

    def provider(self) -> DeepSeekProvider:
        config = DeepSeekConfig(api_key="sk-test-0123456789abcdef", model="deepseek-chat")
        return DeepSeekProvider(
            config,
            usage_recorder=lambda **kwargs: self.records.append(kwargs),
        )

    def test_successful_call_reports_tokens(self) -> None:
        connection = _Connection(_Reply(card_body()))
        with mock.patch(
            "sigflow_teaching_core.deepseek.http.client.HTTPSConnection",
            return_value=connection,
        ):
            provider = self.provider()
            card = provider.generate(context())
        self.assertEqual(card["level"], 1)
        self.assertTrue(connection.closed)
        self.assertEqual(len(self.records), 1)
        self.assertEqual(self.records[0]["model"], "deepseek-chat")
        self.assertTrue(self.records[0]["ok"])
        self.assertEqual(self.records[0]["usage"]["total_tokens"], 15)

    def test_missing_usage_is_still_a_successful_call(self) -> None:
        connection = _Connection(_Reply(card_body(usage=False)))
        with mock.patch(
            "sigflow_teaching_core.deepseek.http.client.HTTPSConnection",
            return_value=connection,
        ):
            self.provider().generate(context())
        self.assertEqual(len(self.records), 1)
        self.assertTrue(self.records[0]["ok"])
        self.assertIsNone(self.records[0]["usage"])

    def test_http_failure_is_recorded_and_raised(self) -> None:
        connection = _Connection(_Reply(b"{}", status=401))
        with mock.patch(
            "sigflow_teaching_core.deepseek.http.client.HTTPSConnection",
            return_value=connection,
        ):
            with self.assertRaises(ProviderError):
                self.provider().generate(context())
        self.assertEqual(len(self.records), 1)
        self.assertFalse(self.records[0]["ok"])
        self.assertIsNone(self.records[0]["usage"])

    def test_invalid_output_is_recorded_and_raised(self) -> None:
        connection = _Connection(_Reply(b"not-json"))
        with mock.patch(
            "sigflow_teaching_core.deepseek.http.client.HTTPSConnection",
            return_value=connection,
        ):
            with self.assertRaises(ProviderError):
                self.provider().generate(context())
        self.assertEqual(len(self.records), 1)
        self.assertFalse(self.records[0]["ok"])

    def test_recorder_failure_never_masks_the_model_call(self) -> None:
        connection = _Connection(_Reply(card_body()))

        def broken(**kwargs):
            raise RuntimeError("accounting unavailable")

        config = DeepSeekConfig(api_key="sk-test-0123456789abcdef", model="deepseek-chat")
        with mock.patch(
            "sigflow_teaching_core.deepseek.http.client.HTTPSConnection",
            return_value=connection,
        ):
            card = DeepSeekProvider(config, usage_recorder=broken).generate(context())
        self.assertEqual(card["level"], 1)


if __name__ == "__main__":
    unittest.main()
