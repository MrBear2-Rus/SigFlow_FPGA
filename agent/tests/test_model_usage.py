"""Usage accounting and balance normalization for the optional model channel."""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from sigflow_edu_agent.model_client import ModelClient  # noqa: E402
from sigflow_edu_agent.usage import UsageStore  # noqa: E402
from tests.test_model_client import API_KEY, BOOTSTRAP_TOKEN, _Endpoint, model_env  # noqa: E402

DEEPSEEK_URL = "https://api.deepseek.com"


def completion(text: str = "这是一段引导式讲解。", usage: bool = True) -> bytes:
    payload = {"choices": [{"message": {"content": text}}]}
    if usage:
        payload["usage"] = {
            "prompt_tokens": 11,
            "completion_tokens": 7,
            "total_tokens": 18,
        }
    return json.dumps(payload, ensure_ascii=False).encode("utf-8")


def balance_body() -> bytes:
    return json.dumps(
        {
            "is_available": True,
            "balance_infos": [
                {
                    "currency": "CNY",
                    "total_balance": "12.34",
                    "granted_balance": "0.00",
                    "topped_up_balance": "12.34",
                }
            ],
        }
    ).encode("utf-8")


class UsageAccountingTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.store = UsageStore(self._temporary.name)

    def client(self, endpoint: _Endpoint, **kwargs: object) -> ModelClient:
        patcher = mock.patch("urllib.request.OpenerDirector.open", new=endpoint)
        patcher.start()
        self.addCleanup(patcher.stop)
        return ModelClient(secrets=(BOOTSTRAP_TOKEN,), usage=self.store, **kwargs)

    def summary(self) -> dict:
        return self.store.summarize(30)

    def test_successful_call_records_token_usage(self) -> None:
        endpoint = _Endpoint(body=completion())
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            answer = self.client(endpoint).complete("instruction", "evidence")
        self.assertTrue(answer.ok, answer.reason)
        totals = self.summary()["totals"]
        self.assertEqual(totals["calls"], 1)
        self.assertEqual(totals["failed_calls"], 0)
        self.assertEqual(totals["prompt_tokens"], 11)
        self.assertEqual(totals["completion_tokens"], 7)
        self.assertEqual(totals["total_tokens"], 18)
        self.assertEqual(totals["tokens_unavailable_calls"], 0)

    def test_missing_usage_is_counted_but_marked_unavailable(self) -> None:
        endpoint = _Endpoint(body=completion(usage=False))
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            answer = self.client(endpoint).complete("instruction", "evidence")
        self.assertTrue(answer.ok, answer.reason)
        totals = self.summary()["totals"]
        self.assertEqual(totals["calls"], 1)
        self.assertEqual(totals["tokens_unavailable_calls"], 1)
        self.assertEqual(totals["total_tokens"], 0)

    def test_failed_endpoint_call_is_recorded(self) -> None:
        endpoint = _Endpoint(error=urllib.error.URLError("down"))
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            answer = self.client(endpoint).complete("instruction", "evidence")
        self.assertFalse(answer.ok)
        totals = self.summary()["totals"]
        self.assertEqual(totals["calls"], 1)
        self.assertEqual(totals["failed_calls"], 1)
        self.assertEqual(totals["tokens_unavailable_calls"], 1)

    def test_refusal_before_request_is_not_recorded(self) -> None:
        endpoint = _Endpoint(body=completion())
        with model_env():
            answer = self.client(endpoint).complete("instruction", "evidence")
        self.assertFalse(answer.ok)
        self.assertEqual(endpoint.requests, [])
        self.assertEqual(self.summary()["totals"]["calls"], 0)

    def test_usage_log_never_contains_prompt_text(self) -> None:
        endpoint = _Endpoint(body=completion())
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            self.client(endpoint).complete("SECRET-INSTRUCTION", "SECRET-EVIDENCE")
        content = (Path(self._temporary.name) / "usage" / "usage.jsonl").read_text(encoding="utf-8")
        self.assertNotIn("SECRET-INSTRUCTION", content)
        self.assertNotIn("SECRET-EVIDENCE", content)
        self.assertNotIn(API_KEY, content)


class BalanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.endpoint = _Endpoint(body=balance_body())
        patcher = mock.patch("urllib.request.OpenerDirector.open", new=self.endpoint)
        patcher.start()
        self.addCleanup(patcher.stop)

    def client(self) -> ModelClient:
        return ModelClient(secrets=(BOOTSTRAP_TOKEN,))

    def test_success_is_normalized_and_never_echoes_the_key(self) -> None:
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            result = self.client().fetch_balance()
        self.assertTrue(result["available"])
        self.assertEqual(result["schema"], "edu.model.balance.v1")
        self.assertEqual(result["currency"], "CNY")
        self.assertEqual(result["total_balance"], "12.34")
        self.assertEqual(result["granted_balance"], "0.00")
        self.assertEqual(result["topped_up_balance"], "12.34")
        self.assertNotIn(API_KEY, json.dumps(result))
        self.assertTrue(self.endpoint.requests[0].get_header("Authorization").startswith("Bearer "))

    def test_prefers_cny_when_multiple_currencies_exist(self) -> None:
        body = {
            "is_available": True,
            "balance_infos": [
                {
                    "currency": "USD",
                    "total_balance": "1.00",
                    "granted_balance": "0.00",
                    "topped_up_balance": "1.00",
                },
                {
                    "currency": "CNY",
                    "total_balance": "7.00",
                    "granted_balance": "0.00",
                    "topped_up_balance": "7.00",
                },
            ],
        }
        self.endpoint.body = json.dumps(body).encode("utf-8")
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            result = self.client().fetch_balance()
        self.assertEqual(result["currency"], "CNY")
        self.assertEqual(result["total_balance"], "7.00")

    def test_unconfigured_model_makes_no_request(self) -> None:
        with model_env():
            result = self.client().fetch_balance()
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "model_unconfigured")
        self.assertEqual(self.endpoint.requests, [])

    def test_unauthorized_maps_to_fixed_reason(self) -> None:
        self.endpoint.error = urllib.error.HTTPError(
            DEEPSEEK_URL + "/user/balance", 401, "unauthorized", {}, io.BytesIO()
        )
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            result = self.client().fetch_balance()
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "balance_unauthorized")

    def test_timeout_maps_to_fixed_reason(self) -> None:
        self.endpoint.error = urllib.error.URLError(TimeoutError("timed out"))
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            result = self.client().fetch_balance()
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "balance_timeout")

    def test_unavailable_account_maps_to_fixed_reason(self) -> None:
        self.endpoint.body = json.dumps(
            {"is_available": False, "balance_infos": []}
        ).encode("utf-8")
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            result = self.client().fetch_balance()
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "balance_unavailable")

    def test_invalid_body_maps_to_fixed_reason(self) -> None:
        self.endpoint.body = b"not json"
        with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
            result = self.client().fetch_balance()
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "balance_invalid_response")

    def test_non_deepseek_provider_is_unsupported(self) -> None:
        self.endpoint.error = urllib.error.URLError("must not be called")
        with model_env(api_key=API_KEY, base_url="https://example.com/v1", name="m"):
            result = self.client().fetch_balance()
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "balance_unsupported_provider")
        self.assertEqual(self.endpoint.requests, [])

    def test_balance_does_not_create_usage_records(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = UsageStore(directory)
            with model_env(api_key=API_KEY, base_url=DEEPSEEK_URL, name="deepseek-chat"):
                model = ModelClient(usage=store)
                model.fetch_balance()
            self.assertEqual(store.summarize(30)["totals"]["calls"], 0)


if __name__ == "__main__":
    unittest.main()
