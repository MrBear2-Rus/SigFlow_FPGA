"""Parsing and validation of the one-line stdin bootstrap record.

The host (SigFlow's ``AgentServiceController``) writes exactly one UTF-8 JSON
line to the sidecar's stdin before anything else happens.  Any problem with
that line must be reported on stderr and terminate the process with exit code
2 while stdout stays completely empty, because the host treats any stdout byte
before a valid ready record as a protocol violation.

Bootstrap tokens are held only in memory for the lifetime of the process.
They must never be logged, echoed, persisted or returned to a client.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any, BinaryIO, Mapping, TextIO, Union

from .version import BOOTSTRAP_TYPE, PROTOCOL

MAX_BOOTSTRAP_LINE_BYTES: int = 64 * 1024
"""Reject an absurd stdin line instead of buffering unbounded input."""

_REQUIRED_TEXT_FIELDS: tuple[str, ...] = (
    "instance_id",
    "nonce",
    "gateway_url",
    "gateway_token",
    "ui_token",
    "data_directory",
)


class BootstrapError(Exception):
    """Raised when the bootstrap record is missing, malformed or inconsistent."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


@dataclass(frozen=True)
class Bootstrap:
    """Validated bootstrap record for one sidecar instance.

    The ``gateway_token`` and ``ui_token`` members are secrets: they are never
    rendered by :meth:`__repr__` and never serialized.
    """

    instance_id: str
    nonce: str
    gateway_url: str
    gateway_token: str = field(repr=False)
    ui_token: str = field(repr=False)
    data_directory: str = field(repr=False)

    @property
    def secrets(self) -> tuple[str, ...]:
        """Return the secrets that must be redacted from any output."""
        return (self.gateway_token, self.ui_token)


def parse_line(raw: str) -> Bootstrap:
    """Parse one bootstrap line and return it, or raise :class:`BootstrapError`."""
    text = raw.strip()
    if not text:
        raise BootstrapError("bootstrap line is empty")
    try:
        payload: Any = json.loads(text)
    except ValueError as exc:
        raise BootstrapError("bootstrap line is not valid JSON: %s" % exc) from exc
    if not isinstance(payload, Mapping):
        raise BootstrapError("bootstrap record must be a JSON object")
    return from_mapping(payload)


def from_mapping(payload: Mapping[str, Any]) -> Bootstrap:
    """Validate an already-decoded bootstrap mapping."""
    record_type = payload.get("type")
    if record_type != BOOTSTRAP_TYPE:
        raise BootstrapError(
            "bootstrap type must be %r but was %r" % (BOOTSTRAP_TYPE, record_type)
        )
    protocol = payload.get("protocol")
    if protocol != PROTOCOL:
        raise BootstrapError(
            "bootstrap protocol must be %r but was %r" % (PROTOCOL, protocol)
        )
    values: dict[str, str] = {}
    for name in _REQUIRED_TEXT_FIELDS:
        value = payload.get(name)
        if not isinstance(value, str) or not value.strip():
            raise BootstrapError("bootstrap field %r must be a non-empty string" % name)
        values[name] = value
    gateway_url = values["gateway_url"].rstrip("/")
    if not gateway_url.startswith(("http://", "https://")):
        raise BootstrapError("bootstrap gateway_url must be an http(s) URL")
    return Bootstrap(
        instance_id=values["instance_id"],
        nonce=values["nonce"],
        gateway_url=gateway_url,
        gateway_token=values["gateway_token"],
        ui_token=values["ui_token"],
        data_directory=values["data_directory"],
    )


def read_from_stream(
    stream: Union[TextIO, BinaryIO],
    limit: int = MAX_BOOTSTRAP_LINE_BYTES,
) -> Bootstrap:
    """Read exactly one line from ``stream`` and validate it.

    The host keeps the write end of the pipe open for the whole session, so a
    blocking read of one line is the correct primitive here: the line is
    terminated by ``\\n`` and no further input is expected on stdin.
    """
    line = stream.readline(limit + 1)
    if isinstance(line, bytes):
        if len(line) > limit:
            raise BootstrapError("bootstrap line exceeds %d bytes" % limit)
        try:
            text = line.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise BootstrapError("bootstrap line is not valid UTF-8: %s" % exc) from exc
    else:
        if len(line) > limit:
            raise BootstrapError("bootstrap line exceeds %d bytes" % limit)
        text = line
    if not text:
        raise BootstrapError("stdin closed before a bootstrap line was received")
    return parse_line(text)
