"""T10 / §6.8 tests: the optional model channel degrades safely.

The model is an *optional enhancement*.  These tests pin the two properties the
rest of the service depends on:

1. with no key (or an unsafe endpoint) nothing is sent anywhere, and the run
   degrades to the deterministic rule explanation;
2. model text is never rendered as a card when it is empty, oversized, carries
   a local path or a credential, or contains code.

The endpoint is stubbed by patching ``OpenerDirector.open``: the real client
only dials credential-free HTTPS, so a loopback test server cannot stand in for
it.
"""

from __future__ import annotations

import contextlib
import json
import os
import sys
import unittest
import urllib.error
from typing import Any, Iterator, Optional
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))

from sigflow_edu_agent.cards import Selection  # noqa: E402
from sigflow_edu_agent.dispatch import (  # noqa: E402
    RunDispatcher,
    RunRequest,
    safe_teaching_answer,
)
from sigflow_edu_agent.model_client import (  # noqa: E402
    API_KEY_ENV,
    BASE_URL_ENV,
    DEFAULT_BASE_URL,
    DEFAULT_MODEL,
    MAX_COMPLETION_BYTES,
    MODEL_ENV,
    REQUEST_TIMEOUT_SECONDS,
    ModelAnswer,
    ModelClient,
    model_status,
)
from sigflow_edu_agent.util import contains_credential  # noqa: E402

API_KEY = "sk-test-0123456789abcdefghijklmn"
BOOTSTRAP_TOKEN = "ui-token-abcdef"


@contextlib.contextmanager
def model_env(
    api_key: Optional[str] = None,
    base_url: Optional[str] = None,
    name: Optional[str] = None,
) -> Iterator[None]:
    """Set (or clear) exactly the three model environment variables."""
    wanted = ((API_KEY_ENV, api_key), (BASE_URL_ENV, base_url), (MODEL_ENV, name))
    with mock.patch.dict(os.environ, {}, clear=False):
        for variable, value in wanted:
            if value is None:
                os.environ.pop(variable, None)
            else:
                os.environ[variable] = value
        yield


def completion_body(text: str) -> bytes:
    """Build an OpenAI-compatible completion body carrying *text*."""
    return json.dumps({"choices": [{"message": {"content": text}}]}).encode("utf-8")


def body_of_exactly(size: int, filler: str = "x") -> bytes:
    """Build a valid completion body whose serialised length is exactly *size*."""
    skeleton = completion_body("")
    padding = size - len(skeleton)
    assert padding >= 0, "the requested body is smaller than the skeleton"
    return completion_body(filler * padding)


class _FakeResponse:
    """Minimal stand-in for the object ``OpenerDirector.open`` returns."""

    def __init__(self, body: bytes, status: int = 200) -> None:
        self._body = body
        self.status = status

    def __enter__(self) -> "_FakeResponse":
        return self

    def __exit__(self, *exc: object) -> None:
        return None

    def read(self, size: int = -1) -> bytes:
        return self._body if size < 0 else self._body[:size]


class _Endpoint:
    """Record every model request; reply with a canned body or raise."""

    def __init__(
        self,
        body: bytes = b"",
        status: int = 200,
        error: Optional[BaseException] = None,
    ) -> None:
        self.body = body
        self.status = status
        self.error = error
        self.requests: list[Any] = []
        self.timeouts: list[Any] = []

    def __call__(self, request: Any, *args: Any, **kwargs: Any) -> _FakeResponse:
        # ``mock.patch(..., new=<callable object>)`` does not bind the opener
        # instance, so the request arrives as the first positional argument.
        del args
        self.requests.append(request)
        self.timeouts.append(kwargs.get("timeout"))
        if self.error is not None:
            raise self.error
        return _FakeResponse(self.body, self.status)


class ModelTestCase(unittest.TestCase):
    """Shared plumbing: an endpoint stub plus a client bound to it."""

    def setUp(self) -> None:
        self.endpoint = _Endpoint(body=completion_body("这是一段引导式讲解。"))
        patcher = mock.patch("urllib.request.OpenerDirector.open", new=self.endpoint)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.env = model_env(api_key=API_KEY)
        self.env.__enter__()
        self.addCleanup(self.env.__exit__, None, None, None)

    def client(self, **kwargs: Any) -> ModelClient:
        return ModelClient(secrets=(BOOTSTRAP_TOKEN,), **kwargs)


