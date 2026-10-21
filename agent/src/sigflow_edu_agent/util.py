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
from typing import Any, Iterable, Mapping, Optional

REDACTED: str = "***redacted***"
"""Replacement text for any secret value that would otherwise be rendered."""

REDACTED_PATH: str = "***redacted-path***"
"""Stable placeholder for a local absolute path embedded in free text."""

_LOCAL_PATH_START: re.Pattern[str] = re.compile(
    r"(?<![A-Za-z0-9+.-])"
    r"(?:"
    r"[A-Za-z]:[\\/]"
    r"|\\\\[^\\/\s\"'<>|?*]+[\\/]"
    r"|(?<!:)//[^/\s\"'<>|?*]+/"
    r"|~[\\/]"
    r"|/(?i:Users|home|tmp|var|usr|etc|root|opt|mnt|media|srv|private|"
    r"Volumes|workspace|workspaces|run|proc|sys|dev|boot)"
    r"(?:/|(?=$|[\s\"',;:)\]]))"
    r")"
)
"""Start of a Windows drive, UNC, home-relative or known POSIX absolute path.

The leading look-behind keeps URL schemes such as ``http://`` from being
misread as a Windows path (``p:/``).  The POSIX branch is intentionally scoped
to well-known roots so ordinary URL paths such as ``/api/v1/jobs`` are not
rewritten.
"""

_PATH_TERMINATORS: frozenset[str] = frozenset("\r\n\t\"'<>|`")
_PATH_SENTENCE_END: frozenset[str] = frozenset("，。；、！？")
_PATH_EXTENSION: re.Pattern[str] = re.compile(
    r"\.[A-Za-z][A-Za-z0-9_+-]{0,15}[.,;:)\]}]*$"
)

_CREDENTIAL_PATTERNS: tuple[re.Pattern[str], ...] = (
    re.compile(r"\bsk-[A-Za-z0-9_-]{16,}"),
    re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]{20,}"),
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    re.compile(
        r"(?i)\b(?:api[_-]?key|access[_-]?key|client[_-]?secret|password|passwd|token)"
        r"\b\s*[:=]\s*[\"']?[A-Za-z0-9._~+/=-]{8,}"
    ),
)
"""Patterns that identify a credential *shape*, not just our own secrets.

A model can invent or echo someone else's key; matching only our configured
secrets would let that through.
"""


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
    return _LOCAL_PATH_START.search(text) is not None


def contains_credential(text: str, secrets: Iterable[str] = ()) -> bool:
    """Return ``True`` when *text* carries a credential.

    Two independent checks: the caller's own secret values (verbatim), and
    generic credential shapes such as ``sk-…``, ``Bearer …``, PEM headers or
    ``api_key=…`` assignments.  The shape check is what catches a credential
    the model invented or copied from somewhere else.
    """
    if any(secret and secret in text for secret in secrets):
        return True
    return any(pattern.search(text) for pattern in _CREDENTIAL_PATTERNS)


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
    return redact_local_paths(result)[0]


def _path_end(text: str, start: int) -> int:
    """Find the end of one path that starts at ``start``.

    Unquoted paths may contain spaces, so a whitespace alone cannot end the
    match.  We stop at a quoted/delimited boundary, at a second consecutive
    space, or after a filename extension when prose follows it.  This keeps
    common sentences readable without ever truncating a directory prefix.
    """
    index = start
    chunk_start = start
    depth = 0
    while index < len(text):
        char = text[index]
        if char in _PATH_TERMINATORS:
            break
        if char in _PATH_SENTENCE_END:
            break
        if char in ",;:" and (index + 1 == len(text) or text[index + 1].isspace()):
            break
        if char in "([{":
            depth += 1
        elif char in ")]}":
            if depth:
                depth -= 1
            else:
                break
        elif char.isspace():
            if index + 1 < len(text) and text[index + 1].isspace():
                break
            if _PATH_EXTENSION.search(text[chunk_start:index]):
                break
        if char in "\\/":
            chunk_start = index + 1
        index += 1
    while index > start and text[index - 1].isspace():
        index -= 1
    return index


def redact_local_paths(text: str) -> tuple[str, int]:
    """Replace every local absolute path in *text* with :data:`REDACTED_PATH`.

    Returns ``(redacted_text, replacement_count)``.  The walk is deterministic
    and only rewrites path-shaped spans; other prose and URL paths are left
    untouched.
    """
    pieces: list[str] = []
    cursor = 0
    replacements = 0
    for match in _LOCAL_PATH_START.finditer(text):
        if match.start() < cursor:
            continue
        end = _path_end(text, match.start())
        if end <= match.start():
            continue
        pieces.append(text[cursor:match.start()])
        pieces.append(REDACTED_PATH)
        cursor = end
        replacements += 1
    pieces.append(text[cursor:])
    return "".join(pieces), replacements


def redact_structure_paths(value: Any) -> tuple[Any, int]:
    """Deep-copy *value* while replacing local paths in every string field."""
    if isinstance(value, str):
        return redact_local_paths(value)
    if isinstance(value, Mapping):
        total = 0
        redacted: dict[Any, Any] = {}
        for key, item in value.items():
            cleaned, count = redact_structure_paths(item)
            redacted[key] = cleaned
            total += count
        return redacted, total
    if isinstance(value, list):
        total = 0
        items: list[Any] = []
        for item in value:
            cleaned, count = redact_structure_paths(item)
            items.append(cleaned)
            total += count
        return items, total
    if isinstance(value, tuple):
        total = 0
        items: list[Any] = []
        for item in value:
            cleaned, count = redact_structure_paths(item)
            items.append(cleaned)
            total += count
        return tuple(items), total
    return value, 0


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
        redacted = sanitize_message(message, self._secrets)
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
