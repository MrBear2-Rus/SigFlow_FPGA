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
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Mapping, Optional

from .util import sanitize_message

LOGGER = logging.getLogger(__name__)

DEFAULT_TIMEOUT_SECONDS: float = 8.0
"""Per-request timeout; the UI is waiting on the other end of this call."""

MAX_REPORT_BYTES: int = 2 * 1024 * 1024
"""Refuse to buffer an unbounded Gateway response into model context."""

_LOOPBACK_HOSTS = frozenset({"127.0.0.1", "localhost", "::1"})


class _RefuseRedirects(urllib.request.HTTPRedirectHandler):
    """AD-04: never follow a redirect.

    A redirect would let the Gateway (or anything that can answer on its port)
    move our credentialed request to another origin.  We refuse instead of
    silently re-issuing it.
    """

    def redirect_request(  # type: ignore[override]
        self,
        req: urllib.request.Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> None:
        raise urllib.error.HTTPError(req.full_url, code, "redirect refused", headers, fp)


def parse_loopback_base_url(base_url: str) -> Optional[tuple[str, int]]:
    """Return ``(host, port)`` when *base_url* is an allowed loopback http endpoint.

    AD-04: the bootstrap record is the only source of the Gateway address, so it is
    validated once here instead of trusting whatever string arrived: http only,
    literal loopback host, explicit port, and no path/query/fragment/userinfo that
    could redirect the request elsewhere.
    """
    try:
        parts = urllib.parse.urlsplit(base_url)
    except ValueError:
        return None
    if parts.scheme != "http":
        return None
    if parts.username or parts.password:
        return None
    host = (parts.hostname or "").lower()
    if host not in _LOOPBACK_HOSTS:
        return None
    try:
        port = parts.port
    except ValueError:
        return None
    if port is None or not (0 < port < 65536):
        return None
    if parts.path not in ("", "/") or parts.query or parts.fragment:
        return None
    return host, port


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
        # AD-04：地址在构造时就校验；不合法则**不发出任何请求**（fail closed），
        # 每次调用返回 GATEWAY_UNAVAILABLE。同时禁用代理与重定向，避免凭据被带离本机。
        self._endpoint = parse_loopback_base_url(self._base_url)
        self._opener: Optional[urllib.request.OpenerDirector] = None
        if self._endpoint is not None:
            self._opener = urllib.request.build_opener(
                urllib.request.ProxyHandler({}), _RefuseRedirects()
            )

    @property
    def blocked(self) -> bool:
        """True when the configured URL is not an allowed loopback endpoint."""
        return self._endpoint is None

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
        return self.request("GET", path)

    def request(self, method: str, path: str, body: Optional[Mapping[str, Any]] = None,
                idempotency_key: Optional[str] = None) -> GatewayOutcome:
        """Perform one bounded GET and classify the outcome."""
        if self._opener is None:
            # AD-04：地址不合法时绝不发起请求。
            return GatewayOutcome(
                ok=False,
                code="GATEWAY_UNAVAILABLE",
                message="gateway_url is not an allowed loopback http endpoint",
            )
        url = self._base_url + path
        if not path.startswith("/api/v1/") or any(char in path for char in "\\\r\n#"):
            return GatewayOutcome(False, code="INVALID_ARGUMENT", message="invalid Gateway route")
        headers = self._headers()
        data = None if body is None else json.dumps(body).encode("utf-8")
        if data is not None:
            if len(data) > 1024 * 1024:
                return GatewayOutcome(False, code="RESOURCE_EXHAUSTED", message="request exceeds budget")
            headers["Content-Type"] = "application/json"
        if idempotency_key:
            headers["Idempotency-Key"] = idempotency_key
        request = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with self._opener.open(request, timeout=self._timeout) as response:
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

        if status not in (200, 201, 202):
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
        if (decoded.get("schema_version") == "edu.api.v1" and "error" not in decoded
                and isinstance(decoded.get("request_id"), str) and decoded["request_id"]
                and isinstance(decoded.get("trace_id"), str) and decoded["trace_id"]
                and isinstance(data, Mapping)):
            return GatewayOutcome(ok=True, report=data)
        return GatewayOutcome(False, code="GATEWAY_UNAVAILABLE", message="invalid Gateway envelope")

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
                if error.get("code") in ("NOT_FOUND", "POLICY_DENIED", "STALE_REVISION",
                                         "IDEMPOTENCY_CONFLICT", "CURSOR_EXPIRED",
                                         "RESOURCE_EXHAUSTED", "CAPABILITY_UNAVAILABLE"):
                    code = error["code"]
                detail = str(error.get("message", "")).strip()
                if detail and code != "NOT_FOUND":
                    message = _safe(detail, self._secrets)
        return GatewayOutcome(ok=False, code=code, message=message)

    def fetch_job_report(self, job_id: str) -> GatewayOutcome:
        """Fetch ``GET {gateway_url}/api/v1/jobs/{job_id}/report``.

        ``job_id`` is validated by the caller; this method only ever appends it
        to a fixed path, which keeps the URL space closed.
        """
        import re
        if not isinstance(job_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,256}", job_id):
            return GatewayOutcome(
                ok=False,
                code="INVALID_ARGUMENT",
                message="job_id must be a non-empty opaque identifier",
            )
        return self._get_json("/api/v1/jobs/%s/report" % job_id)

    @staticmethod
    def identifier(value: str) -> str:
        import re
        if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,256}", value):
            raise ValueError("invalid opaque identifier")
        return value

    def project(self, project_id: str, resource: str = "context") -> GatewayOutcome:
        if resource not in ("context", "state", "jobs", "legacy/jobs"):
            raise ValueError("unsupported project resource")
        return self._get_json("/api/v1/projects/%s/%s" % (self.identifier(project_id), resource))

    def capabilities(self) -> GatewayOutcome:
        return self._get_json("/api/v1/capabilities")

    def source(self, project_id: str, source_id: str, start_line: int, end_line: int) -> GatewayOutcome:
        if (type(start_line) is not int or type(end_line) is not int or
                not 1 <= start_line <= end_line <= 2**64 - 1):
            raise ValueError("invalid source interval")
        return self._get_json("/api/v1/projects/%s/sources/%s?start_line=%d&end_line=%d" %
                              (self.identifier(project_id), self.identifier(source_id), start_line, end_line))

    def design_node(self, project_id: str, node_id: str) -> GatewayOutcome:
        return self._get_json("/api/v1/projects/%s/design/nodes/%s" % (self.identifier(project_id), self.identifier(node_id)))

    def wave_query(self, artifact_id: str, body: Mapping[str, Any]) -> GatewayOutcome:
        for key in ("start_tick", "end_tick"):
            value = body.get(key)
            if not isinstance(value, str) or not value.isascii() or not value.isdigit() or len(value) > 20 or int(value) > 2**64 - 1:
                raise ValueError("tick must be a uint64 decimal string")
        if int(body["start_tick"]) > int(body["end_tick"]):
            raise ValueError("reversed wave interval")
        return self.request("POST", "/api/v1/waves/%s/query" % self.identifier(artifact_id), body)

    def wave_signals(self, artifact_id: str) -> GatewayOutcome:
        return self._get_json("/api/v1/waves/%s/signals" % self.identifier(artifact_id))

    def snapshot(self, project_id: str, revision: str) -> GatewayOutcome:
        return self.request("POST", "/api/v1/projects/%s/snapshots" % self.identifier(project_id),
                            {"expected_revision": revision, "require_saved": True})

    def context_query(self, project_id: str, body: Mapping[str, Any]) -> GatewayOutcome:
        return self.request("POST", "/api/v1/projects/%s/context/query" % self.identifier(project_id), body)

    def job(self, job_id: str, cancel: bool = False) -> GatewayOutcome:
        return self.request("POST" if cancel else "GET", "/api/v1/jobs/%s%s" %
                            (self.identifier(job_id), "/cancel" if cancel else ""), {} if cancel else None)

    def submit(self, project_id: str, body: Mapping[str, Any], key: str) -> GatewayOutcome:
        return self.request("POST", "/api/v1/projects/%s/jobs" % self.identifier(project_id), body, key)

    def grant(self, grant_id: str) -> GatewayOutcome:
        return self._get_json("/api/v1/grants/%s" % self.identifier(grant_id))


def _safe(message: str, secrets: tuple[str, ...]) -> str:
    """Redact secrets and local paths from a diagnostic sentence."""
    return sanitize_message(message, secrets)


def _loopback_host(base_url: str) -> str:
    """Derive the Host header value from the bootstrap gateway URL."""
    without_scheme = base_url.split("://", 1)[-1]
    return without_scheme.split("/", 1)[0] or "127.0.0.1"