class ModelAvailabilityTests(ModelTestCase):
    """No key (or an untrusted endpoint) means no model, never a guess."""

    def test_no_key_means_unavailable_and_no_http_call(self) -> None:
        with model_env():
            status = model_status()
            self.assertFalse(status.available)
            self.assertIsNone(status.provider)
            self.assertIsNone(status.model)
            self.assertIn("规则讲解", status.reason)
            answer = ModelClient().complete("instruction", "evidence")
        self.assertFalse(answer.ok)
        self.assertEqual(answer.text, "")
        self.assertIn("规则讲解", answer.reason)
        self.assertEqual(self.endpoint.requests, [], "no key must not dial out")

    def test_defaults_are_used_when_only_the_key_is_set(self) -> None:
        status = model_status()
        self.assertTrue(status.available)
        self.assertIn("openai.com", status.provider or "")
        self.assertEqual(status.model, DEFAULT_MODEL)
        self.assertTrue(self.client().complete("i", "e").ok)
        payload = json.loads(self.endpoint.requests[0].data.decode("utf-8"))
        self.assertEqual(payload["model"], DEFAULT_MODEL)
        self.assertTrue(DEFAULT_BASE_URL.startswith("https://"))

    def test_environment_overrides_the_endpoint_and_model(self) -> None:
        with model_env(api_key=API_KEY, base_url="https://llm.example.edu/v1", name="edu-model-7b"):
            status = model_status()
            self.assertTrue(status.available)
            self.assertEqual(status.provider, "llm.example.edu")
            self.assertEqual(status.model, "edu-model-7b")
            self.assertTrue(self.client().complete("i", "e").ok)
        request = self.endpoint.requests[0]
        self.assertEqual(request.full_url, "https://llm.example.edu/v1/chat/completions")
        self.assertEqual(
            json.loads(request.data.decode("utf-8"))["model"], "edu-model-7b"
        )

    def test_non_https_endpoint_is_refused(self) -> None:
        for base_url in ("http://127.0.0.1:8080/v1", "ftp://host/v1", "file:///etc/v1"):
            with self.subTest(base_url=base_url):
                with model_env(api_key=API_KEY, base_url=base_url):
                    self.assertFalse(model_status().available)
                    answer = ModelClient().complete("i", "e")
                self.assertFalse(answer.ok)
        self.assertEqual(self.endpoint.requests, [])

    def test_endpoint_with_userinfo_or_query_is_refused(self) -> None:
        for base_url in (
            "https://user:pw@llm.example.edu/v1",
            "https://llm.example.edu/v1?key=1",
            "https://llm.example.edu/v1#frag",
            "https://",
        ):
            with self.subTest(base_url=base_url):
                with model_env(api_key=API_KEY, base_url=base_url):
                    self.assertFalse(model_status().available)
                    self.assertFalse(ModelClient().complete("i", "e").ok)
        self.assertEqual(self.endpoint.requests, [])


class ModelRequestShapeTests(ModelTestCase):
    """One non-streaming chat completion; the key only in the header."""

    def test_request_is_a_non_streaming_chat_completion(self) -> None:
        answer = self.client().complete("讲一下这个片段", "signal=q rule=RTL_LATCH_INCOMPLETE_BRANCH")
        self.assertTrue(answer.ok, answer.reason)
        request = self.endpoint.requests[0]
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.full_url, DEFAULT_BASE_URL + "/chat/completions")
        payload = json.loads(request.data.decode("utf-8"))
        self.assertIs(payload["stream"], False)
        self.assertEqual([message["role"] for message in payload["messages"]], ["system", "user"])
        self.assertIn("讲一下这个片段", payload["messages"][1]["content"])
        self.assertIn("RTL_LATCH_INCOMPLETE_BRANCH", payload["messages"][1]["content"])
        self.assertEqual(REQUEST_TIMEOUT_SECONDS, 30.0)
        self.assertEqual(self.endpoint.timeouts, [REQUEST_TIMEOUT_SECONDS])

    def test_api_key_travels_only_in_the_authorization_header(self) -> None:
        self.assertTrue(self.client().complete("i", "e").ok)
        request = self.endpoint.requests[0]
        self.assertEqual(request.get_header("Authorization"), "Bearer " + API_KEY)
        self.assertNotIn(API_KEY, request.full_url)
        self.assertNotIn(API_KEY.encode("utf-8"), request.data)

    def test_api_key_is_not_logged(self) -> None:
        self.endpoint.error = urllib.error.URLError("connection refused for " + API_KEY)
        with self.assertLogs("sigflow_edu_agent.model_client", level="WARNING") as logs:
            answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertNotIn(API_KEY, "\n".join(logs.output))


