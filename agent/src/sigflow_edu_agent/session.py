"""In-memory session, run and event store.

Everything here is deliberately process-local: v1 has no learner database and
no cross-restart persistence (documented as a known limitation).  The store is
guarded by a single lock because the HTTP server is threaded.

Caps are enforced rather than reported:

* at most :data:`MAX_SESSIONS` sessions, least recently used evicted first;
* at most :data:`MAX_CARDS_PER_RUN` cards per run (enforced by ``cards.py``);
* at most :data:`MAX_EVENTS_PER_SESSION` events per session, trimmed from the
  oldest end, which is what makes ``after`` cursors expire with 410.
"""

from __future__ import annotations

import threading
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any, Optional

from .util import decimal, iso_utc
from .version import MAX_EVENTS_PER_SESSION, MAX_SESSIONS


@dataclass
class RunRecord:
    """One completed (or failed) agent run."""

    run_id: str
    session_id: str
    kind: str
    level: str
    state: str
    state_version: int
    cards: list[dict[str, Any]] = field(default_factory=list)
    omitted: list[dict[str, str]] = field(default_factory=list)
    model_used: bool = False
    error_code: Optional[str] = None
    error_message: Optional[str] = None
    created_at: str = ""

    def to_wire(self) -> dict[str, Any]:
        """Render the run view returned by the run routes."""
        payload: dict[str, Any] = {
            "run_id": self.run_id,
            "session_id": self.session_id,
            "kind": self.kind,
            "level": self.level,
            "state": self.state,
            "state_version": decimal(self.state_version),
            "cards": self.cards,
            "omitted": self.omitted,
            "model_used": self.model_used,
        }
        if self.error_code:
            payload["error"] = {
                "code": self.error_code,
                "message": self.error_message or "",
            }
        return payload


@dataclass
class SessionRecord:
    """One teaching session bound to a project revision."""

    session_id: str
    project_id: str
    revision: str
    top: str
    target: Any
    locale: str
    state_version: int = 1
    created_at: str = ""
    last_used: float = 0.0
    runs: "OrderedDict[str, RunRecord]" = field(default_factory=OrderedDict)
    events: list[dict[str, Any]] = field(default_factory=list)
    sequence: int = 0

    def to_wire(self) -> dict[str, Any]:
        """Render the session summary returned by the session routes."""
        return {
            "session_id": self.session_id,
            "project_id": self.project_id,
            "revision": self.revision,
            "top": self.top,
            "target": self.target,
            "locale": self.locale,
            "state_version": decimal(self.state_version),
            "created_at": self.created_at,
            "run_count": len(self.runs),
            "event_count": len(self.events),
        }

    @property
    def oldest_sequence(self) -> int:
        """Sequence of the oldest retained event, or the current watermark."""
        if not self.events:
            return self.sequence
        return int(self.events[0]["sequence"])

    @property
    def high_watermark(self) -> int:
        """Highest sequence allocated for this session so far."""
        return self.sequence


class SessionStore:
    """Thread-safe LRU store of sessions, runs and events."""

    def __init__(self, max_sessions: int = MAX_SESSIONS) -> None:
        self._lock = threading.RLock()
        self._max_sessions = max(1, int(max_sessions))
        self._sessions: "OrderedDict[str, SessionRecord]" = OrderedDict()
        self._next_session = 0
        self._next_run = 0
        self._next_event = 0

    # -- sessions ---------------------------------------------------------

    def create_session(
        self,
        project_id: str,
        revision: str,
        top: str,
        target: Any,
        locale: str = "zh-CN",
    ) -> SessionRecord:
        """Create a session, evicting the least recently used one if needed."""
        with self._lock:
            self._next_session += 1
            session_id = "sess-%d" % self._next_session
            record = SessionRecord(
                session_id=session_id,
                project_id=project_id,
                revision=revision,
                top=top,
                target=target,
                locale=locale,
                created_at=iso_utc(),
                last_used=time.monotonic(),
            )
            self._sessions[session_id] = record
            self._evict_locked()
            return record

    def get_session(self, session_id: str) -> Optional[SessionRecord]:
        """Return a session and mark it as most recently used."""
        with self._lock:
            record = self._sessions.get(session_id)
            if record is None:
                return None
            record.last_used = time.monotonic()
            self._sessions.move_to_end(session_id)
            return record

    def _evict_locked(self) -> None:
        """Drop the least recently used sessions beyond the cap."""
        while len(self._sessions) > self._max_sessions:
            self._sessions.popitem(last=False)

    # -- runs -------------------------------------------------------------

    def create_run(
        self,
        session: SessionRecord,
        kind: str,
        level: str,
        state: str,
        cards: list[dict[str, Any]],
        omitted: list[dict[str, str]],
        model_used: bool,
        error_code: Optional[str] = None,
        error_message: Optional[str] = None,
    ) -> RunRecord:
        """Append a run to a session and return the stored record."""
        with self._lock:
            self._next_run += 1
            run_id = "run-%d" % self._next_run
            session.state_version += 1
            record = RunRecord(
                run_id=run_id,
                session_id=session.session_id,
                kind=kind,
                level=level,
                state=state,
                state_version=session.state_version,
                cards=cards,
                omitted=omitted,
                model_used=model_used,
                error_code=error_code,
                error_message=error_message,
                created_at=iso_utc(),
            )
            session.runs[run_id] = record
            while len(session.runs) > 64:
                session.runs.popitem(last=False)
            return record

    def get_run(self, run_id: str) -> Optional[RunRecord]:
        """Return a run by id, or ``None`` when the id is unknown."""
        with self._lock:
            for session in self._sessions.values():
                record = session.runs.get(run_id)
                if record is not None:
                    return record
            return None

    # -- events -----------------------------------------------------------

    def append_event(
        self,
        session: SessionRecord,
        event_type: str,
        data: dict[str, Any],
    ) -> dict[str, Any]:
        """Append one session event and return it."""
        with self._lock:
            self._next_event += 1
            session.sequence += 1
            event = {
                "sequence": decimal(session.sequence),
                "event_id": "ev-%d" % self._next_event,
                "type": event_type,
                "at": iso_utc(),
                "data": data,
            }
            session.events.append(event)
            if len(session.events) > MAX_EVENTS_PER_SESSION:
                del session.events[: len(session.events) - MAX_EVENTS_PER_SESSION]
            return event

    def read_events(
        self,
        session: SessionRecord,
        after: Optional[int],
        limit: int = 100,
    ) -> tuple[Optional[list[dict[str, Any]]], int, int]:
        """Return ``(events, high_watermark, oldest_sequence)`` for a cursor.

        ``events`` is ``None`` when the request must be answered with
        ``410 CURSOR_EXPIRED`` because the cursor fell out of the retention
        window.
        """
        with self._lock:
            high = session.sequence
            oldest = session.oldest_sequence
            cursor = 0 if after is None else int(after)
            if cursor > high:
                return None, high, oldest
            if after is not None and cursor < oldest - 1:
                return None, high, oldest
            events = [
                event
                for event in session.events
                if int(event["sequence"]) > cursor
            ]
            return events[:limit], high, oldest
