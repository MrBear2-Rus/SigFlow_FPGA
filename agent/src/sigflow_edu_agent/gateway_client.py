"""Read-only HTTP client for the SigFlow EDA Gateway.

This is the sidecar's *only* outbound channel.  It speaks HTTP to the loopback
Gateway that handed us ``gateway_url``/``gateway_token`` in the bootstrap
record; it never reads a file, never runs a command and never accepts a
caller-supplied URL.

Every call is a *single attempt*: no proxy, no redirect, no automatic retry
(including for POST), and the bearer token travels only in the
``Authorization`` header.  The address space is closed -- ``request`` accepts
routes under ``/api/v1`` only, and the UI-only grant/receipt *issuing* routes
are refused outright because this process is not the UI identity.

The Gateway normalizes job reports into ``edu.jobreport.v1`` and wraps them in
the same ``{schema_version, request_id, trace_id, data}`` envelope it uses for
every other route, so the client unwraps ``data`` when it is present.
"""

from __future__ import annotations

import json
import logging
import re
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

_OPAQUE_ID = re.compile(r"[A-Za-z0-9_-]{1,256}")
"""Opaque identifier shape: no separators, no traversal, bounded length."""

_ALLOWED_ROUTE = re.compile(r"^/api/v1/[A-Za-z0-9_\-./?=&%]+$")
"""The only URL shape this client will ever dial (path + simple query)."""

_IDEMPOTENCY_KEY = re.compile(r"[A-Za-z0-9._:-]{1,200}")
"""Header-safe idempotency key: no separators, no CR/LF, bounded length."""

_UI_ONLY_ROUTES: tuple[re.Pattern[str], ...] = (
    re.compile(r"^/api/v1/projects/[^/]+/grants(?:/|$)"),
    re.compile(r"^/api/v1/grants/[^/]+/revoke(?:/|$)"),
    re.compile(r"^/api/v1/projects/[^/]+/ui-receipts(?:/|$)"),
)
"""Routes that only the UI identity may call (issuing/revoking credentials).

The Agent may *read* a grant and *consume* a UI receipt, but it must never
issue one: T9/§6.7 keep the credential-issuing half of interface B on the UI
token.  Refusing these paths here makes that structural rather than a matter of
which helper happens to exist.
"""

_DOMAIN_ERROR_CODES: frozenset[str] = frozenset(
    {
        "POLICY_DENIED",
        "NOT_FOUND",
        "STALE_REVISION",
        "IDEMPOTENCY_CONFLICT",
        "CURSOR_EXPIRED",
        "RESOURCE_EXHAUSTED",
        "CAPABILITY_UNAVAILABLE",
    }
)
"""Frozen codes the Gateway may report *about a resource or a request*.

These are preserved verbatim because each one asks the UI for a different
action: re-read the project, show the historical view, stop retrying under the
same idempotency key, explain a disabled capability.  Everything else becomes
``GATEWAY_UNAVAILABLE`` -- including ``SERVICE_UNAVAILABLE`` and
``UNAUTHENTICATED``, which describe *our* channel (the Agent's own token was
rejected / the job service is down), not a decision the UI can act on.  That is
the mapping the published behaviour table documents.
"""