class ModelResponsePolicyTests(ModelTestCase):
    """Untrusted model output is refused entirely, never patched in place."""

    def test_local_path_in_the_answer_is_refused_entirely(self) -> None:
        for leaked in (
            "请检查 C:\\Users\\student\\top.v 的 if 分支。",
            "打开 /home/student/proj/rtl/top.v 看看。",
        ):
            with self.subTest(leaked=leaked):
                self.endpoint.body = completion_body(leaked)
                answer = self.client().complete("i", "e")
                self.assertFalse(answer.ok)
                self.assertEqual(answer.text, "", "a refused answer carries no text")
                self.assertIn("本机路径", answer.reason)

    def test_credential_in_the_answer_is_refused(self) -> None:
        for leaked in (
            API_KEY,
            "请把 sk-abcdefghijklmnopqrstuvwx 填进配置。",
            BOOTSTRAP_TOKEN,
            "api_key=abcdef123456",
        ):
            with self.subTest(leaked=leaked):
                self.endpoint.body = completion_body("这段讲解里出现了 " + leaked)
                answer = self.client().complete("i", "e")
                self.assertFalse(answer.ok)
                self.assertIn("敏感凭据", answer.reason)

    def test_response_at_the_budget_is_accepted(self) -> None:
        self.endpoint.body = body_of_exactly(MAX_COMPLETION_BYTES)
        answer = self.client().complete("i", "e")
        self.assertTrue(answer.ok, answer.reason)

    def test_response_over_the_budget_is_refused(self) -> None:
        self.endpoint.body = body_of_exactly(MAX_COMPLETION_BYTES + 1)
        answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertIn("预算", answer.reason)

    def test_streamed_response_is_refused(self) -> None:
        self.endpoint.body = b'data: {"choices":[{"delta":{"content":"hi"}}]}\n\n'
        answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertIn("流式", answer.reason)

    def test_non_json_response_is_refused(self) -> None:
        self.endpoint.body = b"<html>gateway timeout</html>"
        answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertIn("非 JSON", answer.reason)

    def test_missing_choices_is_refused(self) -> None:
        self.endpoint.body = b'{"choices": []}'
        answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertEqual(answer.text, "")

    def test_content_parts_are_joined(self) -> None:
        self.endpoint.body = json.dumps(
            {
                "choices": [
                    {
                        "message": {
                            "content": [
                                {"type": "text", "text": "第一段讲解。"},
                                {"type": "text", "text": "第二段讲解。"},
                            ]
                        }
                    }
                ]
            }
        ).encode("utf-8")
        answer = self.client().complete("i", "e")
        self.assertTrue(answer.ok, answer.reason)
        self.assertIn("第一段讲解。", answer.text)
        self.assertIn("第二段讲解。", answer.text)

    def test_unknown_content_part_is_refused(self) -> None:
        self.endpoint.body = json.dumps(
            {
                "choices": [
                    {"message": {"content": [{"type": "image_url", "image_url": "x"}]}}
                ]
            }
        ).encode("utf-8")
        answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertEqual(answer.text, "")

    def test_http_error_and_redirect_degrade(self) -> None:
        error = urllib.error.HTTPError(
            "https://api.openai.com/v1/chat/completions", 302, "redirect refused", {}, None
        )
        self.addCleanup(error.close)
        self.endpoint.error = error
        with self.assertLogs("sigflow_edu_agent.model_client", level="WARNING"):
            answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertEqual(answer.text, "")

    def test_401_degrades_without_echoing_the_key(self) -> None:
        error = urllib.error.HTTPError(
            "https://api.openai.com/v1/chat/completions",
            401,
            "unauthorized " + API_KEY,
            {},
            None,
        )
        self.addCleanup(error.close)
        self.endpoint.error = error
        with self.assertLogs("sigflow_edu_agent.model_client", level="WARNING") as logs:
            answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertEqual(answer.text, "")
        self.assertNotIn(API_KEY, answer.reason)
        self.assertNotIn(API_KEY, "\n".join(logs.output))

    def test_timeout_degrades(self) -> None:
        for error in (
            TimeoutError("timed out after 30s"),
            urllib.error.URLError(TimeoutError("timed out after 30s")),
        ):
            with self.subTest(error=type(error).__name__):
                self.endpoint.error = error
                with self.assertLogs("sigflow_edu_agent.model_client", level="WARNING") as logs:
                    answer = self.client().complete("i", "e")
                self.assertFalse(answer.ok)
                self.assertEqual(answer.text, "")
                self.assertIn("超时", answer.reason)
                self.assertIn("timed out", "\n".join(logs.output))

    def test_unreachable_endpoint_degrades(self) -> None:
        self.endpoint.error = urllib.error.URLError("connection refused")
        with self.assertLogs("sigflow_edu_agent.model_client", level="WARNING"):
            answer = self.client().complete("i", "e")
        self.assertFalse(answer.ok)
        self.assertIn("不可达", answer.reason)

    def test_caller_evidence_that_leaks_a_secret_is_not_sent(self) -> None:
        answer = self.client().complete("i", "see " + BOOTSTRAP_TOKEN)
        self.assertFalse(answer.ok)
        self.assertEqual(self.endpoint.requests, [], "the prompt never left the process")


