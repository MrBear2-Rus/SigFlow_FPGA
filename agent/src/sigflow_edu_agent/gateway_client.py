"""Read-only HTTP client for the SigFlow EDA Gateway.

This is the sidecar's *only* outbound channel.  It speaks HTTP to the loopback
Gateway that handed us ``gateway_url``/``gateway_token`` in the bootstrap
record; it never reads a file, never runs a command and never accepts a
caller-supplied URL.

The Gateway normalizes job reports into ``edu.jobreport.v1`` and wraps them in
the same ``{schema_version, request_id, trace_id, data}`` envelope it uses for
every other route, so the client unwraps ``data`` when it is present.
"""

from __future__ import annotations

import json
import logging
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Mapping, Optional

from .util import sanitize_message

LOGGER = logging.getLogger(__name__)

DEFAULT_TIMEOUT_SECONDS: float = 8.0
"""Per-request timeout; the UI is waiting on the other end of this call."""

MAX_REPORT_BYTES: int = 2 * 1024 * 1024
"""Refuse to buffer an unbounded Gateway response into model context."""


@dataclass(frozen=True)
class GatewayOutcome:
    """Result of one Gateway call.

    Exactly one of ``report`` (success) or ``code`` (failure) is set.
    ``code`` is always one of the frozen agent error codes.
    """

    ok: bool
    report: Optional[Mapping[str, Any]] = None
    code: Optional[str] = None
    message: str = ""


class GatewayClient:
    """Minimal urllib-based client bound to one Gateway base URL and token."""

    def __init__(
        self,
        base_url: str,
        token: str,
        timeout: float = DEFAULT_TIMEOUT_SECONDS,
        secrets: tuple[str, ...] = (),
    ) -> None:
        self._base_url = base_url.rstrip("/")
        self._token = token
        self._timeout = timeout
        self._secrets = tuple(secret for secret in secrets if secret)

    @property
    def base_url(self) -> str:
        """Gateway base URL as supplied by the bootstrap record."""
        return self._base_url

    def _headers(self) -> dict[str, str]:
        """Build the request headers, keeping the token out of the URL."""
        return {
            "Authorization": "Bearer " + self._token,
            "Accept": "application/json",
            "Host": _loopback_host(self._base_url),
        }

    def _get_json(self, path: str) -> GatewayOutcome:
        """Perform one bounded GET and classify the outcome."""
        url = self._base_url + path
        request = urllib.request.Request(url, headers=self._headers(), method="GET")
        try:
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                status = int(getattr(response, "status", 0) or 0)
                body = response.read(MAX_REPORT_BYTES + 1)
        except urllib.error.HTTPError as exc:
            return self._http_error_outcome(exc)
        except urllib.error.URLError as exc:
            LOGGER.warning("gateway request failed: %s", _safe(str(exc.reason), self._secrets))
            return GatewayOutcome(
                ok=False,
                code="GATEWAY_UNAVAILABLE",
                message="EDA Gateway is unreachable",
            )
        except (TimeoutError, OSError) as exc:
            LOGGER.warning("gateway request failed: %s", _safe(str(exc), self._secrets))
            return GatewayOutcome(
                ok=False,
                code="GATEWAY_UNAVAILABLE",
                message="EDA Gateway is unreachable",
            )

        if status != 200:
            return GatewayOutcome(
                ok=False,
                code="GATEWAY_UNAVAILABLE",
                message="EDA Gateway returned status %d" % status,
            )
        if len(body) > MAX_REPORT_BYTES:
            return GatewayOutcome(
                ok=False,
                code="RESOURCE_EXHAUSTED",
                message="EDA Gateway response exceeds the report budget",
            )
        try:
            decoded = json.loads(body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return GatewayOutcome(
                ok=False,
                code="GATEWAY_UNAVAILABLE",
                message="EDA Gateway returned a non-JSON body",
            )
        if not isinstance(decoded, Mapping):
            return GatewayOutcome(
                ok=False,
                code="GATEWAY_UNAVAILABLE",
                message="EDA Gateway returned a non-object body",
            )
        data = decoded.get("data")
        if isinstance(data, Mapping):
            return GatewayOutcome(ok=True, report=data)
        return GatewayOutcome(ok=True, report=decoded)

    def _http_error_outcome(self, exc: urllib.error.HTTPError) -> GatewayOutcome:
        """Map a Gateway failure envelope to a frozen agent error code.

        A Gateway that refuses our token, has no such job, or is otherwise
        unhappy is reported as ``GATEWAY_UNAVAILABLE`` except for a genuine
        404, which the caller can surface as ``NOT_FOUND``.  Whatever the
        Gateway said is redacted before it reaches a log line or a client.
        """
        code = "NOT_FOUND" if exc.code == 404 else "GATEWAY_UNAVAILABLE"
        if exc.code == 404:
            message = "EDA Gateway has no such job or report"
        else:
            message = "EDA Gateway rejected the request with status %d" % exc.code
        try:
            payload = json.loads(exc.read(MAX_REPORT_BYTES).decode("utf-8"))
        except Exception:  # pragma: no cover - best effort diagnostics only
            payload = None
        if isinstance(payload, Mapping):
            error = payload.get("error")
            if isinstance(error, Mapping):
                detail = str(error.get("message", "")).strip()
                if detail and code != "NOT_FOUND":
                    message = _safe(detail, self._secrets)
        return GatewayOutcome(ok=False, code=code, message=message)

    def fetch_job_report(self, job_id: str) -> GatewayOutcome:
        """Fetch ``GET {gateway_url}/api/v1/jobs/{job_id}/report``.

        ``job_id`` is validated by the caller; this method only ever appends it
        to a fixed path, which keeps the URL space closed.
        """
        if not job_id or any(char in job_id for char in "/\\?#"):
            return GatewayOutcome(
                ok=False,
                code="INVALID_ARGUMENT",
                message="job_id must be a non-empty opaque identifier",
            )
        return self._get_json("/api/v1/jobs/%s/report" % job_id)


def _safe(message: str, secrets: tuple[str, ...]) -> str:
    """Redact secrets and local paths from a diagnostic sentence."""
    return sanitize_message(message, secrets)


def _loopback_host(base_url: str) -> str:
    """Derive the Host header value from the bootstrap gateway URL."""
    without_scheme = base_url.split("://", 1)[-1]
    return without_scheme.split("/", 1)[0] or "127.0.0.1"
