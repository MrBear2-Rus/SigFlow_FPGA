"""Error codes and response-envelope helpers of the SigFlow Edu Agent.

Every response produced by the sidecar uses the same envelope shape as the
SigFlow EDA Gateway so that one UI client can consume both services:

* success: ``{"schema_version", "request_id", "trace_id", "data"}``
* failure: ``{"schema_version", "request_id", "trace_id", "error": {...}}``
"""

from __future__ import annotations

import re
from typing import Any, Optional

from .version import SCHEMA_VERSION

ERROR_CODES: tuple[str, ...] = (
    "INVALID_ARGUMENT",
    "UNAUTHENTICATED",
    "POLICY_DENIED",
    "NOT_FOUND",
    "STALE_REVISION",
    "IDEMPOTENCY_CONFLICT",
    "CURSOR_EXPIRED",
    "RESOURCE_EXHAUSTED",
    "SERVICE_UNAVAILABLE",
    "MODEL_UNAVAILABLE",
    "GATEWAY_UNAVAILABLE",
    "CAPABILITY_UNAVAILABLE",
)
"""The complete set of error codes this service may return.  Nothing else."""

_REQUEST_ID_PATTERN = re.compile(r"^req-[1-9][0-9]*$")
_TRACE_ID_PATTERN = re.compile(r"^trace-[1-9][0-9]*$")


class AgentError(Exception):
    """A failure that must be rendered as a failure envelope.

    Attributes:
        code: One of :data:`ERROR_CODES`.
        message: Short, human readable sentence.
        retryable: Whether the client may retry the same request unchanged.
        http_status: HTTP status code carried alongside ``code``.
    """

    def __init__(
        self,
        code: str,
        message: str,
        retryable: bool = False,
        http_status: int = 400,
    ) -> None:
        if code not in ERROR_CODES:
            raise ValueError("unknown agent error code: %s" % code)
        super().__init__(message)
        self.code = code
        self.message = message
        # T4 fixes retryability to the HTTP status.  A caller cannot mark a
        # non-503 error retryable, and every 503 response is retryable.
        self.retryable = http_status == 503
        self.http_status = http_status

    def __str__(self) -> str:  # pragma: no cover - trivial
        return "%s: %s" % (self.code, self.message)


def success_envelope(
    request_id: str,
    trace_id: str,
    data: Any,
) -> dict[str, Any]:
    """Build the success envelope for one response."""
    return {
        "schema_version": SCHEMA_VERSION,
        "request_id": request_id,
        "trace_id": trace_id,
        "data": data,
    }


def failure_envelope(
    request_id: str,
    trace_id: str,
    code: str,
    message: str,
    retryable: bool = False,
    details: Optional[Any] = None,
    http_status: Optional[int] = None,
) -> dict[str, Any]:
    """Build the failure envelope for one response.

    ``details`` stays ``None`` in v1.  ``retryable`` is derived only from
    ``http_status`` so a caller cannot mark a 4xx response retryable; without
    an HTTP status it fails closed to ``False``.
    """
    if code not in ERROR_CODES:
        raise ValueError("unknown agent error code: %s" % code)
    if details is not None:
        raise ValueError("failure details are frozen to null in %s" % SCHEMA_VERSION)
    effective_retryable = http_status == 503
    return {
        "schema_version": SCHEMA_VERSION,
        "request_id": request_id,
        "trace_id": trace_id,
        "error": {
            "code": code,
            "message": message,
            "retryable": effective_retryable,
            "details": None,
        },
    }


def validate_response_envelope(payload: Any, http_status: int) -> None:
    """Fail closed when a response violates the frozen envelope contract.

    This check deliberately stays at the envelope boundary.  Domain payload
    fields are validated by their builders and by the T4 wire-value scanner;
    trying to infer the type of every nested user-supplied value here would
    reject legitimate task metadata.
    """
    if not isinstance(payload, dict):
        raise ValueError("response envelope must be an object")
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("response envelope has the wrong schema_version")

    request_id = payload.get("request_id")
    trace_id = payload.get("trace_id")
    if not isinstance(request_id, str) or not _REQUEST_ID_PATTERN.fullmatch(request_id):
        raise ValueError("response envelope has an invalid request_id")
    if not isinstance(trace_id, str) or not _TRACE_ID_PATTERN.fullmatch(trace_id):
        raise ValueError("response envelope has an invalid trace_id")

    has_data = "data" in payload
    has_error = "error" in payload
    if has_data == has_error:
        raise ValueError("response envelope must contain exactly one of data/error")

    if has_data:
        if set(payload) != {"schema_version", "request_id", "trace_id", "data"}:
            raise ValueError("success envelope has unexpected top-level fields")
        return

    if set(payload) != {"schema_version", "request_id", "trace_id", "error"}:
        raise ValueError("failure envelope has unexpected top-level fields")
    error = payload["error"]
    if not isinstance(error, dict):
        raise ValueError("failure envelope error must be an object")
    if set(error) != {"code", "message", "retryable", "details"}:
        raise ValueError("failure envelope error has unexpected fields")
    if error["code"] not in ERROR_CODES:
        raise ValueError("failure envelope uses an unknown error code")
    if not isinstance(error["message"], str):
        raise ValueError("failure envelope message must be a string")
    if not isinstance(error["retryable"], bool):
        raise ValueError("failure envelope retryable must be a boolean")
    if error["retryable"] != (http_status == 503):
        raise ValueError("failure envelope retryable must match HTTP 503")
    if error["details"] is not None:
        raise ValueError("failure envelope details must be null")