class DegradedRunTests(unittest.TestCase):
    """T10 item 3: without a key the run is a rule run, and it says so."""

    LATCH_SNIPPET = "always @(*) begin\n  if (sel) y = a;\nend\n"

    def test_explain_without_a_key_degrades_to_rule_cards(self) -> None:
        with model_env():
            dispatcher = RunDispatcher(gateway=object(), model=ModelClient())  # type: ignore[arg-type]
            outcome = dispatcher.dispatch(
                RunRequest(
                    kind="explain",
                    level="L1",
                    selection=Selection(text=self.LATCH_SNIPPET),
                ),
                state_version="1",
            )
        self.assertEqual(outcome.state, "degraded")
        self.assertFalse(outcome.model_used)
        self.assertTrue(outcome.cards)
        self.assertTrue(all(card["source"] == "rule" for card in outcome.cards))
        skipped = [item for item in outcome.omitted if item["kind"] == "model_explanation"]
        self.assertEqual(len(skipped), 1, "the skipped model step must be recorded")
        self.assertIn("规则讲解", skipped[0]["reason"])

    def test_hint_without_a_key_degrades_to_rule_cards(self) -> None:
        with model_env():
            dispatcher = RunDispatcher(gateway=object(), model=ModelClient())  # type: ignore[arg-type]
            outcome = dispatcher.dispatch(
                RunRequest(
                    kind="hint",
                    level="L2",
                    selection=Selection(text=self.LATCH_SNIPPET),
                ),
                state_version="1",
            )
        self.assertFalse(outcome.model_used)
        self.assertTrue(all(card["source"] == "rule" for card in outcome.cards))
        self.assertTrue(any(item["kind"] == "model_hint" for item in outcome.omitted))


class SafeTeachingAnswerTests(unittest.TestCase):
    """T10 item 5: the last gate before model prose becomes a card."""

    def test_empty_text_degrades(self) -> None:
        for text in ("", "   \n  "):
            with self.subTest(text=text):
                answer = safe_teaching_answer(ModelAnswer(ok=True, text=text), "L1")
                self.assertFalse(answer.ok)
                self.assertEqual(answer.text, "")

    def test_the_limit_is_twelve_kib_of_utf8(self) -> None:
        allowed = safe_teaching_answer(ModelAnswer(ok=True, text="x" * (12 * 1024)), "L1")
        self.assertTrue(allowed.ok, allowed.reason)
        refused = safe_teaching_answer(ModelAnswer(ok=True, text="x" * (12 * 1024 + 1)), "L1")
        self.assertFalse(refused.ok)
        self.assertIn("长度预算", refused.reason)
        # 5000 Chinese characters are >12 KiB of UTF-8, even though the
        # character count looks small: the budget is bytes, not characters.
        chinese = safe_teaching_answer(ModelAnswer(ok=True, text="讲" * 5000), "L1")
        self.assertFalse(chinese.ok)

    def test_local_path_degrades(self) -> None:
        answer = safe_teaching_answer(
            ModelAnswer(ok=True, text="看 C:\\Users\\student\\top.v"), "L1"
        )
        self.assertFalse(answer.ok)
        self.assertIn("本机路径", answer.reason)
        self.assertEqual(answer.text, "")

    def test_credential_degrades(self) -> None:
        for text in ("sk-abcdefghijklmnopqrstuvwx", "api_key=abcdef123456"):
            with self.subTest(text=text):
                answer = safe_teaching_answer(ModelAnswer(ok=True, text=text), "L1")
                self.assertFalse(answer.ok)
                self.assertIn("敏感凭据", answer.reason)
        self.assertTrue(contains_credential("sk-abcdefghijklmnopqrstuvwx"))

    def test_code_markers_degrade_at_every_teaching_level(self) -> None:
        markers = (
            "修好后应该写成：\n```verilog\nmodule m; endmodule\n```",
            "把 endmodule 前面的逻辑改掉。",
            "在 always @(*) 里补一个 else。",
            "改成 always_comb 块。",
            "改成 always_ff 块。",
        )
        for level in ("L1", "L2", "L3", "L4"):
            for marker in markers:
                with self.subTest(level=level, marker=marker[:24]):
                    answer = safe_teaching_answer(ModelAnswer(ok=True, text=marker), level)
                    self.assertFalse(answer.ok)
                    self.assertIn("教学级别", answer.reason)
                    self.assertEqual(answer.text, "")

    def test_plain_prose_is_accepted_and_stripped(self) -> None:
        answer = safe_teaching_answer(
            ModelAnswer(ok=True, text="  这段组合逻辑在条件不成立时没有赋值。  "), "L1"
        )
        self.assertTrue(answer.ok)
        self.assertEqual(answer.text, "这段组合逻辑在条件不成立时没有赋值。")

    def test_already_failed_answers_pass_through(self) -> None:
        original = ModelAnswer(ok=False, reason="模型不可用")
        self.assertIs(safe_teaching_answer(original, "L1"), original)


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
