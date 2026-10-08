"""Error codes and response-envelope helpers of the SigFlow Edu Agent.

Every response produced by the sidecar uses the same envelope shape as the
SigFlow EDA Gateway so that one UI client can consume both services:

* success: ``{"schema_version", "request_id", "trace_id", "data"}``
* failure: ``{"schema_version", "request_id", "trace_id", "error": {...}}``
"""

from __future__ import annotations

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
        self.retryable = retryable
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
) -> dict[str, Any]:
    """Build the failure envelope for one response.

    ``details`` stays ``None`` in v1; the field is kept for forward
    compatibility with the EDA Gateway envelope.
    """
    if code not in ERROR_CODES:
        raise ValueError("unknown agent error code: %s" % code)
    return {
        "schema_version": SCHEMA_VERSION,
        "request_id": request_id,
        "trace_id": trace_id,
        "error": {
            "code": code,
            "message": message,
            "retryable": bool(retryable),
            "details": details,
        },
    }
