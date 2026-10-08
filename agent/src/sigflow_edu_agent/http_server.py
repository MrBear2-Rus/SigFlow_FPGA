"""Loopback HTTP server exposing the frozen ``/api/v1`` surface.

The server is intentionally tiny and stdlib-only: :mod:`http.server` for the
socket layer, :mod:`urllib.request` (through :mod:`gateway_client`) for the
single outbound call.  It binds ``127.0.0.1`` only, requires the UI bearer
token on every route except ``/health``, and renders every response with the
same envelope the SigFlow EDA Gateway uses.

Hard limits enforced here (never silently ignored):

* request bodies: 1 MiB, otherwise ``413 RESOURCE_EXHAUSTED``;
* ``selection.text``: 64 KiB, otherwise ``400 INVALID_ARGUMENT``;
* cards per run: 16, enforced by :mod:`cards`;
* sessions: 32, LRU-evicted by :mod:`session`.

No absolute local path may appear in any response.  The bootstrap tokens are
never echoed: they are only used for comparisons and outbound headers.
"""

from __future__ import annotations

import hmac
import json
import logging
import re
import threading
from dataclasses import dataclass, field
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Mapping, Optional
from urllib.parse import parse_qs, urlsplit

from .dispatch import RunDispatcher, RunRequest
from .errors import AgentError, failure_envelope, success_envelope
from .gateway_client import GatewayClient
from .model_client import ModelClient, ModelStatus
from .rules import Selection
from .session import SessionRecord, SessionStore
from .util import decimal, sanitize_message
from .version import (
    AGENT_VERSION,
    LEVELS,
    MAX_CARD_BYTES,
    PROTOCOL,
    RUN_KINDS,
)

LOGGER = logging.getLogger(__name__)

MAX_BODY_BYTES: int = 1024 * 1024
"""Hard cap for a request body: 1 MiB, matching the EDA Gateway."""

MAX_EVENT_PAGE: int = 500
"""Upper bound for one ``/events`` page."""

DEFAULT_EVENT_PAGE: int = 100
"""Default ``/events`` page size."""

_LOOPBACK_HOSTS: frozenset[str] = frozenset({"127.0.0.1", "localhost", "::1", "[::1]"})

_SUPPORTED_METHODS: frozenset[str] = frozenset({"GET", "POST"})
"""Methods this service serves; anything else is answered with 404 NOT_FOUND."""

_ROUTE_SESSION = re.compile(r"^/api/v1/sessions/(?P<sid>[^/]+)$")
_ROUTE_RUNS = re.compile(r"^/api/v1/sessions/(?P<sid>[^/]+)/runs$")
_ROUTE_EVENTS = re.compile(r"^/api/v1/sessions/(?P<sid>[^/]+)/events$")
_ROUTE_RUN = re.compile(r"^/api/v1/runs/(?P<rid>[^/]+)$")
_ROUTE_SHUTDOWN = re.compile(r"^/api/v1/shutdown$")
_ROUTE_HEALTH = re.compile(r"^/api/v1/health$")
_ROUTE_CAPABILITIES = re.compile(r"^/api/v1/capabilities$")
_ROUTE_SESSIONS = re.compile(r"^/api/v1/sessions$")


@dataclass
class ServerConfig:
    """Everything the HTTP layer needs, assembled once at startup."""

    instance_id: str
    ui_token: str
    agent_version: str = AGENT_VERSION
    host: str = "127.0.0.1"
    port: int = 0
    gateway: Optional[GatewayClient] = None
    model: Optional[ModelClient] = None
    secrets: tuple[str, ...] = ()
    stop_event: threading.Event = field(default_factory=threading.Event)


