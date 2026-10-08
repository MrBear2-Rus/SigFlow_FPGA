"""Optional model access for the SigFlow Edu Agent.

The service is fully functional without a model key.  When no key is
configured the dispatcher degrades to the deterministic rule engine and both
``/health`` and ``/capabilities`` report ``model.available = false``.  A model
answer is never fabricated: if the HTTP call fails, the run degrades.

Only the Python standard library is used, and the API key never appears in a
URL, a log line or a response.
"""

from __future__ import annotations

import json
import logging
import os
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Mapping, Optional

from .util import contains_absolute_path, sanitize_message

LOGGER = logging.getLogger(__name__)

API_KEY_ENV: str = "SIGFLOW_EDU_MODEL_API_KEY"
BASE_URL_ENV: str = "SIGFLOW_EDU_MODEL_BASE_URL"
MODEL_ENV: str = "SIGFLOW_EDU_MODEL_NAME"

DEFAULT_BASE_URL: str = "https://api.openai.com/v1"
DEFAULT_MODEL: str = "gpt-4o-mini"
REQUEST_TIMEOUT_SECONDS: float = 30.0
MAX_COMPLETION_BYTES: int = 256 * 1024

SYSTEM_PROMPT: str = (
    "你是 SigFlow FPGA 教学助手。只用中文回答，只做引导式讲解，"
    "L1–L3 绝不给出完整可直接替换的修正代码，也不要编造没有提供的证据。"
)


@dataclass(frozen=True)
class ModelStatus:
    """Availability of the optional language model."""

    available: bool
    provider: Optional[str]
    reason: str
    model: Optional[str] = None

    def to_wire(self) -> dict[str, Any]:
        """Render the ``model`` object used by /health and /capabilities."""
        return {
            "available": self.available,
            "provider": self.provider,
            "reason": self.reason,
        }


MODEL_DISABLED_REASON: str = (
    "未配置模型 Key（SIGFLOW_EDU_MODEL_API_KEY），已降级为确定性规则讲解。"
)


def model_status() -> ModelStatus:
    """Report whether an OpenAI-compatible model endpoint is configured."""
    api_key = os.environ.get(API_KEY_ENV, "").strip()
    if not api_key:
        return ModelStatus(
            available=False,
            provider=None,
            reason=MODEL_DISABLED_REASON,
            model=None,
        )
    base_url = os.environ.get(BASE_URL_ENV, "").strip() or DEFAULT_BASE_URL
    model = os.environ.get(MODEL_ENV, "").strip() or DEFAULT_MODEL
    return ModelStatus(
        available=True,
        provider=base_url,
        reason="模型端点已配置，可用于教学讲解。",
        model=model,
    )


@dataclass(frozen=True)
class ModelAnswer:
    """One completed (or refused) model call."""

    ok: bool
    text: str = ""
    reason: str = ""


class ModelClient:
    """Minimal OpenAI-compatible chat-completions client over urllib."""

    def __init__(
        self,
        status: Optional[ModelStatus] = None,
        *,
        secrets: tuple[str, ...] = (),
    ) -> None:
        self._status = status or model_status()
        self._api_key = os.environ.get(API_KEY_ENV, "").strip()
        self._base_url = (
            os.environ.get(BASE_URL_ENV, "").strip() or DEFAULT_BASE_URL
        ).rstrip("/")
        self._model = os.environ.get(MODEL_ENV, "").strip() or DEFAULT_MODEL
        # The sidecar bootstrap secrets are never sent to the model.  Retain
        # them only as an output guard in case an endpoint echoes or invents a
        # token-like value that happens to match this process's credentials.
        self._secrets = tuple(
            secret for secret in (self._api_key, *secrets) if secret
        )

    @property
    def status(self) -> ModelStatus:
        """Current model availability, as reported on the wire."""
        return self._status

    def complete(self, instruction: str, evidence: str) -> ModelAnswer:
        """Ask the model for a teaching paragraph, or refuse cleanly.

        Returns ``ok=False`` with a reason when no key is configured, when the
        endpoint fails, or when the answer would leak a local path.
        """
        if not self._status.available or not self._api_key:
            return ModelAnswer(ok=False, reason=MODEL_DISABLED_REASON)
        payload = {
            "model": self._model,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": instruction + "\n\n" + evidence},
            ],
            "temperature": 0.2,
            "stream": False,
        }
        request = urllib.request.Request(
            self._base_url + "/chat/completions",
            data=json.dumps(payload).encode("utf-8"),
            headers={
                "Authorization": "Bearer " + self._api_key,
                "Content-Type": "application/json",
                "Accept": "application/json",
            },
            method="POST",
        )
        secrets = (self._api_key,)
        try:
            with urllib.request.urlopen(
                request, timeout=REQUEST_TIMEOUT_SECONDS
            ) as response:
                body = response.read(MAX_COMPLETION_BYTES + 1)
        except urllib.error.HTTPError as exc:
            LOGGER.warning(
                "model endpoint returned status %d", int(getattr(exc, "code", 0) or 0)
            )
            return ModelAnswer(ok=False, reason="模型端点拒绝了本次请求，已降级为规则讲解。")
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            LOGGER.warning("model endpoint unreachable: %s", sanitize_message(str(exc), secrets))
            return ModelAnswer(ok=False, reason="模型端点不可达，已降级为规则讲解。")
        if len(body) > MAX_COMPLETION_BYTES:
            return ModelAnswer(ok=False, reason="模型响应超出预算，已降级为规则讲解。")
        try:
            decoded = json.loads(body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return ModelAnswer(ok=False, reason="模型返回了非 JSON 响应，已降级为规则讲解。")
        text = _extract_text(decoded)
        if not text:
            return ModelAnswer(ok=False, reason="模型没有返回可用文本，已降级为规则讲解。")
        if contains_absolute_path(text):
            return ModelAnswer(
                ok=False,
                reason="模型返回包含本机路径，已降级为规则讲解。",
            )
        if any(secret in text for secret in self._secrets):
            return ModelAnswer(
                ok=False,
                reason="模型返回包含敏感凭据，已降级为规则讲解。",
            )
        return ModelAnswer(ok=True, text=text)


def _extract_text(payload: Any) -> str:
    """Pull the first assistant message out of an OpenAI-compatible body."""
    if not isinstance(payload, Mapping):
        return ""
    choices = payload.get("choices")
    if not isinstance(choices, list) or not choices:
        return ""
    first = choices[0]
    if not isinstance(first, Mapping):
        return ""
    message = first.get("message")
    if isinstance(message, Mapping):
        content = message.get("content")
        if isinstance(content, str):
            return content.strip()
    text = first.get("text")
    return text.strip() if isinstance(text, str) else ""
