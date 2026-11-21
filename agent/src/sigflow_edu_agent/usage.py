"""Bounded local accounting for optional model calls.

Only non-content counters are persisted: UTC timestamp, model name, success
flag and token counts.  Prompts, evidence, file paths, API keys and raw error
messages are never written here.

The store is best-effort: a missing/read-only data directory, a corrupt line or
an oversized file must never take the teaching service down.  Reads skip bad
lines and report how many were skipped; writes are append-only and bounded by a
retention window.
"""

from __future__ import annotations

import json
import logging
import os
import threading
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Mapping, Optional

LOGGER = logging.getLogger(__name__)

MAX_LINE_BYTES: int = 4096
"""A single usage record is tiny; anything larger is corrupt."""

MAX_FILE_BYTES: int = 8 * 1024 * 1024
"""Upper read/write budget for the append-only usage log."""

RETENTION_DAYS: int = 90
"""Records older than this are pruned on start and when the file grows."""

DEFAULT_USAGE_DAYS: int = 30
"""Default aggregation window of ``GET /api/v1/usage``."""

MAX_USAGE_DAYS: int = 90
"""Maximum aggregation window; matches the retention window."""

MAX_TOKEN_VALUE: int = 1_000_000_000
"""Sanity bound for a single token counter."""

USAGE_SCHEMA: str = "edu.usage.v1"
BALANCE_SCHEMA: str = "edu.model.balance.v1"


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _iso(value: datetime) -> str:
    return value.astimezone(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def _date_key(value: datetime) -> str:
    return value.astimezone(timezone.utc).strftime("%Y-%m-%d")


def _token_value(value: Any) -> Optional[int]:
    """Return a sane non-negative integer, or ``None`` for an unknown count."""
    if type(value) is not int or value < 0 or value > MAX_TOKEN_VALUE:
        return None
    return value


def _safe_model(value: Any) -> str:
    """Keep the model label ASCII-printable and bounded; never store user text."""
    if not isinstance(value, str) or not value:
        return "unknown"
    if len(value) > 128 or any(ord(character) < 33 or ord(character) > 126 for character in value):
        return "unknown"
    return value


def _empty_totals() -> dict[str, int]:
    return {
        "calls": 0,
        "failed_calls": 0,
        "prompt_tokens": 0,
        "completion_tokens": 0,
        "total_tokens": 0,
        "tokens_unavailable_calls": 0,
    }


def _empty_day(date: str) -> dict[str, Any]:
    return {
        "date": date,
        "calls": 0,
        "failed_calls": 0,
        "prompt_tokens": 0,
        "completion_tokens": 0,
        "total_tokens": 0,
    }


class UsageStore:
    """Append-only token counters stored under ``<data_directory>/usage``."""

    def __init__(
        self,
        data_directory: Optional[str],
        *,
        retention_days: int = RETENTION_DAYS,
        max_file_bytes: int = MAX_FILE_BYTES,
    ) -> None:
        self._lock = threading.Lock()
        self._retention_days = max(1, int(retention_days))
        self._max_file_bytes = max(MAX_LINE_BYTES, int(max_file_bytes))
        self._path: Optional[Path] = None
        if data_directory:
            try:
                directory = Path(data_directory) / "usage"
                directory.mkdir(parents=True, exist_ok=True)
                self._path = directory / "usage.jsonl"
                with self._lock:
                    self._prune_locked(_utc_now())
            except OSError:
                # A read-only or unavailable data directory disables accounting,
                # exactly like a missing model key disables model calls.
                self._path = None

    @property
    def enabled(self) -> bool:
        """Whether a usable log path was opened."""
        return self._path is not None

    def record(
        self,
        *,
        model: str,
        ok: bool,
        usage: Optional[Mapping[str, Any]] = None,
    ) -> None:
        """Append one model-call record; failures are silently ignored."""
        try:
            prompt = _token_value(usage.get("prompt_tokens")) if isinstance(usage, Mapping) else None
            completion = _token_value(usage.get("completion_tokens")) if isinstance(usage, Mapping) else None
            total = _token_value(usage.get("total_tokens")) if isinstance(usage, Mapping) else None
            unavailable = prompt is None or completion is None or total is None
            if unavailable:
                prompt = completion = total = 0
            record = {
                "ts": _iso(_utc_now()),
                "model": _safe_model(model),
                "ok": bool(ok),
                "prompt_tokens": prompt,
                "completion_tokens": completion,
                "total_tokens": total,
                "tokens_unavailable": unavailable,
            }
            line = json.dumps(record, ensure_ascii=True, separators=(",", ":")) + "\n"
            if len(line.encode("utf-8")) > MAX_LINE_BYTES:
                return
            with self._lock:
                path = self._path
                if path is None:
                    return
                try:
                    with path.open("a", encoding="ascii", newline="") as stream:
                        stream.write(line)
                        stream.flush()
                        os.fsync(stream.fileno())
                    if path.stat().st_size > self._max_file_bytes:
                        self._prune_locked(_utc_now())
                except OSError:
                    # Accounting is an enhancement; never fail a model call for it.
                    pass
        except Exception:  # noqa: BLE001 - accounting must never affect a model call
            LOGGER.debug("usage record failed", exc_info=True)

    def summarize(
        self,
        days: int = DEFAULT_USAGE_DAYS,
        *,
        now: Optional[datetime] = None,
    ) -> dict[str, Any]:
        """Aggregate retained records into totals plus a daily window."""
        window = max(1, min(MAX_USAGE_DAYS, int(days)))
        current = now or _utc_now()
        oldest_day = _date_key(current - timedelta(days=window - 1))
        today = _date_key(current)

        with self._lock:
            records, skipped = self._read_locked()

        totals = _empty_totals()
        daily: dict[str, dict[str, Any]] = {}
        since: Optional[str] = None
        for record in records:
            day = record["date"]
            if since is None or day < since:
                since = day
            totals["calls"] += 1
            if not record["ok"]:
                totals["failed_calls"] += 1
            if record["tokens_unavailable"]:
                totals["tokens_unavailable_calls"] += 1
            totals["prompt_tokens"] += record["prompt_tokens"]
            totals["completion_tokens"] += record["completion_tokens"]
            totals["total_tokens"] += record["total_tokens"]
            if oldest_day <= day <= today:
                bucket = daily.setdefault(day, _empty_day(day))
                bucket["calls"] += 1
                if not record["ok"]:
                    bucket["failed_calls"] += 1
                bucket["prompt_tokens"] += record["prompt_tokens"]
                bucket["completion_tokens"] += record["completion_tokens"]
                bucket["total_tokens"] += record["total_tokens"]

        return {
            "schema": USAGE_SCHEMA,
            "generated_at": _iso(current),
            "since": since,
            "days": window,
            "skipped": skipped,
            "totals": totals,
            "daily": [daily[date] for date in sorted(daily)],
        }

    # -- internals --------------------------------------------------------

    def _read_locked(self) -> tuple[list[dict[str, Any]], int]:
        path = self._path
        if path is None:
            return [], 0
        raw, _ = self._read_tail_locked(path)
        records: list[dict[str, Any]] = []
        skipped = 0
        for line in raw.splitlines():
            if not line.strip():
                continue
            parsed = self._parse_line(line)
            if parsed is None:
                skipped += 1
            else:
                records.append(parsed)
        return records, skipped

    def _read_tail_locked(self, path: Path) -> tuple[bytes, bool]:
        """Read the newest bounded tail; a truncated first line is dropped."""
        try:
            size = path.stat().st_size
            truncated = size > self._max_file_bytes
            with path.open("rb") as stream:
                if truncated:
                    stream.seek(size - self._max_file_bytes)
                raw = stream.read(self._max_file_bytes + 1)
        except OSError:
            return b"", False
        if truncated:
            newline = raw.find(b"\n")
            raw = raw[newline + 1:] if newline >= 0 else b""
        return raw, truncated

    @staticmethod
    def _parse_line(line: bytes) -> Optional[dict[str, Any]]:
        if len(line) > MAX_LINE_BYTES:
            return None
        try:
            record = json.loads(line.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None
        if not isinstance(record, dict):
            return None
        timestamp = record.get("ts")
        model = record.get("model")
        ok = record.get("ok")
        unavailable = record.get("tokens_unavailable")
        if not isinstance(timestamp, str) or not isinstance(model, str) or type(ok) is not bool:
            return None
        if type(unavailable) is not bool:
            return None
        try:
            parsed_time = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
        except ValueError:
            return None
        if parsed_time.tzinfo is None:
            return None
        prompt = _token_value(record.get("prompt_tokens"))
        completion = _token_value(record.get("completion_tokens"))
        total = _token_value(record.get("total_tokens"))
        if unavailable:
            prompt = completion = total = 0
        elif prompt is None or completion is None or total is None:
            return None
        return {
            "date": _date_key(parsed_time),
            "ok": ok,
            "prompt_tokens": prompt,
            "completion_tokens": completion,
            "total_tokens": total,
            "tokens_unavailable": unavailable,
        }

    def _prune_locked(self, now: datetime) -> None:
        """Drop malformed and expired lines, rewriting atomically when needed."""
        path = self._path
        if path is None:
            return
        raw, truncated = self._read_tail_locked(path)
        if not raw and not truncated:
            return
        cutoff = now - timedelta(days=self._retention_days)
        kept: list[bytes] = []
        changed = truncated
        for line in raw.splitlines():
            parsed = self._parse_line(line)
            if parsed is None or not line.strip():
                changed = True
                continue
            try:
                timestamp = datetime.fromisoformat(
                    json.loads(line.decode("utf-8"))["ts"].replace("Z", "+00:00")
                )
            except (ValueError, KeyError, UnicodeDecodeError):
                changed = True
                continue
            if timestamp < cutoff:
                changed = True
                continue
            kept.append(line)
        if not changed:
            return
        payload = b"\n".join(kept)
        if payload:
            payload += b"\n"
        temporary = path.with_suffix(path.suffix + ".tmp")
        try:
            with temporary.open("wb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        except OSError:
            try:
                temporary.unlink()
            except OSError:
                pass
