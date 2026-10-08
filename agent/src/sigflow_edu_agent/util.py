"""Small shared helpers: identifiers, redaction and timestamp formatting.

Nothing in this module performs I/O beyond reading the wall clock, and it
never logs.  Redaction is the single place where bootstrap tokens are turned
into a fixed placeholder so that no other module has to remember to do it.
"""

from __future__ import annotations

import json
import os
import re
from datetime import datetime, timezone
from typing import Any, Iterable, Optional

REDACTED: str = "***redacted***"
"""Replacement text for any secret value that would otherwise be rendered."""

_ABSOLUTE_PATH_PATTERNS: tuple[re.Pattern[str], ...] = (
    re.compile(r"[A-Za-z]:[\\/]"),
    re.compile(r"\\\\[^\\\s]+\\"),
    re.compile(r"(?<![\w.])/(?:home|tmp|var|usr|etc|root|opt|mnt|media)/"),
)
"""Patterns that identify a leaked local absolute path (Windows or POSIX)."""


def new_clock() -> datetime:
    """Return the current time as a timezone-aware UTC datetime."""
    return datetime.now(timezone.utc)


def iso_utc(moment: Optional[datetime] = None) -> str:
    """Format ``moment`` (default: now) as a UTC ISO 8601 second-resolution string."""
    value = moment or new_clock()
    return value.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def decimal(value: int) -> str:
    """Render a 64-bit counter as a decimal string for the wire contract."""
    return str(int(value))


def contains_absolute_path(text: str) -> bool:
    """Return ``True`` when ``text`` looks like it contains a local absolute path."""
    return any(pattern.search(text) for pattern in _ABSOLUTE_PATH_PATTERNS)


def redact(text: str, secrets: Iterable[str]) -> str:
    """Replace every non-empty secret in ``text`` with :data:`REDACTED`.

    Empty strings are ignored so that a missing optional secret cannot turn
    into a global string substitution.
    """
    result = text
    for secret in secrets:
        if secret:
            result = result.replace(secret, REDACTED)
    return result


def sanitize_message(text: str, secrets: Iterable[str]) -> str:
    """Redact secrets and absolute paths from a message bound for a client."""
    result = redact(text, secrets)
    for pattern in _ABSOLUTE_PATH_PATTERNS:
        result = pattern.sub("<path>", result)
    return result


class RedactionFilter:
    """Logging filter that removes bootstrap tokens from every log record.

    Instantiated by :mod:`sigflow_edu_agent.__main__` right after the bootstrap
    line has been parsed, so that no later diagnostic can leak a token.
    """

    def __init__(self, secrets: Iterable[str]) -> None:
        self._secrets = tuple(secret for secret in secrets if secret)

    def filter(self, record: "Any") -> bool:
        """Mutate the record in place so its rendered text carries no secret."""
        try:
            message = record.getMessage()
        except Exception:  # pragma: no cover - defensive
            return True
        redacted = redact(message, self._secrets)
        if redacted != message:
            record.msg = redacted
            record.args = ()
        return True


def json_dumps(payload: Any) -> str:
    """Serialize ``payload`` as UTF-8 friendly JSON on a single line."""
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def env_flag(name: str, default: bool = False) -> bool:
    """Read a boolean environment variable using a permissive truth table."""
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")