class EduAgentState:
    """Per-process runtime state shared by all request handlers."""

    def __init__(self, config: ServerConfig) -> None:
        self.config = config
        self.sessions = SessionStore()
        model = config.model or ModelClient(secrets=config.secrets)
        gateway = config.gateway or GatewayClient(
            "http://127.0.0.1", "", secrets=config.secrets
        )
        self.dispatcher = RunDispatcher(gateway=gateway, model=model)
        self._counter_lock = threading.Lock()
        self._request_counter = 0

    @property
    def model_status(self) -> ModelStatus:
        """Current model availability reported by health and capabilities."""
        return self.dispatcher.model_status

    def next_ids(self) -> tuple[str, str]:
        """Allocate a ``(request_id, trace_id)`` pair for one response."""
        with self._counter_lock:
            self._request_counter += 1
            value = self._request_counter
        return "req-%d" % value, "trace-%d" % value


class EduAgentServer(ThreadingHTTPServer):
    """Threaded HTTP server that carries the runtime state for its handlers."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], state: EduAgentState) -> None:
        self.state = state
        super().__init__(address, EduAgentRequestHandler)


class EduAgentRequestHandler(BaseHTTPRequestHandler):
    """One request handler: routing, auth, limits and envelope rendering."""

    server_version = "SigFlowEduAgent/" + AGENT_VERSION
    sys_version = ""
    protocol_version = "HTTP/1.1"

    # -- plumbing ---------------------------------------------------------

    @property
    def state(self) -> EduAgentState:
        """Runtime state attached to the owning server."""
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, format: str, *args: Any) -> None:
        """Route the access log through :mod:`logging` with token redaction."""
        message = sanitize_message(format % args, self.state.config.secrets)
        LOGGER.info("%s", message)

    def log_error(self, format: str, *args: Any) -> None:
        """Route the error log through :mod:`logging` with token redaction."""
        message = sanitize_message(format % args, self.state.config.secrets)
        LOGGER.warning("%s", message)

    def _send_json(self, status: int, payload: Mapping[str, Any]) -> None:
        """Serialize and send one JSON response."""
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _respond_data(self, status: int, data: Any) -> None:
        """Send a success envelope."""
        request_id, trace_id = self.state.next_ids()
        self._send_json(status, success_envelope(request_id, trace_id, data))

    def _respond_error(self, error: AgentError) -> None:
        """Send a failure envelope for an :class:`AgentError`."""
        request_id, trace_id = self.state.next_ids()
        message = sanitize_message(error.message, self.state.config.secrets)
        self._send_json(
            error.http_status,
            failure_envelope(request_id, trace_id, error.code, message, error.retryable),
        )

    def _fail(
        self,
        code: str,
        message: str,
        status: int,
        retryable: bool = False,
    ) -> None:
        """Send a failure envelope built from raw parts."""
        self._respond_error(AgentError(code, message, retryable, status))

    # -- dispatch ---------------------------------------------------------

    def do_GET(self) -> None:  # noqa: N802 - http.server API
        """Handle a GET request."""
        self._dispatch("GET")

    def do_POST(self) -> None:  # noqa: N802 - http.server API
        """Handle a POST request."""
        self._dispatch("POST")

    def do_PUT(self) -> None:  # noqa: N802 - http.server API
        """Reject unsupported methods with 404 NOT_FOUND."""
        self._dispatch("PUT")

    def do_DELETE(self) -> None:  # noqa: N802 - http.server API
        """Reject unsupported methods with 404 NOT_FOUND."""
        self._dispatch("DELETE")

    def do_PATCH(self) -> None:  # noqa: N802 - http.server API
        """Reject unsupported methods with 404 NOT_FOUND."""
        self._dispatch("PATCH")

    def do_HEAD(self) -> None:  # noqa: N802 - http.server API
        """Reject HEAD with 404 NOT_FOUND, like any other unsupported method."""
        self._dispatch("HEAD")

    def do_OPTIONS(self) -> None:  # noqa: N802 - http.server API
        """Reject OPTIONS; this service never enables wildcard CORS."""
        self._dispatch("OPTIONS")

    def _dispatch(self, method: str) -> None:
        """Run boundary checks, authentication and route matching."""
        try:
            self._check_host()
            path = urlsplit(self.path).path
            if method == "GET" and _ROUTE_HEALTH.match(path):
                self._handle_health()
                return
            if method not in _SUPPORTED_METHODS:
                # An unsupported method is a routing fact, not an auth fact, and
                # the contract asks for 404 NOT_FOUND either way.
                self._fail("NOT_FOUND", "status 404", 404)
                return
            self._check_auth()
            if method == "GET" and _ROUTE_CAPABILITIES.match(path):
                self._handle_capabilities()
                return
            if method == "POST" and _ROUTE_SESSIONS.match(path):
                self._handle_create_session()
                return
            match = _ROUTE_RUNS.match(path)
            if method == "POST" and match:
                self._handle_create_run(match.group("sid"))
                return
            match = _ROUTE_EVENTS.match(path)
            if method == "GET" and match:
                self._handle_events(match.group("sid"))
                return
            match = _ROUTE_SESSION.match(path)
            if method == "GET" and match:
                self._handle_session(match.group("sid"))
                return
            match = _ROUTE_RUN.match(path)
            if method == "GET" and match:
                self._handle_run(match.group("rid"))
                return
            if method == "POST" and _ROUTE_SHUTDOWN.match(path):
                self._handle_shutdown()
                return
            self._fail("NOT_FOUND", "status 404", 404)
        except AgentError as error:
            self._respond_error(error)
        except Exception as exc:  # pragma: no cover - defensive boundary
            LOGGER.exception("unhandled handler failure")
            self._fail(
                "INVALID_ARGUMENT",
                "internal error: %s" % type(exc).__name__,
                500,
            )

    # -- boundary and auth ------------------------------------------------

    def _check_host(self) -> None:
        """Reject a non-loopback Host header with 403 POLICY_DENIED."""
        raw = self.headers.get("Host", "")
        hostname = raw.strip()
        if hostname.startswith("["):
            end = hostname.find("]")
            hostname = hostname[: end + 1] if end >= 0 else hostname
        elif ":" in hostname:
            hostname = hostname.split(":", 1)[0]
        if hostname not in _LOOPBACK_HOSTS:
            raise AgentError(
                "POLICY_DENIED",
                "Host header must be loopback",
                http_status=403,
            )

    def _check_auth(self) -> None:
        """Require ``Authorization: Bearer <ui_token>`` on every non-health route."""
        header = self.headers.get("Authorization", "")
        scheme, _, token = header.partition(" ")
        expected = self.state.config.ui_token
        if not expected:
            raise AgentError(
                "UNAUTHENTICATED",
                "this instance has no UI token configured",
                http_status=401,
            )
        if scheme.lower() != "bearer" or not token:
            raise AgentError(
                "UNAUTHENTICATED",
                "missing or invalid bearer token",
                http_status=401,
            )
        if not hmac.compare_digest(token.encode("utf-8"), expected.encode("utf-8")):
            raise AgentError(
                "UNAUTHENTICATED",
                "missing or invalid bearer token",
                http_status=401,
            )

    # -- body helpers -----------------------------------------------------

    def _read_json_body(self) -> Mapping[str, Any]:
        """Read, size-check, parse and type-check a JSON object body."""
        raw_length = self.headers.get("Content-Length", "0")
        try:
            length = int(raw_length)
        except ValueError:
            raise AgentError("INVALID_ARGUMENT", "invalid Content-Length", http_status=400)
        if length < 0:
            raise AgentError("INVALID_ARGUMENT", "invalid Content-Length", http_status=400)
        if length > MAX_BODY_BYTES:
            # Drain what the client already sent before answering: closing a
            # socket with unread request bytes can trigger an RST that destroys
            # the 413 response on the client side.
            self._drain(length)
            raise AgentError(
                "RESOURCE_EXHAUSTED",
                "request body exceeds %d bytes" % MAX_BODY_BYTES,
                http_status=413,
            )
        body = self.rfile.read(length) if length else b""
        if not body:
            return {}
        try:
            decoded = json.loads(body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            raise AgentError("INVALID_ARGUMENT", "request body is not valid JSON", http_status=400)
        if not isinstance(decoded, Mapping):
            raise AgentError(
                "INVALID_ARGUMENT",
                "request body must be a JSON object",
                http_status=400,
            )
        return decoded

    def _drain(self, length: int, chunk: int = 65536) -> None:
        """Consume an over-sized body (up to 8 MiB) so the response can be read."""
        remaining = min(length, 8 * MAX_BODY_BYTES)
        while remaining > 0:
            read = self.rfile.read(min(chunk, remaining))
            if not read:
                return
            remaining -= len(read)

    def _query(self) -> Mapping[str, list[str]]:
        """Return the parsed query string of the current request."""
        return parse_qs(urlsplit(self.path).query)

    # -- routes -----------------------------------------------------------

    def _handle_health(self) -> None:
        """``GET /api/v1/health`` — the only route that accepts an absent token."""
        status = self.state.model_status
        self._respond_data(
            HTTPStatus.OK,
            {
                "protocol": PROTOCOL,
                "agent_version": self.state.config.agent_version,
                "instance_id": self.state.config.instance_id,
                "ready": True,
                "model": status.to_wire(),
            },
        )

    def _handle_capabilities(self) -> None:
        """``GET /api/v1/capabilities`` — modes, capability readiness and model state."""
        status = self.state.model_status
        modes = [
            {
                "id": level,
                "available": level != "L4",
                "reason": (
                    "L4 参考解需要策略允许并经过两次明确操作；本服务只提供要点清单，不代写工程文件。"
                    if level == "L4"
                    else "该难度由确定性规则引擎提供，无需模型 Key。"
                ),
            }
            for level in LEVELS
        ]
        capabilities = [
            {
                "name": "edu.rules.diagnose",
                "ready": True,
                "reason": "确定性规则引擎随服务启动即可用。",
                "plugin_version": self.state.config.agent_version,
            },
            {
                "name": "edu.report.review",
                "ready": True,
                "reason": "需要 EDA Gateway 可达；不可达时本次运行失败并明确报错。",
                "plugin_version": self.state.config.agent_version,
            },
            {
                "name": "edu.plan.propose",
                "ready": True,
                "reason": "只生成计划卡片，不持有执行权限。",
                "plugin_version": self.state.config.agent_version,
            },
            {
                "name": "edu.model.explain",
                "ready": status.available,
                "reason": status.reason,
                "plugin_version": None if not status.available else self.state.config.agent_version,
            },
        ]
        self._respond_data(
            HTTPStatus.OK,
            {
                "protocol": PROTOCOL,
                "agent_version": self.state.config.agent_version,
                "modes": modes,
                "capabilities": capabilities,
                "model": status.to_wire(),
                "course": {
                    "available": False,
                    "reason": "v1 未内置课程库；课程引用由后续版本提供。",
                },
                "execution": {
                    "can_run_eda": False,
                    "note": (
                        "EDA 执行只能由 SigFlow 签发的 grant 触发；本服务不持有执行权限"
                    ),
                },
            },
        )

    def _handle_create_session(self) -> None:
        """``POST /api/v1/sessions``."""
        body = self._read_json_body()
        project_id = body.get("project_id")
        revision = body.get("revision")
        if not isinstance(project_id, str) or not project_id.strip():
            raise AgentError("INVALID_ARGUMENT", "project_id must be a non-empty string", http_status=400)
        if not isinstance(revision, str) or not revision.strip():
            raise AgentError("INVALID_ARGUMENT", "revision must be a non-empty string", http_status=400)
        top = body.get("top")
        target = body.get("target")
        locale = body.get("locale") or "zh-CN"
        if not isinstance(locale, str):
            locale = "zh-CN"
        session = self.state.sessions.create_session(
            project_id=project_id,
            revision=revision,
            top=top if isinstance(top, str) else "",
            target=target if target is not None else {},
            locale=locale,
        )
        self.state.sessions.append_event(
            session,
            "session/created",
            {"project_id": project_id, "revision": revision},
        )
        self._respond_data(
            HTTPStatus.CREATED,
            {
                "session_id": session.session_id,
                "project_id": session.project_id,
                "revision": session.revision,
                "state_version": "1",
                "created_at": session.created_at,
            },
        )

    def _handle_session(self, session_id: str) -> None:
        """``GET /api/v1/sessions/{sid}``."""
        session = self._require_session(session_id)
        self._respond_data(HTTPStatus.OK, session.to_wire())

    def _handle_run(self, run_id: str) -> None:
        """``GET /api/v1/runs/{rid}``."""
        run = self.state.sessions.get_run(run_id)
        if run is None:
            raise AgentError("NOT_FOUND", "unknown run", http_status=404)
        self._respond_data(HTTPStatus.OK, run.to_wire())

    def _handle_create_run(self, session_id: str) -> None:
        """``POST /api/v1/sessions/{sid}/runs``."""
        session = self._require_session(session_id)
        body = self._read_json_body()
        request = self._parse_run_request(session, body)
        state_version = decimal(session.state_version + 1)
        outcome = self.state.dispatcher.dispatch(request, state_version)
        if outcome.error_code:
            run = self.state.sessions.create_run(
                session=session,
                kind=request.kind,
                level=request.level,
                state=outcome.state,
                cards=[],
                omitted=[],
                model_used=False,
                error_code=outcome.error_code,
                error_message=outcome.error_message,
            )
            self.state.sessions.append_event(
                session,
                "run/failed",
                {"run_id": run.run_id, "kind": request.kind, "code": outcome.error_code},
            )
            raise AgentError(
                outcome.error_code,
                outcome.error_message or "run failed",
                http_status=_status_for(outcome.error_code),
                retryable=outcome.error_code in ("GATEWAY_UNAVAILABLE", "SERVICE_UNAVAILABLE"),
            )
        run = self.state.sessions.create_run(
            session=session,
            kind=request.kind,
            level=request.level,
            state=outcome.state,
            cards=outcome.cards,
            omitted=outcome.omitted,
            model_used=outcome.model_used,
        )
        self.state.sessions.append_event(
            session,
            "run/finished",
            {
                "run_id": run.run_id,
                "kind": request.kind,
                "level": request.level,
                "state": outcome.state,
                "card_count": len(outcome.cards),
            },
        )
        self._respond_data(
            HTTPStatus.CREATED,
            {
                "run_id": run.run_id,
                "session_id": run.session_id,
                "state": run.state,
                "state_version": decimal(run.state_version),
                "cards": run.cards,
                "omitted": run.omitted,
                "model_used": run.model_used,
            },
        )

    def _handle_events(self, session_id: str) -> None:
        """``GET /api/v1/sessions/{sid}/events?after=<seq>``."""
        session = self._require_session(session_id)
        raw_after = self._query().get("after", [None])[0]
        cursor: Optional[int] = None
        if raw_after not in (None, ""):
            if not str(raw_after).isdigit():
                raise AgentError(
                    "INVALID_ARGUMENT",
                    "after must be a decimal cursor",
                    http_status=400,
                )
            cursor = int(raw_after)
        events, high, oldest = self.state.sessions.read_events(session, cursor)
        if events is None:
            raise AgentError(
                "CURSOR_EXPIRED",
                "cursor is older than the retained event window",
                http_status=410,
            )
        self._respond_data(
            HTTPStatus.OK,
            {
                "events": events,
                "high_watermark": decimal(high),
                "oldest_sequence": decimal(oldest),
            },
        )

    def _handle_shutdown(self) -> None:
        """``POST /api/v1/shutdown`` — acknowledge, then stop serving."""
        self._respond_data(HTTPStatus.ACCEPTED, {"state": "shutting_down"})
        self.state.config.stop_event.set()

    # -- request parsing --------------------------------------------------

    def _require_session(self, session_id: str) -> SessionRecord:
        """Return a session or raise ``404 NOT_FOUND``."""
        session = self.state.sessions.get_session(session_id)
        if session is None:
            raise AgentError("NOT_FOUND", "unknown session", http_status=404)
        return session

    def _parse_run_request(
        self,
        session: SessionRecord,
        body: Mapping[str, Any],
    ) -> RunRequest:
        """Validate a run body into a :class:`RunRequest`."""
        kind = body.get("kind")
        if not isinstance(kind, str) or kind not in RUN_KINDS:
            raise AgentError(
                "INVALID_ARGUMENT",
                "kind must be one of %s" % ", ".join(RUN_KINDS),
                http_status=400,
            )
        level = body.get("level", "L1")
        if not isinstance(level, str) or level not in LEVELS:
            raise AgentError(
                "INVALID_ARGUMENT",
                "level must be one of %s" % ", ".join(LEVELS),
                http_status=400,
            )
        more = body.get("more", False)
        if not isinstance(more, bool):
            raise AgentError("INVALID_ARGUMENT", "more must be a boolean", http_status=400)

        job_id = body.get("job_id")
        if job_id is not None and not isinstance(job_id, str):
            raise AgentError("INVALID_ARGUMENT", "job_id must be a string", http_status=400)

        selection = self._parse_selection(body.get("selection"))

        if kind == "report_review" and not (job_id or "").strip():
            raise AgentError(
                "INVALID_ARGUMENT",
                "kind=report_review requires job_id",
                http_status=400,
            )
        if level == "L4" and not more:
            raise AgentError(
                "POLICY_DENIED",
                "L4 参考解采用两步放行策略：第一次请求只做引导讲解，"
                "确认你已自行尝试后，再带 more=true 发起第二次请求；"
                "本次响应不会包含任何参考答案。",
                http_status=403,
            )
        return RunRequest(
            kind=kind,
            level=level,
            selection=selection,
            job_id=(job_id or "").strip() or None,
            more=more,
            top=session.top,
            revision=session.revision,
        )

    def _parse_selection(self, raw: Any) -> Selection:
        """Validate and bound the ``selection`` object."""
        if raw is None:
            return Selection(text="")
        if not isinstance(raw, Mapping):
            raise AgentError("INVALID_ARGUMENT", "selection must be an object or null", http_status=400)
        text = raw.get("text") or ""
        if not isinstance(text, str):
            raise AgentError("INVALID_ARGUMENT", "selection.text must be a string", http_status=400)
        if len(text.encode("utf-8")) > MAX_CARD_BYTES:
            raise AgentError(
                "INVALID_ARGUMENT",
                "selection.text exceeds %d bytes" % MAX_CARD_BYTES,
                http_status=400,
            )
        node_id = raw.get("node_id")
        source_id = raw.get("source_id")
        for name, value in (("node_id", node_id), ("source_id", source_id)):
            if value is not None and not isinstance(value, str):
                raise AgentError(
                    "INVALID_ARGUMENT",
                    "selection.%s must be a string" % name,
                    http_status=400,
                )
        start_line = _optional_int(raw.get("start_line"), "selection.start_line")
        end_line = _optional_int(raw.get("end_line"), "selection.end_line")
        return Selection(
            text=text,
            node_id=node_id,
            source_id=source_id,
            start_line=start_line,
            end_line=end_line,
        )


def _optional_int(value: Any, field_name: str) -> Optional[int]:
    """Validate an optional non-negative integer field."""
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise AgentError("INVALID_ARGUMENT", "%s must be an integer" % field_name, http_status=400)
    if value < 0:
        raise AgentError("INVALID_ARGUMENT", "%s must be non-negative" % field_name, http_status=400)
    return value


def _status_for(code: str) -> int:
    """Map a frozen error code to the HTTP status used on the wire."""
    if code == "NOT_FOUND":
        return 404
    if code == "UNAUTHENTICATED":
        return 401
    if code in ("POLICY_DENIED", "CAPABILITY_UNAVAILABLE"):
        return 403
    if code == "RESOURCE_EXHAUSTED":
        return 413
    if code in ("STALE_REVISION", "IDEMPOTENCY_CONFLICT"):
        return 409
    if code == "CURSOR_EXPIRED":
        return 410
    if code in ("SERVICE_UNAVAILABLE", "MODEL_UNAVAILABLE", "GATEWAY_UNAVAILABLE"):
        return 503
    return 400


def create_server(state: EduAgentState) -> EduAgentServer:
    """Bind the loopback socket and return a ready-to-serve HTTP server."""
    config = state.config
    server = EduAgentServer((config.host, config.port), state)
    server.timeout = 0.2
    return server