_LEGACY_ERROR_CODE_MAP: Mapping[str, str] = {
    "PATH_OUTSIDE_PROJECT": "POLICY_DENIED",
    "SOURCE_UNAVAILABLE": "GATEWAY_UNAVAILABLE",
    "PLUGIN_UNAVAILABLE": "CAPABILITY_UNAVAILABLE",
}
"""Older Gateway codes mapped onto the frozen Agent vocabulary."""


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
        # Our own bearer token is always redacted too: a Gateway that echoes
        # the credential back in an error message must not be able to put it
        # into a log line or into a client-facing ``message``.
        self._secrets = tuple(dict.fromkeys(s for s in (token, *secrets) if s))
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
        """Perform exactly one bounded request and classify the outcome.

        There is deliberately no retry loop anywhere in this method: a POST
        that fails is reported to the caller, never replayed by the client
        (``Idempotency-Key`` makes a caller-side retry safe if the UI wants
        one).  The route space is closed to ``/api/v1`` and the UI-only
        credential-issuing routes are refused before a socket is opened.
        """
        if self._opener is None:
            # AD-04：地址不合法时绝不发起请求。
            return GatewayOutcome(
                ok=False,
                code="GATEWAY_UNAVAILABLE",
                message="gateway_url is not an allowed loopback http endpoint",
            )
        # ``fullmatch`` (not ``match`` with ``$``) matters: ``$`` also matches
        # just before a trailing newline, which would let a caller smuggle a
        # line break into the request line.
        if _ALLOWED_ROUTE.fullmatch(path) is None or ".." in path:
            return GatewayOutcome(False, code="INVALID_ARGUMENT", message="invalid Gateway route")
        if any(pattern.match(path) for pattern in _UI_ONLY_ROUTES):
            # T9/§6.7：签发/撤销 grant 与签发 receipt 属于 UI 身份，Agent 不持有该能力。
            return GatewayOutcome(
                False,
                code="POLICY_DENIED",
                message="issuing or revoking grants and receipts belongs to the UI identity",
            )
        url = self._base_url + path
        headers = self._headers()
        data = None if body is None else json.dumps(body).encode("utf-8")
        if data is not None:
            if len(data) > 1024 * 1024:
                return GatewayOutcome(False, code="RESOURCE_EXHAUSTED", message="request exceeds budget")
            headers["Content-Type"] = "application/json"
        if idempotency_key is not None:
            # A caller-supplied header value must never be able to inject a
            # second header or a second request line.
            if _IDEMPOTENCY_KEY.fullmatch(idempotency_key) is None:
                return GatewayOutcome(
                    False,
                    code="INVALID_ARGUMENT",
                    message="Idempotency-Key must be a bounded opaque token",
                )
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

        A frozen code that describes the resource or the request
        (:data:`_DOMAIN_ERROR_CODES`) is preserved verbatim: flattening a 409
        ``IDEMPOTENCY_CONFLICT`` or a 403 ``POLICY_DENIED`` into
        ``GATEWAY_UNAVAILABLE`` would hide a decision the UI has to act on.
        Everything else -- an availability failure such as
        ``SERVICE_UNAVAILABLE``, a rejection of *our* token, an unknown code, a
        code that belongs to another interface, or a body that is not an error
        envelope -- becomes ``GATEWAY_UNAVAILABLE``.  A bare 404 stays
        ``NOT_FOUND``.

        Whatever the Gateway said is redacted (secrets *and* local paths)
        before it reaches a log line or a client-facing ``message``.
        """
        code = "NOT_FOUND" if exc.code == 404 else "GATEWAY_UNAVAILABLE"
        message = (
            "EDA Gateway has no such job or report"
            if exc.code == 404
            else "EDA Gateway rejected the request with status %d" % exc.code
        )
        payload = None
        try:
            try:
                raw = exc.read(MAX_REPORT_BYTES)
            finally:
                # Release the response socket now instead of at GC: an unclosed
                # HTTPError keeps a file object alive and warns on collection.
                exc.close()
            payload = json.loads(raw.decode("utf-8"))
        except Exception:  # pragma: no cover - best effort diagnostics only
            payload = None
        if isinstance(payload, Mapping):
            error = payload.get("error")
            if isinstance(error, Mapping):
                reported = error.get("code")
                if isinstance(reported, str) and reported in _DOMAIN_ERROR_CODES:
                    code = reported
                elif isinstance(reported, str) and reported in _LEGACY_ERROR_CODE_MAP:
                    code = _LEGACY_ERROR_CODE_MAP[reported]
                detail = str(error.get("message", "")).strip()
                if detail and code != "NOT_FOUND":
                    message = _safe(detail, self._secrets)
        return GatewayOutcome(ok=False, code=code, message=message)

    def fetch_job_report(self, job_id: str) -> GatewayOutcome:
        """Fetch ``GET {gateway_url}/api/v1/jobs/{job_id}/report``.

        ``job_id`` is validated by the caller; this method only ever appends it
        to a fixed path, which keeps the URL space closed.
        """
        if not isinstance(job_id, str) or _OPAQUE_ID.fullmatch(job_id) is None:
            return GatewayOutcome(
                ok=False,
                code="INVALID_ARGUMENT",
                message="job_id must be a non-empty opaque identifier",
            )
        return self._get_json("/api/v1/jobs/%s/report" % job_id)

    @staticmethod
    def identifier(value: str) -> str:
        """Return *value* if it is an opaque identifier, else raise ``ValueError``."""
        if not isinstance(value, str) or _OPAQUE_ID.fullmatch(value) is None:
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
        if not isinstance(body, Mapping):
            raise ValueError("wave query body must be an object")
        normalized = dict(body)
        for key in ("start_tick", "end_tick"):
            value = body.get(key)
            if isinstance(value, str):
                if (not value.isascii() or not value.isdigit()
                        or len(value) > 20 or int(value) > 2**64 - 1):
                    raise ValueError("tick must be a uint64 decimal value")
                normalized_value = value
            elif type(value) is int and 0 <= value <= 2**64 - 1:
                normalized_value = str(value)
            else:
                raise ValueError("tick must be a uint64 decimal value")
            normalized[key] = normalized_value
        if int(normalized["start_tick"]) > int(normalized["end_tick"]):
            raise ValueError("reversed wave interval")
        return self.request(
            "POST",
            "/api/v1/waves/%s/query" % self.identifier(artifact_id),
            normalized,
        )

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
        """Read one grant's status (``GET /api/v1/grants/{id}``).

        Reading is an Agent capability; issuing and revoking are not (see
        :data:`_UI_ONLY_ROUTES`).
        """
        return self._get_json("/api/v1/grants/%s" % self.identifier(grant_id))

    def consume_ui_receipt(self, receipt_id: str) -> GatewayOutcome:
        """Atomically consume a UI-issued receipt (``POST .../consume``).

        Consuming is an Agent route; *issuing* the receipt is not -- this
        client refuses that route instead of exposing a method for it.
        """
        return self.request(
            "POST",
            "/api/v1/ui-receipts/%s/consume" % self.identifier(receipt_id),
            {},
        )


def _safe(message: str, secrets: tuple[str, ...]) -> str:
    """Redact secrets and local paths from a diagnostic sentence."""
    return sanitize_message(message, secrets)


def _loopback_host(base_url: str) -> str:
    """Derive the Host header value from the bootstrap gateway URL."""
    without_scheme = base_url.split("://", 1)[-1]
    return without_scheme.split("/", 1)[0] or "127.0.0.1"
